// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_25_nr_active_paths - max_active updates and cpu hotplug under load.
 *
 * An unbound workqueue keeps its nr_active budget per NUMA node, and that
 * budget is recomputed from workqueue_set_max_active() and from the cpu
 * hot[un]plug callbacks.  A per-cpu workqueue has no such budget and must stay
 * out of those paths.  Both are driven here with work in flight: raise and
 * lower max_active behind a parked item, then offline and online a cpu with a
 * batch queued to it.
 *
 * Work queued to a cpu that goes away must still run, and every item must be
 * accounted for at the end.  A workqueue that took the wrong nr_active path
 * shows up as a lost item, a stall caught by WQ_WATCHDOG, or an outright crash.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/wait.h>
#include "wqtest.h"

#define NR_FILL		16
#define HI_ACTIVE	4
#define WAIT_MS		5000

static struct work_struct blocker;
static struct work_struct fillers[NR_FILL];
static struct work_struct hp_items[NR_FILL];
static DECLARE_COMPLETION(blocker_running);
static DECLARE_COMPLETION(blocker_release);
static DECLARE_WAIT_QUEUE_HEAD(waitq);
static atomic_t done;
static int hp_ran_on[NR_FILL];

static void blocker_fn(struct work_struct *w)
{
	complete(&blocker_running);
	wait_for_completion_timeout(&blocker_release, msecs_to_jiffies(WAIT_MS));
}

static void fill_fn(struct work_struct *w)
{
	atomic_inc(&done);
	wake_up(&waitq);
}

static void hp_fn(struct work_struct *w)
{
	/* raw_: unbound workers are not pinned; see wqt_24. */
	hp_ran_on[w - hp_items] = raw_smp_processor_id();
	atomic_inc(&done);
	wake_up(&waitq);
}

static int __init wqt_25_init(void)
{
	static const struct {
		unsigned int flags;
		const char *name;
	} variants[] = {
		{ WQ_PERCPU,	"percpu" },
		{ WQ_UNBOUND,	"unbound" },
	};
	int v, i, ret, target, victim, ran;

	WQT_INIT(25, "nr_active_paths");

	target = cpumask_first(cpu_online_mask);
	victim = cpumask_next(target, cpu_online_mask);
	if (victim >= nr_cpu_ids)
		victim = -1;

	for (v = 0; v < ARRAY_SIZE(variants); v++) {
		const char *name = variants[v].name;
		struct workqueue_struct *wq;

		wq = alloc_workqueue("wqt25_%s", variants[v].flags, 1, name);
		if (!wq) {
			WQT_FAIL("%s: alloc_workqueue failed", name);
			continue;
		}

		/* --- max_active raised and lowered with a backlog queued --- */
		atomic_set(&done, 0);
		reinit_completion(&blocker_running);
		reinit_completion(&blocker_release);

		INIT_WORK(&blocker, blocker_fn);
		queue_work_on(target, wq, &blocker);
		if (!wait_for_completion_timeout(&blocker_running,
						 msecs_to_jiffies(WAIT_MS))) {
			WQT_FAIL("%s: blocker never started", name);
			complete(&blocker_release);
			destroy_workqueue(wq);
			continue;
		}

		for (i = 0; i < NR_FILL; i++) {
			INIT_WORK(&fillers[i], fill_fn);
			queue_work_on(target, wq, &fillers[i]);
		}

		workqueue_set_max_active(wq, HI_ACTIVE);
		workqueue_set_max_active(wq, 1);

		complete(&blocker_release);
		wait_event_timeout(waitq, atomic_read(&done) == NR_FILL,
				   msecs_to_jiffies(WAIT_MS));
		flush_workqueue(wq);

		ran = atomic_read(&done);
		WQT_CHECK(ran == NR_FILL,
			  "%s: %d/%d items ran across a max_active 1->%d->1 cycle",
			  name, ran, NR_FILL, HI_ACTIVE);

		/* --- the same wq across an offline/online of one cpu ------ */
		if (victim < 0 || !IS_ENABLED(CONFIG_HOTPLUG_CPU)) {
			destroy_workqueue(wq);
			continue;
		}

		atomic_set(&done, 0);
		for (i = 0; i < NR_FILL; i++) {
			hp_ran_on[i] = -1;
			INIT_WORK(&hp_items[i], hp_fn);
			queue_work_on(victim, wq, &hp_items[i]);
		}

		ret = remove_cpu(victim);
		if (ret) {
			WQT_DIAG("%s: remove_cpu(%d) returned %d, hotplug phase skipped",
				 name, victim, ret);
			wait_event_timeout(waitq, atomic_read(&done) == NR_FILL,
					   msecs_to_jiffies(WAIT_MS));
			flush_workqueue(wq);
			destroy_workqueue(wq);
			victim = -1;
			continue;
		}

		wait_event_timeout(waitq, atomic_read(&done) == NR_FILL,
				   msecs_to_jiffies(WAIT_MS));
		flush_workqueue(wq);

		ran = atomic_read(&done);
		WQT_CHECK(ran == NR_FILL,
			  "%s: %d/%d items queued to cpu%d ran after it went offline",
			  name, ran, NR_FILL, victim);

		ret = add_cpu(victim);
		WQT_CHECK(ret == 0, "%s: add_cpu(%d) failed: %d", name, victim,
			  ret);
		if (ret) {
			destroy_workqueue(wq);
			victim = -1;
			continue;
		}

		/* The re-onlined cpu takes work again, on its own pwq. */
		atomic_set(&done, 0);
		hp_ran_on[0] = -1;
		INIT_WORK(&hp_items[0], hp_fn);
		queue_work_on(victim, wq, &hp_items[0]);
		wait_event_timeout(waitq, atomic_read(&done) == 1,
				   msecs_to_jiffies(WAIT_MS));
		flush_workqueue(wq);

		WQT_CHECK(atomic_read(&done) == 1,
			  "%s: item queued to re-onlined cpu%d never ran", name,
			  victim);
		if (!(variants[v].flags & WQ_UNBOUND))
			WQT_CHECK(hp_ran_on[0] == victim,
				  "%s: item for re-onlined cpu%d ran on cpu%d",
				  name, victim, hp_ran_on[0]);

		WQT_DIAG("%s: survived max_active churn and an offline/online of cpu%d",
			 name, victim);

		destroy_workqueue(wq);
	}

	if (victim < 0)
		WQT_DIAG("no offlineable cpu: hotplug phase skipped");

	return WQT_FINISH();
}
module_init(wqt_25_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: max_active updates and cpu hotplug under load");
MODULE_LICENSE("GPL");
