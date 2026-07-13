// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_02_ordered - ordered workqueue FIFO / non-overlap guarantee.
 *
 * An ordered workqueue (alloc_ordered_workqueue) must run its work items one
 * at a time and in submission order.  Each item records the order in which it
 * executed and flags any overlap; we verify strict FIFO and that no two items
 * ran concurrently.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include "wqtest.h"

#define NR_ITEMS 32

struct ord_item {
	struct work_struct work;
	int seq;
};

static struct ord_item items[NR_ITEMS];
static int exec_order[NR_ITEMS];
static atomic_t exec_idx;
static atomic_t live;
static atomic_t overlap;

static void ord_fn(struct work_struct *w)
{
	struct ord_item *it = container_of(w, struct ord_item, work);
	int slot;

	if (atomic_inc_return(&live) != 1)
		atomic_set(&overlap, 1);

	slot = atomic_inc_return(&exec_idx) - 1;
	if (slot >= 0 && slot < NR_ITEMS)
		exec_order[slot] = it->seq;

	/* Widen the window so any concurrent execution is observable. */
	udelay(50);

	atomic_dec(&live);
}

static int __init wqt_02_init(void)
{
	struct workqueue_struct *wq;
	int i, misordered = 0, executed;

	WQT_INIT(2, "ordered");

	atomic_set(&exec_idx, 0);
	atomic_set(&live, 0);
	atomic_set(&overlap, 0);

	wq = alloc_ordered_workqueue("wqt02_ord", 0);
	if (!wq) {
		WQT_FAIL("alloc_ordered_workqueue failed");
		return WQT_FINISH();
	}

	for (i = 0; i < NR_ITEMS; i++) {
		INIT_WORK(&items[i].work, ord_fn);
		items[i].seq = i;
		exec_order[i] = -1;
	}

	for (i = 0; i < NR_ITEMS; i++)
		queue_work(wq, &items[i].work);

	flush_workqueue(wq);

	executed = atomic_read(&exec_idx);
	WQT_CHECK(executed == NR_ITEMS, "executed=%d expected=%d",
		  executed, NR_ITEMS);

	for (i = 0; i < NR_ITEMS && i < executed; i++)
		if (exec_order[i] != i)
			misordered++;
	WQT_CHECK(misordered == 0, "%d items executed out of FIFO order",
		  misordered);

	WQT_CHECK(atomic_read(&overlap) == 0,
		  "ordered workqueue ran items concurrently");

	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_02_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: ordered workqueue FIFO/non-overlap");
MODULE_LICENSE("GPL");
