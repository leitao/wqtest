// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_01_basic - basic queue / flush correctness.
 *
 * Queues a batch of distinct work items on a per-cpu workqueue, an unbound
 * workqueue and the system workqueue (via schedule_work), and after flushing
 * verifies that every item ran exactly once and nothing is left pending.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include "wqtest.h"

#define NR_ITEMS 64

struct basic_item {
	struct work_struct work;
	atomic_t runs;
};

static struct basic_item items[NR_ITEMS];
static atomic_t total_runs;

static void basic_fn(struct work_struct *w)
{
	struct basic_item *it = container_of(w, struct basic_item, work);

	atomic_inc(&it->runs);
	atomic_inc(&total_runs);
}

static void reset_items(void)
{
	int i;

	atomic_set(&total_runs, 0);
	for (i = 0; i < NR_ITEMS; i++)
		atomic_set(&items[i].runs, 0);
}

/* Verify the just-flushed batch; expands where __wqt is in scope. */
#define VERIFY(label)							\
do {									\
	int i, bad = 0, pend = 0;					\
	for (i = 0; i < NR_ITEMS; i++) {				\
		if (atomic_read(&items[i].runs) != 1)			\
			bad++;						\
		if (work_pending(&items[i].work))			\
			pend++;						\
	}								\
	WQT_CHECK(bad == 0, "%s: %d/%d items did not run exactly once",	\
		  label, bad, NR_ITEMS);				\
	WQT_CHECK(atomic_read(&total_runs) == NR_ITEMS,			\
		  "%s: total_runs=%d expected=%d", label,		\
		  atomic_read(&total_runs), NR_ITEMS);			\
	WQT_CHECK(pend == 0, "%s: %d items still pending after flush",	\
		  label, pend);						\
} while (0)

static int __init wqt_01_init(void)
{
	struct workqueue_struct *pcpu_wq, *unbound_wq;
	int i;

	WQT_INIT(1, "basic");

	for (i = 0; i < NR_ITEMS; i++)
		INIT_WORK(&items[i].work, basic_fn);

	pcpu_wq = alloc_workqueue("wqt01_pcpu", WQ_PERCPU, 0);
	unbound_wq = alloc_workqueue("wqt01_unb", WQ_UNBOUND, 0);
	if (!pcpu_wq || !unbound_wq) {
		WQT_FAIL("alloc_workqueue failed (pcpu=%p unbound=%p)",
			 pcpu_wq, unbound_wq);
		goto out;
	}

	/* Per-cpu workqueue. */
	reset_items();
	for (i = 0; i < NR_ITEMS; i++)
		WQT_CHECK(queue_work(pcpu_wq, &items[i].work),
			  "percpu: queue_work rejected item %d", i);
	flush_workqueue(pcpu_wq);
	VERIFY("percpu");

	/* Unbound workqueue. */
	reset_items();
	for (i = 0; i < NR_ITEMS; i++)
		queue_work(unbound_wq, &items[i].work);
	flush_workqueue(unbound_wq);
	VERIFY("unbound");

	/* System workqueue via schedule_work(). */
	reset_items();
	for (i = 0; i < NR_ITEMS; i++)
		schedule_work(&items[i].work);
	for (i = 0; i < NR_ITEMS; i++)
		flush_work(&items[i].work);
	VERIFY("system_wq");

out:
	if (pcpu_wq)
		destroy_workqueue(pcpu_wq);
	if (unbound_wq)
		destroy_workqueue(unbound_wq);

	return WQT_FINISH();
}
module_init(wqt_01_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: basic queue/flush exactly-once");
MODULE_LICENSE("GPL");
