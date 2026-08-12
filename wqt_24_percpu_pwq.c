// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_24_percpu_pwq - per-cpu pool_workqueue install and teardown.
 *
 * A per-cpu workqueue gets one pool_workqueue per cpu, each bound to that cpu's
 * static worker pool: the normal pool, the highpri one, or the BH one when
 * WQ_BH is set.  Queue one item per online cpu on each of those, and on an
 * unbound wq for contrast, then check the item ran where it was queued.
 *
 * The second half churns short-lived per-cpu workqueues so the install and
 * teardown of those per-cpu slots runs many times over; KASAN and
 * debugobjects are the oracle for anything the assertions cannot see.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include "wqtest.h"

#define NR_CHURN	32
#define WAIT_MS		5000

struct cpu_item {
	struct work_struct work;
	int ran_on;
};

static struct cpu_item *items, *bh_items;
static struct work_struct churn_work;
static atomic_t ran;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void cpu_fn(struct work_struct *w)
{
	struct cpu_item *it = container_of(w, struct cpu_item, work);

	/*
	 * raw_: an unbound worker is not pinned, so plain smp_processor_id()
	 * would warn under DEBUG_PREEMPT.  A per-cpu worker is pinned, which
	 * is what makes the reading exact for the variants asserted below.
	 */
	it->ran_on = raw_smp_processor_id();
	atomic_inc(&ran);
	wake_up(&waitq);
}

static void churn_fn(struct work_struct *w)
{
	atomic_inc(&ran);
}

static int __init wqt_24_init(void)
{
	static const struct {
		unsigned int flags;
		const char *name;
	} variants[] = {
		{ WQ_PERCPU,			"percpu" },
		{ WQ_PERCPU | WQ_HIGHPRI,	"percpu_highpri" },
		{ WQ_BH | WQ_PERCPU,		"bh" },
		{ WQ_UNBOUND,			"unbound" },
	};
	int v, i, cpu, n, mism;

	WQT_INIT(24, "percpu_pwq");

	items = kcalloc(nr_cpu_ids, sizeof(*items), GFP_KERNEL);
	bh_items = kcalloc(nr_cpu_ids, sizeof(*bh_items), GFP_KERNEL);
	if (!items || !bh_items) {
		WQT_FAIL("kcalloc failed");
		goto out;
	}

	/*
	 * Two arrays, initialised at two call sites: a BH workqueue runs its
	 * items in softirq context while the other variants run theirs in task
	 * context, and INIT_WORK() takes the lockdep class from its call site.
	 * One item shared between them would be reported as inconsistent
	 * softirq usage.
	 */
	for_each_possible_cpu(cpu)
		INIT_WORK(&items[cpu].work, cpu_fn);
	for_each_possible_cpu(cpu)
		INIT_WORK(&bh_items[cpu].work, cpu_fn);

	for (v = 0; v < ARRAY_SIZE(variants); v++) {
		struct cpu_item *set = (variants[v].flags & WQ_BH) ? bh_items
								   : items;
		const char *name = variants[v].name;
		struct workqueue_struct *wq;

		wq = alloc_workqueue("wqt24_%s", variants[v].flags, 0, name);
		if (!wq) {
			WQT_FAIL("%s: alloc_workqueue failed", name);
			continue;
		}

		atomic_set(&ran, 0);
		n = 0;
		for_each_online_cpu(cpu) {
			set[cpu].ran_on = -1;
			WQT_CHECK(queue_work_on(cpu, wq, &set[cpu].work),
				  "%s: queue_work_on(cpu=%d) rejected", name,
				  cpu);
			n++;
		}

		wait_event_timeout(waitq, atomic_read(&ran) == n,
				   msecs_to_jiffies(WAIT_MS));
		flush_workqueue(wq);

		WQT_CHECK(atomic_read(&ran) == n, "%s: %d/%d items ran", name,
			  atomic_read(&ran), n);

		/* Only a per-cpu pwq guarantees where the item runs. */
		if (!(variants[v].flags & WQ_UNBOUND)) {
			mism = 0;
			for_each_online_cpu(cpu)
				if (set[cpu].ran_on != cpu) {
					mism++;
					WQT_DIAG("%s: cpu%d item ran on cpu%d",
						 name, cpu, set[cpu].ran_on);
				}
			WQT_CHECK(mism == 0, "%s: %d/%d items ran on the wrong cpu",
				  name, mism, n);
		}

		destroy_workqueue(wq);
	}

	WQT_DIAG("covered %zu workqueue variants over %d online cpus",
		 ARRAY_SIZE(variants), n);

	/* Churn: allocate, use and free per-cpu pwq sets back to back. */
	atomic_set(&ran, 0);
	cpu = cpumask_first(cpu_online_mask);
	for (i = 0; i < NR_CHURN; i++) {
		struct workqueue_struct *wq;

		wq = alloc_workqueue("wqt24_churn%d", WQ_PERCPU, 0, i);
		if (!wq) {
			WQT_FAIL("churn: alloc_workqueue failed at #%d", i);
			break;
		}

		INIT_WORK(&churn_work, churn_fn);
		queue_work_on(cpu, wq, &churn_work);
		flush_workqueue(wq);
		destroy_workqueue(wq);

		cpu = cpumask_next(cpu, cpu_online_mask);
		if (cpu >= nr_cpu_ids)
			cpu = cpumask_first(cpu_online_mask);
	}
	WQT_CHECK(atomic_read(&ran) == NR_CHURN,
		  "churn: %d/%d items ran across short-lived workqueues",
		  atomic_read(&ran), NR_CHURN);

out:
	kfree(bh_items);
	kfree(items);

	return WQT_FINISH();
}
module_init(wqt_24_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: per-cpu pool_workqueue install/teardown");
MODULE_LICENSE("GPL");
