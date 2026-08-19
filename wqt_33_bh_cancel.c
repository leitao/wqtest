// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_33_bh_cancel - cancelling and disabling BH work from atomic context.
 *
 * cancel_work_sync() and disable_work_sync() normally have to sleep waiting for
 * a running item, so they may only be called from a sleepable context.  A BH
 * work item runs in softirq and cannot sleep, so there is nothing to sleep on:
 * __flush_work() spots the WORK_OFFQ_BH tag the pool left in work->data and
 * busy-waits instead.  That is what lets the documented "can also be called
 * from non-hardirq atomic contexts including BH" hold, and it is what this test
 * drives -- once from a softirq-disabled section, once from inside a BH handler
 * cancelling the item queued behind it.
 *
 * The pool writes that tag when it releases the item -- when it finishes
 * running it, or when a cancel grabs it off the worklist -- so cancelling an
 * item that is pending right now carries its own proof that it is BH work.
 * Cancelling an *idle* one does not: it has to rely on the tag left behind by
 * an earlier run, which is why every work here is run once up front.  Take that
 * away and the idle cancel reaches might_sleep() in atomic context, which a
 * DEBUG_ATOMIC_SLEEP kernel reports as "BUG: sleeping function called from
 * invalid context" -- so the splat scan is a second oracle here, not just the
 * assertions.
 *
 * Softirqs are disabled around the pending-state checks because that is the
 * only way to hold a BH item pending: there is no max_active slot to occupy the
 * way wqt_05 does it on a normal workqueue.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/bottom_half.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include "wqtest.h"

#define SPIN_US		200

struct counted {
	struct work_struct work;
	atomic_t runs;
};

static struct counted cw, victim, dw, idle_item;
static struct work_struct canceller;

/* filled in by canceller_fn */
static bool nested_ret, nested_in_bh;

static void count_fn(struct work_struct *w)
{
	struct counted *c = container_of(w, struct counted, work);

	atomic_inc(&c->runs);
}

/* Runs in softirq and cancels the item queued behind it on the same pool. */
static void canceller_fn(struct work_struct *w)
{
	nested_in_bh = in_serving_softirq();
	nested_ret = cancel_work_sync(&victim.work);
}

