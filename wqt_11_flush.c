// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_11_flush - flush_work() wait-and-order guarantee.
 *
 * flush_work() must block until an already-queued/running item has finished
 * executing, and return true iff it actually had to wait; flushing an idle item
 * returns false immediately.  A work item that sleeps for a fixed time lets us
 * assert that flush_work() did not return before the item completed.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include "wqtest.h"

#define SLEEP_MS 300

static struct work_struct busy_work;
static struct work_struct idle_work;
static atomic_t finished;

static void busy_fn(struct work_struct *w)
{
	msleep(SLEEP_MS);
	atomic_inc(&finished);
}

static void idle_fn(struct work_struct *w)
{
	atomic_inc(&finished);
}

static int __init wqt_11_init(void)
{
	struct workqueue_struct *wq;
	unsigned long t0, elapsed;
	bool waited;

	WQT_INIT(11, "flush");

	wq = alloc_workqueue("wqt11", WQ_UNBOUND, 0);
	if (!wq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}
	INIT_WORK(&busy_work, busy_fn);
	INIT_WORK(&idle_work, idle_fn);

	/* 1: flushing a never-queued item returns false and runs nothing. */
	atomic_set(&finished, 0);
	waited = flush_work(&idle_work);
	WQT_CHECK(!waited, "flush_work on idle item returned true");
	WQT_CHECK(atomic_read(&finished) == 0,
		  "idle item ran during flush (%d)", atomic_read(&finished));

	/* 2: flush_work blocks until a queued+running item finishes. */
	atomic_set(&finished, 0);
	t0 = jiffies;
	queue_work(wq, &busy_work);
	waited = flush_work(&busy_work);
	elapsed = jiffies_to_msecs(jiffies - t0);

	WQT_CHECK(waited, "flush_work returned false for a queued item");
	WQT_CHECK(atomic_read(&finished) == 1,
		  "item not finished when flush_work returned (%d)",
		  atomic_read(&finished));
	WQT_CHECK(elapsed >= SLEEP_MS - 100,
		  "flush_work returned early: %lums for a %dms item",
		  elapsed, SLEEP_MS);
	WQT_DIAG("flush_work waited %lums for a %dms item", elapsed, SLEEP_MS);

	/* 3: flushing again after completion returns false (already idle). */
	waited = flush_work(&busy_work);
	WQT_CHECK(!waited, "flush_work on a completed item returned true");

	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_11_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: flush_work wait/return semantics");
MODULE_LICENSE("GPL");
