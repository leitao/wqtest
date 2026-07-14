// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_06_mem_reclaim - WQ_MEM_RECLAIM forward progress.
 *
 * A WQ_MEM_RECLAIM workqueue has a rescuer thread so queued work makes forward
 * progress even when the normal worker pool cannot grow.  We cannot introspect
 * the rescuer or deterministically create real memory pressure from a module,
 * so this test verifies the observable behaviour: the reclaim workqueue is
 * created, runs a batch of work, and drains a set of items queued behind a
 * held-open slot without stalling (WQ_WATCHDOG would fire on a real stall).
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include "wqtest.h"

#define NR_BASIC  32
#define NR_FOLLOW 16

static struct work_struct basic[NR_BASIC];
static struct work_struct blocker;
static struct work_struct follow[NR_FOLLOW];
static atomic_t basic_done, follow_done;
static struct completion gate, blk_started;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void basic_fn(struct work_struct *w)
{
	atomic_inc(&basic_done);
	wake_up(&waitq);
}

static void blocker_fn(struct work_struct *w)
{
	complete(&blk_started);
	wait_for_completion_timeout(&gate, msecs_to_jiffies(5000));
}

static void follow_fn(struct work_struct *w)
{
	atomic_inc(&follow_done);
	wake_up(&waitq);
}

static int __init wqt_06_init(void)
{
	struct workqueue_struct *wq;
	long r;
	int i;

	WQT_INIT(6, "mem_reclaim");

	wq = alloc_workqueue("wqt06", WQ_MEM_RECLAIM | WQ_PERCPU, 1);
	if (!wq) {
		WQT_FAIL("alloc_workqueue(WQ_MEM_RECLAIM) failed");
		return WQT_FINISH();
	}

	/* Basic forward progress on the reclaim workqueue. */
	atomic_set(&basic_done, 0);
	for (i = 0; i < NR_BASIC; i++) {
		INIT_WORK(&basic[i], basic_fn);
		queue_work(wq, &basic[i]);
	}
	r = wait_event_timeout(waitq, atomic_read(&basic_done) == NR_BASIC,
			       msecs_to_jiffies(5000));
	WQT_CHECK(r > 0, "reclaim wq: only %d/%d basic items ran",
		  atomic_read(&basic_done), NR_BASIC);
	flush_workqueue(wq);

	/* Queue items behind a held-open slot, then release and drain. */
	init_completion(&gate);
	init_completion(&blk_started);
	atomic_set(&follow_done, 0);
	INIT_WORK(&blocker, blocker_fn);
	queue_work(wq, &blocker);
	r = wait_for_completion_timeout(&blk_started, msecs_to_jiffies(2000));
	WQT_CHECK(r > 0, "blocker never started");

	for (i = 0; i < NR_FOLLOW; i++) {
		INIT_WORK(&follow[i], follow_fn);
		queue_work(wq, &follow[i]);
	}
	complete(&gate);			/* release the slot */
	r = wait_event_timeout(waitq, atomic_read(&follow_done) == NR_FOLLOW,
			       msecs_to_jiffies(5000));
	WQT_CHECK(r > 0, "reclaim wq: only %d/%d followers ran after unblock",
		  atomic_read(&follow_done), NR_FOLLOW);

	flush_workqueue(wq);
	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_06_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: WQ_MEM_RECLAIM forward progress");
MODULE_LICENSE("GPL");
