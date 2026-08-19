// SPDX-License-Identifier: GPL-2.0
/*
 * wqh_sysfs - workqueues for the userspace sysfs tests to poke at.
 *
 * Not a test.  The workqueue sysfs interface only exposes workqueues created
 * with WQ_SYSFS, and only for as long as they exist, so the tests that drive it
 * from userspace need something to drive.  This module creates one workqueue
 * per interesting shape, holds them, and unlike the wqt_* modules stays loaded
 * until it is removed:
 *
 *	wqh_percpu	WQ_PERCPU, max_active 8	  no unbound attrs
 *	wqh_unbound	WQ_UNBOUND, max_active 8  the main subject
 *	wqh_ordered	ordered			  max_active is read-only
 *	wqh_hipri	WQ_UNBOUND | WQ_HIGHPRI	  starts at nice -20
 *	wqh_hidden	WQ_PERCPU, no WQ_SYSFS	  must not appear in sysfs
 *
 * WQ_BH is missing on purpose: __WQ_BH_ALLOWS permits only WQ_HIGHPRI and
 * WQ_PERCPU alongside WQ_BH, so alloc_workqueue() rejects WQ_BH | WQ_SYSFS and
 * a BH workqueue can never show up under /sys/devices/virtual/workqueue.
 *
 * Reading an attribute back only proves the store handler parsed it.  To show
 * that it reached the workers, writing a workqueue name to the "probe"
 * parameter runs a batch of items on that workqueue and records the cpus they
 * ran on and the nice they ran at; reading the parameter reports them:
 *
 *	echo wqh_unbound > /sys/module/wqh_sysfs/parameters/probe
 *	cat /sys/module/wqh_sysfs/parameters/probe
 *	wq=wqh_unbound ran=64 cpus=2 nice=-5..-5
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait.h>

#define NR_PROBE	64
#define WAIT_MS		5000

static struct {
	const char *name;
	unsigned int flags;
	int max_active;
	bool ordered;
	struct workqueue_struct *wq;
} wqs[] = {
	{ "wqh_percpu",	 WQ_SYSFS | WQ_PERCPU,			8, false },
	{ "wqh_unbound", WQ_SYSFS | WQ_UNBOUND,			8, false },
	{ "wqh_ordered", WQ_SYSFS,				0, true  },
	{ "wqh_hipri",	 WQ_SYSFS | WQ_UNBOUND | WQ_HIGHPRI,	4, false },
	{ "wqh_hidden",	 WQ_PERCPU,				8, false },
};

struct probe_item {
	struct work_struct work;
};

static struct probe_item *pitems;
static cpumask_var_t probe_cpus;
static DEFINE_SPINLOCK(probe_lock);
static DECLARE_WAIT_QUEUE_HEAD(probe_waitq);
static atomic_t probe_ran;
static int probe_idx = -1;
static int probe_nice_lo, probe_nice_hi;

static void probe_fn(struct work_struct *w)
{
	unsigned long flags;
	int nice;

	/* raw_: an unbound worker is not pinned, see wqt_24. */
	nice = task_nice(current);

	spin_lock_irqsave(&probe_lock, flags);
	cpumask_set_cpu(raw_smp_processor_id(), probe_cpus);
	probe_nice_lo = min(probe_nice_lo, nice);
	probe_nice_hi = max(probe_nice_hi, nice);
	spin_unlock_irqrestore(&probe_lock, flags);

	atomic_inc(&probe_ran);
	wake_up(&probe_waitq);
}

/* Run a batch on the named workqueue and remember where and how it ran. */
static int probe_set(const char *val, const struct kernel_param *kp)
{
	char name[32];
	int i, idx = -1;

	if (sscanf(val, "%31s", name) != 1)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(wqs); i++) {
		if (!strcmp(name, wqs[i].name)) {
			idx = i;
			break;
		}
	}
	if (idx < 0)
		return -EINVAL;
	if (!wqs[idx].wq)
		return -ENODEV;

	cpumask_clear(probe_cpus);
	probe_nice_lo = MAX_NICE;
	probe_nice_hi = MIN_NICE;
	atomic_set(&probe_ran, 0);

	for (i = 0; i < NR_PROBE; i++)
		queue_work(wqs[idx].wq, &pitems[i].work);

	wait_event_timeout(probe_waitq, atomic_read(&probe_ran) == NR_PROBE,
			   msecs_to_jiffies(WAIT_MS));
	flush_workqueue(wqs[idx].wq);
	probe_idx = idx;

	return 0;
}

static int probe_get(char *buf, const struct kernel_param *kp)
{
	if (probe_idx < 0)
		return scnprintf(buf, PAGE_SIZE, "none\n");

	return scnprintf(buf, PAGE_SIZE, "wq=%s ran=%d cpus=%*pbl nice=%d..%d\n",
			 wqs[probe_idx].name, atomic_read(&probe_ran),
			 cpumask_pr_args(probe_cpus), probe_nice_lo,
			 probe_nice_hi);
}

static const struct kernel_param_ops probe_ops = {
	.set = probe_set,
	.get = probe_get,
};
module_param_cb(probe, &probe_ops, NULL, 0644);
MODULE_PARM_DESC(probe, "write a workqueue name to run a batch on it and record where it ran");

static void wqh_destroy(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(wqs); i++) {
		if (wqs[i].wq) {
			destroy_workqueue(wqs[i].wq);
			wqs[i].wq = NULL;
		}
	}
}

static int __init wqh_sysfs_init(void)
{
	int i;

	if (!zalloc_cpumask_var(&probe_cpus, GFP_KERNEL))
		return -ENOMEM;

	pitems = kcalloc(NR_PROBE, sizeof(*pitems), GFP_KERNEL);
	if (!pitems) {
		free_cpumask_var(probe_cpus);
		return -ENOMEM;
	}
	for (i = 0; i < NR_PROBE; i++)
		INIT_WORK(&pitems[i].work, probe_fn);

	for (i = 0; i < ARRAY_SIZE(wqs); i++) {
		if (wqs[i].ordered)
			wqs[i].wq = alloc_ordered_workqueue("%s", wqs[i].flags,
							    wqs[i].name);
		else
			wqs[i].wq = alloc_workqueue("%s", wqs[i].flags,
						    wqs[i].max_active,
						    wqs[i].name);
		if (!wqs[i].wq) {
			pr_err("wqh_sysfs: alloc_workqueue(%s) failed\n",
			       wqs[i].name);
			wqh_destroy();
			kfree(pitems);
			free_cpumask_var(probe_cpus);
			return -ENOMEM;
		}
	}

	pr_info("wqh_sysfs: %zu workqueues created\n", ARRAY_SIZE(wqs));

	return 0;
}

static void __exit wqh_sysfs_exit(void)
{
	wqh_destroy();
	kfree(pitems);
	free_cpumask_var(probe_cpus);
}

module_init(wqh_sysfs_init);
module_exit(wqh_sysfs_exit);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest helper: WQ_SYSFS workqueues for the userspace tests");
MODULE_LICENSE("GPL");
