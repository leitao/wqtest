// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_22_timeout - delayed_work as a cancellable command deadline.
 *
 * Drivers arm a delayed_work as a deadline when starting an operation and
 * cancel it on the happy path, letting it fire only when the operation stalls.
 * drivers/nvme/host/core.c does exactly this: nvme_failfast_work is armed with
 * schedule_delayed_work() when a controller starts reconnecting and cancelled
 * with cancel_delayed_work_sync() once it recovers; the keep-alive ka_work is
 * managed the same way.
 *
 * We model a command with an INFLIGHT/COMPLETED/TIMEDOUT state: a fast command
 * cancels its (long) deadline before it fires; a stalled command lets the
 * (short) deadline fire and abort it.  The state transition is a cmpxchg so the
 * completion/timeout race resolves to exactly one winner.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include "wqtest.h"

enum { CMD_INFLIGHT, CMD_COMPLETED, CMD_TIMEDOUT };

static struct workqueue_struct *twq;
static struct delayed_work deadline;
static atomic_t cmd_state;
static atomic_t timeouts_fired;

/* deadline handler: aborts the command iff it is still in flight. */
static void deadline_fn(struct work_struct *w)
{
	if (atomic_cmpxchg(&cmd_state, CMD_INFLIGHT, CMD_TIMEDOUT) == CMD_INFLIGHT)
		atomic_inc(&timeouts_fired);
}

static void cmd_issue(unsigned long deadline_ms)
{
	atomic_set(&cmd_state, CMD_INFLIGHT);
	queue_delayed_work(twq, &deadline, msecs_to_jiffies(deadline_ms));
}

/* happy path: mark complete and disarm the deadline. */
static bool cmd_complete(void)
{
	if (atomic_cmpxchg(&cmd_state, CMD_INFLIGHT, CMD_COMPLETED) != CMD_INFLIGHT)
		return false;			/* deadline already won */
	cancel_delayed_work_sync(&deadline);
	return true;
}

static int __init wqt_22_init(void)
{
	bool ok;

	WQT_INIT(22, "timeout");

	twq = alloc_workqueue("wqt22", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!twq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}
	INIT_DELAYED_WORK(&deadline, deadline_fn);

	/* 1: fast command -- completes long before its deadline, cancels it. */
	atomic_set(&timeouts_fired, 0);
	cmd_issue(10000);			/* 10s deadline */
	msleep(20);				/* command "runs" */
	ok = cmd_complete();
	WQT_CHECK(ok, "cmd_complete lost the race against a 10s deadline");
	WQT_CHECK(atomic_read(&cmd_state) == CMD_COMPLETED,
		  "state=%d after completion", atomic_read(&cmd_state));
	msleep(50);
	WQT_CHECK(atomic_read(&timeouts_fired) == 0,
		  "deadline fired for a completed command (%d)",
		  atomic_read(&timeouts_fired));
	WQT_CHECK(!delayed_work_pending(&deadline),
		  "deadline still armed after completion");

	/* 2: stalled command -- the deadline fires and aborts it. */
	atomic_set(&timeouts_fired, 0);
	cmd_issue(100);				/* 100ms deadline, never completed */
	msleep(400);
	WQT_CHECK(atomic_read(&cmd_state) == CMD_TIMEDOUT,
		  "stalled command not timed out (state=%d)",
		  atomic_read(&cmd_state));
	WQT_CHECK(atomic_read(&timeouts_fired) == 1,
		  "deadline fired %d times, expected 1",
		  atomic_read(&timeouts_fired));
	WQT_CHECK(!cmd_complete(),
		  "completion succeeded after the deadline already fired");
	WQT_DIAG("fast command cancelled its deadline; stalled command timed out");

	cancel_delayed_work_sync(&deadline);
	destroy_workqueue(twq);

	return WQT_FINISH();
}
module_init(wqt_22_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: cancellable command timeout (nvme idiom)");
MODULE_LICENSE("GPL");
