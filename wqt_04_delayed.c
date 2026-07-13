// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_04_delayed - delayed work timing, reschedule and cancel.
 *
 * Exercises delayed_work: that it fires after (and not before) its delay, that
 * mod_delayed_work() reschedules it, that cancel_delayed_work_sync() stops a
 * pending item, and that flush_delayed_work() runs it immediately.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include "wqtest.h"

static struct delayed_work dwork;
static struct completion dcomp;
static unsigned long queued_at, ran_at;
static atomic_t runs;

static void delayed_fn(struct work_struct *w)
{
	ran_at = jiffies;
	atomic_inc(&runs);
	complete(&dcomp);
}

static int __init wqt_04_init(void)
{
	struct workqueue_struct *wq;
	unsigned long elapsed;
	bool cancelled;
	long r;

	WQT_INIT(4, "delayed");

	wq = alloc_workqueue("wqt04", WQ_PERCPU, 0);
	if (!wq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}
	INIT_DELAYED_WORK(&dwork, delayed_fn);

	/* 1: fires after the delay, and not before. */
	init_completion(&dcomp);
	atomic_set(&runs, 0);
	queued_at = jiffies;
	queue_delayed_work(wq, &dwork, msecs_to_jiffies(200));
	msleep(50);
	WQT_CHECK(!completion_done(&dcomp),
		  "delayed work fired early (50ms into a 200ms delay)");
	r = wait_for_completion_timeout(&dcomp, msecs_to_jiffies(2000));
	WQT_CHECK(r > 0, "delayed work never fired");
	elapsed = jiffies_to_msecs(ran_at - queued_at);
	WQT_CHECK(elapsed >= 150, "delayed work fired too soon: %lums", elapsed);
	WQT_DIAG("delayed fired after %lums (target 200)", elapsed);

	/* 2: mod_delayed_work() shortens a long delay. */
	reinit_completion(&dcomp);
	atomic_set(&runs, 0);
	queued_at = jiffies;
	queue_delayed_work(wq, &dwork, msecs_to_jiffies(1000));
	mod_delayed_work(wq, &dwork, msecs_to_jiffies(100));
	r = wait_for_completion_timeout(&dcomp, msecs_to_jiffies(600));
	WQT_CHECK(r > 0, "mod_delayed_work: work never fired");
	elapsed = jiffies_to_msecs(ran_at - queued_at);
	WQT_CHECK(elapsed < 500, "mod_delayed_work did not shorten: %lums",
		  elapsed);
	WQT_DIAG("mod_delayed_work fired after %lums (target 100)", elapsed);

	/* 3: cancel_delayed_work_sync() stops a pending item. */
	reinit_completion(&dcomp);
	atomic_set(&runs, 0);
	queue_delayed_work(wq, &dwork, msecs_to_jiffies(300));
	cancelled = cancel_delayed_work_sync(&dwork);
	WQT_CHECK(cancelled,
		  "cancel_delayed_work_sync returned false for a pending item");
	msleep(400);
	WQT_CHECK(atomic_read(&runs) == 0,
		  "cancelled delayed work still ran (%d times)",
		  atomic_read(&runs));

	/* 4: flush_delayed_work() runs the item immediately. */
	reinit_completion(&dcomp);
	atomic_set(&runs, 0);
	queued_at = jiffies;
	queue_delayed_work(wq, &dwork, msecs_to_jiffies(10000));
	flush_delayed_work(&dwork);
	elapsed = jiffies_to_msecs(jiffies - queued_at);
	WQT_CHECK(atomic_read(&runs) == 1,
		  "flush_delayed_work did not run the item (%d)",
		  atomic_read(&runs));
	WQT_CHECK(elapsed < 5000,
		  "flush_delayed_work waited the full delay: %lums", elapsed);

	cancel_delayed_work_sync(&dwork);
	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_04_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: delayed work timing/cancel");
MODULE_LICENSE("GPL");
