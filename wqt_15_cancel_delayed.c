// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_15_cancel_delayed - async cancel_delayed_work() and on-cpu delayed queue.
 *
 * cancel_delayed_work() (the non-sync variant) cancels a delayed item whose
 * timer has not yet fired: it returns true, clears delayed_work_pending() and
 * the item never runs; on an idle item it returns false.  Also checks that
 * queue_delayed_work_on(cpu, ...) runs the item on the requested CPU.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include "wqtest.h"

static struct delayed_work dwork;
static atomic_t runs;
static int ran_on;

static void d_fn(struct work_struct *w)
{
	ran_on = smp_processor_id();
	atomic_inc(&runs);
}

static int __init wqt_15_init(void)
{
	struct workqueue_struct *wq;
	bool c1, c2;
	int target;

	WQT_INIT(15, "cancel_delayed");

	wq = alloc_workqueue("wqt15", WQ_PERCPU, 0);
	if (!wq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}
	INIT_DELAYED_WORK(&dwork, d_fn);

	/* 1: cancel_delayed_work() removes a still-armed timer. */
	atomic_set(&runs, 0);
	queue_delayed_work(wq, &dwork, msecs_to_jiffies(10000));
	WQT_CHECK(delayed_work_pending(&dwork),
		  "delayed work not pending after queue");
	c1 = cancel_delayed_work(&dwork);
	WQT_CHECK(c1, "cancel_delayed_work returned false for an armed timer");
	WQT_CHECK(!delayed_work_pending(&dwork),
		  "delayed work still pending after cancel");
	msleep(50);
	WQT_CHECK(atomic_read(&runs) == 0,
		  "cancelled delayed work ran (%d)", atomic_read(&runs));

	/* 2: cancelling an idle delayed item returns false. */
	c2 = cancel_delayed_work(&dwork);
	WQT_CHECK(!c2, "cancel_delayed_work on idle item returned true");

	/* 3: queue_delayed_work_on() runs on the requested CPU. */
	atomic_set(&runs, 0);
	ran_on = -1;
	target = cpumask_first(cpu_online_mask);
	queue_delayed_work_on(target, wq, &dwork, 0);
	flush_delayed_work(&dwork);
	WQT_CHECK(atomic_read(&runs) == 1,
		  "on-cpu delayed work ran %d times, expected 1",
		  atomic_read(&runs));
	WQT_CHECK(ran_on == target,
		  "delayed work ran on cpu %d, expected %d", ran_on, target);

	cancel_delayed_work_sync(&dwork);
	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_15_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: cancel_delayed_work + on-cpu delayed");
MODULE_LICENSE("GPL");