static int __init wqt_33_init(void)
{
	bool pending_ret, idle_ret, sync_ret, disabled_queued, cold_idle_ret;
	struct workqueue_struct *wq;
	unsigned int busy;
	int ran_early;

	WQT_INIT(33, "bh_cancel");

	wq = alloc_workqueue("wqt33", WQ_BH | WQ_PERCPU, 0);
	if (!wq) {
		WQT_FAIL("alloc_workqueue(WQ_BH) failed");
		return WQT_FINISH();
	}

	INIT_WORK(&cw.work, count_fn);
	INIT_WORK(&victim.work, count_fn);
	INIT_WORK(&dw.work, count_fn);
	INIT_WORK(&idle_item.work, count_fn);
	INIT_WORK(&canceller, canceller_fn);

	/*
	 * Warm-up: one run on the BH pool each, so work->data carries
	 * WORK_OFFQ_BH even while the item is idle.
	 */
	atomic_set(&cw.runs, 0);
	atomic_set(&victim.runs, 0);
	atomic_set(&dw.runs, 0);
	atomic_set(&idle_item.runs, 0);
	queue_work(wq, &cw.work);
	queue_work(wq, &victim.work);
	queue_work(wq, &dw.work);
	queue_work(wq, &idle_item.work);
	flush_workqueue(wq);
	WQT_CHECK(atomic_read(&cw.runs) == 1 && atomic_read(&victim.runs) == 1 &&
		  atomic_read(&dw.runs) == 1 && atomic_read(&idle_item.runs) == 1,
		  "warm-up runs %d/%d/%d/%d, expected 1 each",
		  atomic_read(&cw.runs), atomic_read(&victim.runs),
		  atomic_read(&dw.runs), atomic_read(&idle_item.runs));

	/*
	 * 0: an item that is idle but was last on a BH pool.  Nothing in this
	 * call establishes that it is BH work -- only the tag its last run left
	 * behind -- so this is the case that needs the warm-up above.
	 */
	local_bh_disable();
	cold_idle_ret = cancel_work_sync(&idle_item.work);
	local_bh_enable();
	WQT_CHECK(!cold_idle_ret, "cancel of an idle BH item returned true");
	WQT_CHECK(atomic_read(&idle_item.runs) == 1,
		  "idle BH item ran during its own cancel (%d)",
		  atomic_read(&idle_item.runs));

	/* 1 & 2: cancel a pending, then an idle, BH item from atomic context. */
	atomic_set(&cw.runs, 0);
	local_bh_disable();
	queue_work(wq, &cw.work);
	pending_ret = cancel_work_sync(&cw.work);
	ran_early = atomic_read(&cw.runs);
	busy = work_busy(&cw.work);
	idle_ret = cancel_work_sync(&cw.work);
	local_bh_enable();
	udelay(SPIN_US);

	WQT_CHECK(pending_ret, "cancel of a pending BH item returned false");
	WQT_CHECK(ran_early == 0, "cancelled BH item ran anyway (%d)", ran_early);
	WQT_CHECK(!(busy & WORK_BUSY_PENDING),
		  "cancelled BH item still reported pending (busy=0x%x)", busy);
	WQT_CHECK(!idle_ret, "cancel of an idle BH item returned true");
	WQT_CHECK(atomic_read(&cw.runs) == 0,
		  "cancelled BH item ran after the gate opened (%d)",
		  atomic_read(&cw.runs));

	/* 3: the item is queueable again after a cancel. */
	queue_work(wq, &cw.work);
	flush_work(&cw.work);
	WQT_CHECK(atomic_read(&cw.runs) == 1,
		  "re-queued BH item ran %d times, expected 1",
		  atomic_read(&cw.runs));

	/* 4: cancel from inside a BH handler, the item queued behind it. */
	atomic_set(&victim.runs, 0);
	nested_ret = false;
	nested_in_bh = false;
	local_bh_disable();
	queue_work(wq, &canceller);
	queue_work(wq, &victim.work);
	local_bh_enable();
	flush_work(&canceller);

	WQT_CHECK(nested_in_bh, "canceller did not run in softirq context");
	WQT_CHECK(nested_ret,
		  "cancel from BH context returned false for a pending item");
	WQT_CHECK(atomic_read(&victim.runs) == 0,
		  "BH item cancelled from BH context still ran (%d)",
		  atomic_read(&victim.runs));

	/* 5: disable_work() depth, and enable_and_queue_work() at depth 0. */
	atomic_set(&dw.runs, 0);
	WQT_CHECK(!disable_work(&dw.work),
		  "disable_work of an idle item reported it pending");
	disabled_queued = queue_work(wq, &dw.work);
	WQT_CHECK(!disabled_queued, "queue_work accepted a disabled BH item");
	disable_work(&dw.work);				/* depth 2 */
	WQT_CHECK(!enable_work(&dw.work),
		  "enable_work reported depth 0 while still disabled");
	WQT_CHECK(!queue_work(wq, &dw.work),
		  "queue_work accepted a BH item still at disable depth 1");
	WQT_CHECK(enable_and_queue_work(wq, &dw.work),
		  "enable_and_queue_work did not queue at depth 0");
	flush_work(&dw.work);
	WQT_CHECK(atomic_read(&dw.runs) == 1,
		  "re-enabled BH item ran %d times, expected 1",
		  atomic_read(&dw.runs));

	/* 6: disable_work_sync() from a softirq-disabled section. */
	atomic_set(&dw.runs, 0);
	local_bh_disable();
	queue_work(wq, &dw.work);
	sync_ret = disable_work_sync(&dw.work);
	disabled_queued = queue_work(wq, &dw.work);
	local_bh_enable();
	udelay(SPIN_US);

	WQT_CHECK(sync_ret, "disable_work_sync returned false for a pending item");
	WQT_CHECK(!disabled_queued,
		  "queue_work accepted an item disabled by disable_work_sync");
	WQT_CHECK(atomic_read(&dw.runs) == 0,
		  "disabled BH item ran (%d)", atomic_read(&dw.runs));
	WQT_CHECK(enable_work(&dw.work), "enable_work did not reach depth 0");

	cancel_work_sync(&cw.work);
	cancel_work_sync(&victim.work);
	cancel_work_sync(&dw.work);
	cancel_work_sync(&idle_item.work);
	cancel_work_sync(&canceller);
	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_33_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: cancel/disable of WQ_BH work from atomic context");
MODULE_LICENSE("GPL");
