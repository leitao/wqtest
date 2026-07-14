// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_12_drain - queue idempotency and drain_workqueue() chain draining.
 *
 * Two guarantees:
 *   - queue_work() on an item that is already pending returns false and the
 *     item still runs only once (a work_struct has a single pending slot);
 *   - drain_workqueue() drains work that re-queues itself, which plain
 *     flush_workqueue() would not follow.  A self-chaining item is queued once
 *     and must have run its whole chain by the time drain_workqueue() returns.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include "wqtest.h"

#define CHAIN_LEN 6

/* --- part 1: double-queue idempotency --- */
static struct work_struct blocker;
static struct work_struct dq_work;
static struct completion blk_started, blk_gate;
static atomic_t dq_runs;

static void blocker_fn(struct work_struct *w)
{
	complete(&blk_started);
	wait_for_completion_timeout(&blk_gate, msecs_to_jiffies(5000));
}

static void dq_fn(struct work_struct *w)
{
	atomic_inc(&dq_runs);
}

/* --- part 2: self-requeuing chain --- */
static struct workqueue_struct *chain_wq;
static struct work_struct chain_work;
static atomic_t chain_left, chain_runs;

static void chain_fn(struct work_struct *w)
{
	atomic_inc(&chain_runs);
	if (atomic_dec_return(&chain_left) > 0)
		queue_work(chain_wq, w);	/* chained re-queue */
}

static int __init wqt_12_init(void)
{
	struct workqueue_struct *wq;
	bool q1, q2;
	long r;

	WQT_INIT(12, "drain");

	/* 1: an already-pending item cannot be queued twice, runs once. */
	wq = alloc_workqueue("wqt12_dq", WQ_UNBOUND, 1);
	if (!wq) {
		WQT_FAIL("alloc_workqueue(dq) failed");
		return WQT_FINISH();
	}
	init_completion(&blk_started);
	init_completion(&blk_gate);
	atomic_set(&dq_runs, 0);
	INIT_WORK(&blocker, blocker_fn);
	INIT_WORK(&dq_work, dq_fn);

	queue_work(wq, &blocker);		/* occupy the single slot */
	r = wait_for_completion_timeout(&blk_started, msecs_to_jiffies(2000));
	WQT_CHECK(r > 0, "blocker never started");

	q1 = queue_work(wq, &dq_work);		/* becomes pending */
	q2 = queue_work(wq, &dq_work);		/* already pending */
	WQT_CHECK(q1, "first queue_work of idle item returned false");
	WQT_CHECK(!q2, "second queue_work of pending item returned true");
	WQT_CHECK(work_pending(&dq_work), "item not pending after queue");

	complete(&blk_gate);			/* release the blocker */
	flush_workqueue(wq);
	WQT_CHECK(atomic_read(&dq_runs) == 1,
		  "double-queued item ran %d times, expected 1",
		  atomic_read(&dq_runs));
	destroy_workqueue(wq);

	/* 2: drain_workqueue() follows a self-requeuing chain to the end. */
	chain_wq = alloc_workqueue("wqt12_chain", WQ_UNBOUND, 0);
	if (!chain_wq) {
		WQT_FAIL("alloc_workqueue(chain) failed");
		return WQT_FINISH();
	}
	INIT_WORK(&chain_work, chain_fn);
	atomic_set(&chain_left, CHAIN_LEN);
	atomic_set(&chain_runs, 0);

	queue_work(chain_wq, &chain_work);
	drain_workqueue(chain_wq);

	WQT_CHECK(atomic_read(&chain_runs) == CHAIN_LEN,
		  "chain ran %d times after drain, expected %d",
		  atomic_read(&chain_runs), CHAIN_LEN);
	WQT_CHECK(!work_pending(&chain_work),
		  "chain item still pending after drain_workqueue");

	destroy_workqueue(chain_wq);

	return WQT_FINISH();
}
module_init(wqt_12_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: queue idempotency + drain_workqueue");
MODULE_LICENSE("GPL");
