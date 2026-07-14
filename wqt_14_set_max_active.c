// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_14_set_max_active - dynamic workqueue_set_max_active().
 *
 * workqueue_set_max_active() changes a live workqueue's concurrency cap.  A wq
 * created with max_active=1 must serialise (peak concurrency 1); after raising
 * it to N the pool must be able to run more items at once (peak > 1, never > N);
 * after lowering it back to 1 it must serialise again.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include "wqtest.h"

#define NR_ITEMS 16
#define HI_ACTIVE 4

static struct work_struct items[NR_ITEMS];
static atomic_t live, max_live, executed;

static void ma_fn(struct work_struct *w)
{
	int cur, old;

	cur = atomic_inc_return(&live);
	old = atomic_read(&max_live);
	while (cur > old) {
		int prev = atomic_cmpxchg(&max_live, old, cur);

		if (prev == old)
			break;
		old = prev;
	}
	msleep(20);
	atomic_inc(&executed);
	atomic_dec(&live);
}

/* Run a batch through @wq and return the peak observed concurrency. */
static int run_batch(struct workqueue_struct *wq)
{
	int i;

	atomic_set(&live, 0);
	atomic_set(&max_live, 0);
	atomic_set(&executed, 0);

	for (i = 0; i < NR_ITEMS; i++) {
		INIT_WORK(&items[i], ma_fn);
		queue_work(wq, &items[i]);
	}
	flush_workqueue(wq);
	return atomic_read(&max_live);
}

static int __init wqt_14_init(void)
{
	struct workqueue_struct *wq;
	int peak;

	WQT_INIT(14, "set_max_active");

	wq = alloc_workqueue("wqt14", WQ_UNBOUND, 1);
	if (!wq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}

	/* 1: max_active=1 serialises. */
	peak = run_batch(wq);
	WQT_CHECK(peak == 1, "max_active=1 but peak concurrency=%d", peak);
	WQT_CHECK(atomic_read(&executed) == NR_ITEMS,
		  "max_active=1: executed=%d expected=%d",
		  atomic_read(&executed), NR_ITEMS);

	/* 2: raising the cap allows more concurrency, still bounded by it. */
	workqueue_set_max_active(wq, HI_ACTIVE);
	peak = run_batch(wq);
	WQT_CHECK(peak > 1, "raised max_active to %d but peak stayed at %d",
		  HI_ACTIVE, peak);
	WQT_CHECK(peak <= HI_ACTIVE, "peak concurrency %d exceeds max_active=%d",
		  peak, HI_ACTIVE);
	WQT_CHECK(atomic_read(&executed) == NR_ITEMS,
		  "max_active=%d: executed=%d expected=%d", HI_ACTIVE,
		  atomic_read(&executed), NR_ITEMS);
	WQT_DIAG("peak concurrency %d after set_max_active(%d)", peak, HI_ACTIVE);

	/* 3: lowering the cap serialises again. */
	workqueue_set_max_active(wq, 1);
	peak = run_batch(wq);
	WQT_CHECK(peak == 1, "lowered max_active to 1 but peak concurrency=%d",
		  peak);

	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_14_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: dynamic workqueue_set_max_active");
MODULE_LICENSE("GPL");
