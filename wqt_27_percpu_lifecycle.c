// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_27_percpu_lifecycle - repeated create and destroy of per-cpu workqueues.
 *
 * Building a workqueue means allocating a pool_workqueue for every possible
 * cpu, linking each to its pool and installing it; tearing one down releases
 * them again through pwq_release_workfn(), which has to tell a refcounted
 * unbound pool from a static per-cpu one.  wqt_24 churns plain WQ_PERCPU
 * queues; this covers the highpri and BH variants too, and destroys some of
 * them with work still queued so the drain path runs rather than the empty
 * one.
 *
 * Nothing here asserts much beyond "every item ran".  The point is volume
 * through the install and release paths with KASAN, debugobjects and lockdep
 * watching: a pwq freed twice, a pool refcount driven below zero, or a leaked
 * pwq shows up there and not in an assertion.
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

#define NR_ROUNDS	24
#define NR_ITEMS	8
#define WAIT_MS		5000

static unsigned int rounds = NR_ROUNDS;
module_param(rounds, uint, 0444);
MODULE_PARM_DESC(rounds, "create/destroy rounds per workqueue variant");

static struct work_struct items[NR_ITEMS];
static struct work_struct bh_items[NR_ITEMS];
static atomic_t ran;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void item_fn(struct work_struct *w)
{
	atomic_inc(&ran);
	wake_up(&waitq);
}

static int __init wqt_27_init(void)
{
	static const struct {
		unsigned int flags;
		const char *name;
		bool bh;
	} variants[] = {
		{ WQ_PERCPU,				"percpu",	false },
		{ WQ_PERCPU | WQ_HIGHPRI,		"percpu_highpri", false },
		{ WQ_PERCPU | WQ_FREEZABLE,		"percpu_freezable", false },
		{ WQ_BH | WQ_PERCPU,			"bh",		true },
		{ WQ_BH | WQ_HIGHPRI | WQ_PERCPU,	"bh_highpri",	true },
		{ WQ_UNBOUND,				"unbound",	false },
	};
	unsigned int r;
	int v, i, cpu, expect;

	WQT_INIT(27, "percpu_lifecycle");

	for (i = 0; i < NR_ITEMS; i++)
		INIT_WORK(&items[i], item_fn);
	for (i = 0; i < NR_ITEMS; i++)
		INIT_WORK(&bh_items[i], item_fn);

	for (v = 0; v < ARRAY_SIZE(variants); v++) {
		struct work_struct *set = variants[v].bh ? bh_items : items;
		const char *name = variants[v].name;
		int failed_alloc = 0;

		atomic_set(&ran, 0);
		expect = 0;

		for (r = 0; r < rounds; r++) {
			struct workqueue_struct *wq;

			wq = alloc_workqueue("wqt27_%s%u", variants[v].flags,
					     0, name, r);
			if (!wq) {
				failed_alloc++;
				break;
			}

			/*
			 * Spread the items over cpus so more than one per-cpu
			 * pwq of this queue is used before it goes away.
			 */
			cpu = cpumask_first(cpu_online_mask);
			for (i = 0; i < NR_ITEMS; i++) {
				if (queue_work_on(cpu, wq, &set[i]))
					expect++;

				cpu = cpumask_next(cpu, cpu_online_mask);
				if (cpu >= nr_cpu_ids)
					cpu = cpumask_first(cpu_online_mask);
			}

			/*
			 * Alternate between draining first and letting
			 * destroy_workqueue() do it with work still queued.
			 */
			if (r & 1)
				flush_workqueue(wq);

			destroy_workqueue(wq);
		}

		WQT_CHECK(failed_alloc == 0, "%s: alloc_workqueue failed", name);

		wait_event_timeout(waitq, atomic_read(&ran) == expect,
				   msecs_to_jiffies(WAIT_MS));
		WQT_CHECK(atomic_read(&ran) == expect,
			  "%s: %d/%d items ran over %u rounds", name,
			  atomic_read(&ran), expect, rounds);
	}

	WQT_DIAG("churned %zu variants for %u rounds each",
		 ARRAY_SIZE(variants), rounds);

	return WQT_FINISH();
}
module_init(wqt_27_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: per-cpu workqueue create/destroy churn");
MODULE_LICENSE("GPL");
