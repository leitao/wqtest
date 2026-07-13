// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_05_cancel - cancel_work_sync() semantics.
 *
 * Verifies the documented return values and behaviour of cancel_work_sync():
 *   - cancelling a *pending* (queued, not yet started) item returns true and
 *     the item never runs;
 *   - cancelling a *running* item returns false but waits for it to finish;
 *   - cancelling an *idle* item returns false;
 *   - an item can be re-queued after being cancelled;
 * and that no item ever runs more than once.
 *
 * A max_active=1 workqueue plus a blocker work item is used to hold a second
 * item in the pending state deterministically.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include "wqtest.h"

static struct work_struct cw;		/* work under test */
static struct work_struct bw;		/* blocker, occupies the single slot */

static struct completion b_started, b_gate;
static struct completion c_started, c_may_finish, c_finished;
static atomic_t cruns;

static void blocker_fn(struct work_struct *w)
{
	complete(&b_started);
	wait_for_completion_timeout(&b_gate, msecs_to_jiffies(5000));
}

static void cancel_fn(struct work_struct *w)
{
	atomic_inc(&cruns);
	complete(&c_started);
	wait_for_completion_timeout(&c_may_finish, msecs_to_jiffies(2000));
	complete(&c_finished);
}

static int __init wqt_05_init(void)
{
	struct workqueue_struct *wq;
	bool ret;
	long r;

	WQT_INIT(5, "cancel");

	wq = alloc_workqueue("wqt05", WQ_UNBOUND, 1);
	if (!wq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}

	init_completion(&b_started);
	init_completion(&b_gate);
	init_completion(&c_started);
	init_completion(&c_may_finish);
	init_completion(&c_finished);
	INIT_WORK(&bw, blocker_fn);
	INIT_WORK(&cw, cancel_fn);

	/* 1: cancel a pending (not yet started) item -> true, never runs. */
	atomic_set(&cruns, 0);
	queue_work(wq, &bw);
	r = wait_for_completion_timeout(&b_started, msecs_to_jiffies(1000));
	WQT_CHECK(r > 0, "blocker never started");
	queue_work(wq, &cw);			/* stuck behind the blocker */
	WQT_CHECK(work_busy(&cw) & WORK_BUSY_PENDING,
		  "queued work not reported pending (busy=0x%x)", work_busy(&cw));
	ret = cancel_work_sync(&cw);
	WQT_CHECK(ret, "cancel of pending work returned false");
	WQT_CHECK(atomic_read(&cruns) == 0,
		  "pending work ran after cancel (%d)", atomic_read(&cruns));
	complete(&b_gate);			/* release the blocker */
	flush_workqueue(wq);

	/* 2: cancel a running item -> false (not pending), but waits. */
	reinit_completion(&c_started);
	reinit_completion(&c_may_finish);
	reinit_completion(&c_finished);
	atomic_set(&cruns, 0);
	INIT_WORK(&cw, cancel_fn);
	queue_work(wq, &cw);
	r = wait_for_completion_timeout(&c_started, msecs_to_jiffies(1000));
	WQT_CHECK(r > 0, "work never started");
	WQT_DIAG("work_busy while running = 0x%x", work_busy(&cw));
	WQT_CHECK(work_busy(&cw) & WORK_BUSY_RUNNING,
		  "running work not reported busy");
	complete(&c_may_finish);
	ret = cancel_work_sync(&cw);
	WQT_CHECK(!ret, "cancel of running (non-pending) work returned true");
	WQT_CHECK(atomic_read(&cruns) == 1,
		  "work ran %d times, expected 1", atomic_read(&cruns));
	WQT_CHECK(completion_done(&c_finished),
		  "work not finished after cancel_work_sync returned");

	/* 3: cancel an idle item -> false. */
	atomic_set(&cruns, 0);
	ret = cancel_work_sync(&cw);
	WQT_CHECK(!ret, "cancel of idle work returned true");
	WQT_CHECK(atomic_read(&cruns) == 0, "idle work ran (%d)",
		  atomic_read(&cruns));

	/* 4: re-queue after cancel works. */
	reinit_completion(&c_may_finish);
	complete(&c_may_finish);		/* let it finish without blocking */
	atomic_set(&cruns, 0);
	INIT_WORK(&cw, cancel_fn);
	queue_work(wq, &cw);
	flush_work(&cw);
	WQT_CHECK(atomic_read(&cruns) == 1,
		  "re-queued work ran %d times, expected 1", atomic_read(&cruns));
	WQT_CHECK(work_busy(&cw) == 0, "idle work_busy nonzero (0x%x)",
		  work_busy(&cw));

	cancel_work_sync(&cw);
	cancel_work_sync(&bw);
	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_05_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: cancel_work_sync semantics");
MODULE_LICENSE("GPL");
