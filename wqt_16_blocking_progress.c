// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_16_blocking_progress - a sleeping worker must not stall the pool.
 *
 * When a work item blocks (sleeps) on an unbound workqueue that is not capped
 * at max_active=1, the pool must still make forward progress on the other
 * queued items by growing/waking additional workers.  This is the
 * concurrency-management guarantee that a long-blocking handler (e.g. one that
 * waits in the kernel) would otherwise expose as a stall.  We hold one worker
 * in a wait, queue a batch behind it, and require the batch to finish while the
 * blocker is still parked; WQ_WATCHDOG is the second oracle for a real stall.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include "wqtest.h"

#define NR_FOLLOW 32

static struct work_struct blocker;
static struct work_struct follow[NR_FOLLOW];
static struct completion blk_started, blk_gate;
static atomic_t follow_done;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void blocker_fn(struct work_struct *w)
{
	complete(&blk_started);
	/* Park this worker the way a blocking handler would. */
	wait_for_completion_timeout(&blk_gate, msecs_to_jiffies(10000));
}

static void follow_fn(struct work_struct *w)
{
	atomic_inc(&follow_done);
	wake_up(&waitq);
}

static int __init wqt_16_init(void)
{
	struct workqueue_struct *wq;
	long r;
	int i;

	WQT_INIT(16, "blocking_progress");

	/* Default max_active: the cap must not be what serialises us. */
	wq = alloc_workqueue("wqt16", WQ_UNBOUND, 0);
	if (!wq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}

	init_completion(&blk_started);
	init_completion(&blk_gate);
	atomic_set(&follow_done, 0);
	INIT_WORK(&blocker, blocker_fn);

	/* Occupy one worker with a blocked handler. */
	queue_work(wq, &blocker);
	r = wait_for_completion_timeout(&blk_started, msecs_to_jiffies(5000));
	WQT_CHECK(r > 0, "blocker never started");

	/* Queue a batch that must run while the blocker is still parked. */
	for (i = 0; i < NR_FOLLOW; i++) {
		INIT_WORK(&follow[i], follow_fn);
		queue_work(wq, &follow[i]);
	}
	r = wait_event_timeout(waitq, atomic_read(&follow_done) == NR_FOLLOW,
			       msecs_to_jiffies(8000));
	WQT_CHECK(r > 0,
		  "pool stalled behind a blocked worker: %d/%d items ran",
		  atomic_read(&follow_done), NR_FOLLOW);
	WQT_CHECK(!completion_done(&blk_gate),
		  "blocker was released early; test did not exercise a stall");

	complete(&blk_gate);			/* release the blocker */
	flush_workqueue(wq);
	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_16_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: forward progress behind a blocked worker");
MODULE_LICENSE("GPL");
