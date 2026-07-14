// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_13_rcu_work - queue_rcu_work() / flush_rcu_work().
 *
 * queue_rcu_work() runs a work item only after a full RCU grace period has
 * elapsed.  We verify the item runs exactly once per queue, that
 * flush_rcu_work() waits for it (grace period included), and that the same
 * rcu_work can be re-queued after it has run.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include "wqtest.h"

static struct rcu_work rwork;
static atomic_t runs;
static struct completion ran;

static void rcu_fn(struct work_struct *w)
{
	atomic_inc(&runs);
	complete(&ran);
}

static int __init wqt_13_init(void)
{
	struct workqueue_struct *wq;
	bool queued, waited;
	long r;

	WQT_INIT(13, "rcu_work");

	wq = alloc_workqueue("wqt13", WQ_UNBOUND, 0);
	if (!wq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}
	INIT_RCU_WORK(&rwork, rcu_fn);
	atomic_set(&runs, 0);
	init_completion(&ran);

	/* 1: queue, flush, and confirm it ran exactly once. */
	queued = queue_rcu_work(wq, &rwork);
	WQT_CHECK(queued, "queue_rcu_work returned false for an idle rcu_work");

	waited = flush_rcu_work(&rwork);
	WQT_CHECK(waited, "flush_rcu_work returned false for a queued rcu_work");
	WQT_CHECK(atomic_read(&runs) == 1,
		  "rcu_work ran %d times, expected 1", atomic_read(&runs));

	/* Backstop the flush with an explicit wait in case the above raced. */
	r = wait_for_completion_timeout(&ran, msecs_to_jiffies(5000));
	WQT_CHECK(r > 0, "rcu_work never signalled completion");

	/* 2: an rcu_work can be re-queued after it has run. */
	reinit_completion(&ran);
	queued = queue_rcu_work(wq, &rwork);
	WQT_CHECK(queued, "re-queue of rcu_work returned false");
	r = wait_for_completion_timeout(&ran, msecs_to_jiffies(5000));
	WQT_CHECK(r > 0, "re-queued rcu_work never ran");
	WQT_CHECK(atomic_read(&runs) == 2,
		  "rcu_work ran %d times after re-queue, expected 2",
		  atomic_read(&runs));

	flush_rcu_work(&rwork);
	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_13_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: queue_rcu_work/flush_rcu_work");
MODULE_LICENSE("GPL");
