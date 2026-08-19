// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_32_bh_delayed - delayed queueing onto a BH workqueue.
 *
 * "BH work items cannot sleep.  All other features such as delayed queueing,
 * flushing and canceling are supported."  Delayed queueing is the interesting
 * one because it runs through a second deferral layer: the timer is armed on a
 * housekeeping cpu, and delayed_work_timer_fn() -- itself already in softirq --
 * queues the work to dwork->cpu, whose BH pool then runs it.  The handoff has
 * to preserve both the target cpu and the BH execution context.
 *
 * wqt_04 covers the timing contract on a normal workqueue; this repeats it on a
 * BH one and adds the checks that only make sense there.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include "wqtest.h"

static struct delayed_work dwork;
static struct completion dcomp;
static unsigned long queued_at, ran_at;
static atomic_t runs;
static int ran_on = -1;
static bool softirq_ctx, hardirq_ctx;

static void delayed_fn(struct work_struct *w)
{
	ran_at = jiffies;
	ran_on = smp_processor_id();
	softirq_ctx = in_serving_softirq();
	hardirq_ctx = in_hardirq();
	atomic_inc(&runs);
	complete(&dcomp);
}

static int __init wqt_32_init(void)
{
	struct workqueue_struct *wq;
	unsigned long elapsed;
	int target;
	bool ret;
	long r;

	WQT_INIT(32, "bh_delayed");

	wq = alloc_workqueue("wqt32", WQ_BH | WQ_PERCPU, 0);
	if (!wq) {
		WQT_FAIL("alloc_workqueue(WQ_BH) failed");
		return WQT_FINISH();
	}
	INIT_DELAYED_WORK(&dwork, delayed_fn);

	/* 1: fires after the delay, not before, and lands in softirq. */
	init_completion(&dcomp);
	atomic_set(&runs, 0);
	queued_at = jiffies;
	ret = queue_delayed_work(wq, &dwork, msecs_to_jiffies(200));
	WQT_CHECK(ret, "queue_delayed_work rejected an idle item");
	WQT_CHECK(delayed_work_pending(&dwork),
		  "armed delayed work not reported pending");
	msleep(50);
	WQT_CHECK(!completion_done(&dcomp),
		  "BH delayed work fired early (50ms into a 200ms delay)");
	r = wait_for_completion_timeout(&dcomp, msecs_to_jiffies(2000));
	WQT_CHECK(r > 0, "BH delayed work never fired");
	elapsed = jiffies_to_msecs(ran_at - queued_at);
	WQT_CHECK(elapsed >= 150, "BH delayed work fired too soon: %lums",
		  elapsed);
	WQT_CHECK(softirq_ctx && !hardirq_ctx,
		  "BH delayed work ran softirq=%d hardirq=%d", softirq_ctx,
		  hardirq_ctx);
	WQT_CHECK(!delayed_work_pending(&dwork),
		  "delayed work still pending after it ran");
	WQT_DIAG("BH delayed fired after %lums (target 200) on cpu%d", elapsed,
		 ran_on);

	/* 2: mod_delayed_work() shortens a long delay. */
	reinit_completion(&dcomp);
	atomic_set(&runs, 0);
	queued_at = jiffies;
	queue_delayed_work(wq, &dwork, msecs_to_jiffies(1000));
	mod_delayed_work(wq, &dwork, msecs_to_jiffies(100));
	r = wait_for_completion_timeout(&dcomp, msecs_to_jiffies(600));
	WQT_CHECK(r > 0, "mod_delayed_work: BH work never fired");
	elapsed = jiffies_to_msecs(ran_at - queued_at);
	WQT_CHECK(elapsed < 500, "mod_delayed_work did not shorten: %lums",
		  elapsed);
	WQT_CHECK(atomic_read(&runs) == 1,
		  "mod_delayed_work: work ran %d times, expected 1",
		  atomic_read(&runs));

	/* 3: cancel_delayed_work_sync() stops an armed item. */
	reinit_completion(&dcomp);
	atomic_set(&runs, 0);
	queue_delayed_work(wq, &dwork, msecs_to_jiffies(300));
	ret = cancel_delayed_work_sync(&dwork);
	WQT_CHECK(ret, "cancel_delayed_work_sync returned false for an armed item");
	WQT_CHECK(!delayed_work_pending(&dwork),
		  "cancelled delayed work still reported pending");
	msleep(400);
	WQT_CHECK(atomic_read(&runs) == 0, "cancelled BH delayed work still ran (%d)",
		  atomic_read(&runs));

	/* 4: flush_delayed_work() runs the item without waiting out the delay. */
	reinit_completion(&dcomp);
	atomic_set(&runs, 0);
	queued_at = jiffies;
	queue_delayed_work(wq, &dwork, msecs_to_jiffies(10000));
	ret = flush_delayed_work(&dwork);
	elapsed = jiffies_to_msecs(jiffies - queued_at);
	WQT_CHECK(ret, "flush_delayed_work returned false for an armed item");
	WQT_CHECK(atomic_read(&runs) == 1,
		  "flush_delayed_work did not run the item (%d)",
		  atomic_read(&runs));
	WQT_CHECK(elapsed < 5000, "flush_delayed_work waited the full delay: %lums",
		  elapsed);
	WQT_CHECK(softirq_ctx,
		  "flushed BH delayed work ran outside softirq context");

	/* 5: queue_delayed_work_on() binds the BH item to the target cpu. */
	target = cpumask_last(cpu_online_mask);
	reinit_completion(&dcomp);
	atomic_set(&runs, 0);
	ran_on = -1;
	queue_delayed_work_on(target, wq, &dwork, msecs_to_jiffies(50));
	r = wait_for_completion_timeout(&dcomp, msecs_to_jiffies(2000));
	WQT_CHECK(r > 0, "queue_delayed_work_on: BH work never fired");
	WQT_CHECK(ran_on == target,
		  "BH delayed work queued to cpu%d ran on cpu%d", target,
		  ran_on);
	WQT_CHECK(softirq_ctx,
		  "cpu-bound BH delayed work ran outside softirq context");

	cancel_delayed_work_sync(&dwork);
	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_32_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: delayed work on a WQ_BH workqueue");
MODULE_LICENSE("GPL");
