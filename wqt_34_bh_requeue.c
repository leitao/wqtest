// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_34_bh_requeue - queueing more work from inside a BH handler.
 *
 * A BH item runs from softirq, so a queue_work() it makes has no worker to wake
 * and no task to schedule: it feeds the same pool it is being run out of, and
 * bh_worker() picks the item up on a later iteration of its own loop or in the
 * next softirq round.  It must never nest -- a BH pool has one execution
 * context per cpu, so a self-requeue that ran inline would be running the same
 * handler on top of itself.
 *
 * Four destinations are covered from inside a BH handler: itself, a second BH
 * workqueue on the same pool, a remote cpu (which cannot be kicked with
 * raise_softirq() and goes through irq_work instead), and a normal per-cpu
 * workqueue -- the handoff a BH item has to make when the work needs to sleep.
 *
 * The chain is finite and settled before teardown on purpose.  is_chained_work()
 * identifies a worker by PF_WQ_WORKER on %current, which no BH item has, so a
 * requeue from a BH handler during drain_workqueue() -- and therefore during
 * destroy_workqueue() -- is not recognised as chained work and is dropped with
 * a WARN.  An endless BH self-requeue chain cannot be torn down.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include "wqtest.h"

#define CHAIN_LEN	8
#define WAIT_MS		5000

static struct workqueue_struct *bh_wq, *bh_wq2, *task_wq;

static struct work_struct chain_work, peer_work, remote_work;
static struct work_struct escalate_work;	/* own lockdep class: task ctx */

static atomic_t chain_runs, peer_runs, remote_runs, escalate_runs;
static bool nested, chain_inside;
static bool chain_all_bh = true, peer_bh, remote_bh, escalate_in_task;
static int chain_cpu = -1, chain_wrong_cpu, remote_cpu = -1, remote_target = -1;
static int peer_after;				/* chain_runs when peer ran */
static DECLARE_COMPLETION(chain_done);
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void peer_fn(struct work_struct *w)
{
	peer_bh = in_serving_softirq();
	peer_after = atomic_read(&chain_runs);
	atomic_inc(&peer_runs);
	wake_up(&waitq);
}

static void remote_fn(struct work_struct *w)
{
	remote_cpu = smp_processor_id();
	remote_bh = in_serving_softirq();
	atomic_inc(&remote_runs);
	wake_up(&waitq);
}

/* The sleepable half of the handoff: queued from BH, runs in task context. */
static void escalate_fn(struct work_struct *w)
{
	escalate_in_task = !in_serving_softirq() && !in_hardirq();
	atomic_inc(&escalate_runs);
	wake_up(&waitq);
}

static void chain_fn(struct work_struct *w)
{
	int n;

	if (READ_ONCE(chain_inside))
		WRITE_ONCE(nested, true);
	WRITE_ONCE(chain_inside, true);

	if (!in_serving_softirq())
		chain_all_bh = false;
	if (chain_cpu < 0)
		chain_cpu = smp_processor_id();
	else if (chain_cpu != smp_processor_id())
		chain_wrong_cpu++;

	n = atomic_inc_return(&chain_runs);

	if (n == 1) {
		/* Same pool via a second BH workqueue, and the task-context
		 * handoff, both issued from softirq.
		 */
		queue_work(bh_wq2, &peer_work);
		queue_work(task_wq, &escalate_work);
		if (remote_target >= 0)
			queue_work_on(remote_target, bh_wq, &remote_work);
	}

	if (n < CHAIN_LEN)
		queue_work(bh_wq, &chain_work);	/* self-requeue */
	else
		complete(&chain_done);

	WRITE_ONCE(chain_inside, false);
}

static int __init wqt_34_init(void)
{
	int cpu, expect_remote;
	long r;

	WQT_INIT(34, "bh_requeue");

	bh_wq = alloc_workqueue("wqt34_bh", WQ_BH | WQ_PERCPU, 0);
	bh_wq2 = alloc_workqueue("wqt34_bh2", WQ_BH | WQ_PERCPU, 0);
	task_wq = alloc_workqueue("wqt34_task", WQ_PERCPU, 0);
	if (!bh_wq || !bh_wq2 || !task_wq) {
		WQT_FAIL("alloc_workqueue failed");
		goto out;
	}

	INIT_WORK(&chain_work, chain_fn);
	INIT_WORK(&peer_work, peer_fn);
	INIT_WORK(&remote_work, remote_fn);
	/* Separate call site: this one runs in task context, so it must not
	 * share a lockdep class with the BH items above.
	 */
	INIT_WORK(&escalate_work, escalate_fn);

	cpu = get_cpu();
	remote_target = cpumask_next(cpu, cpu_online_mask);
	if (remote_target >= nr_cpu_ids)
		remote_target = cpumask_first(cpu_online_mask);
	if (remote_target == cpu)
		remote_target = -1;		/* single online cpu */
	queue_work(bh_wq, &chain_work);
	put_cpu();
	expect_remote = remote_target >= 0 ? 1 : 0;

	r = wait_for_completion_timeout(&chain_done, msecs_to_jiffies(WAIT_MS));
	WQT_CHECK(r > 0, "BH self-requeue chain never finished (%d/%d runs)",
		  atomic_read(&chain_runs), CHAIN_LEN);

	wait_event_timeout(waitq,
			   atomic_read(&peer_runs) == 1 &&
			   atomic_read(&escalate_runs) == 1 &&
			   atomic_read(&remote_runs) == expect_remote,
			   msecs_to_jiffies(WAIT_MS));
	flush_workqueue(bh_wq);
	flush_workqueue(bh_wq2);
	flush_workqueue(task_wq);

	/* 1: the self-requeue chain. */
	WQT_CHECK(!nested, "BH item ran nested inside itself");
	WQT_CHECK(atomic_read(&chain_runs) == CHAIN_LEN,
		  "BH chain ran %d times, expected %d", atomic_read(&chain_runs),
		  CHAIN_LEN);
	WQT_CHECK(chain_all_bh, "a BH chain link ran outside softirq context");
	WQT_CHECK(chain_wrong_cpu == 0,
		  "%d BH chain links left cpu%d", chain_wrong_cpu, chain_cpu);

	/* 2: a second BH workqueue on the same pool, queued from softirq. */
	WQT_CHECK(atomic_read(&peer_runs) == 1, "peer BH item ran %d times",
		  atomic_read(&peer_runs));
	WQT_CHECK(peer_bh, "peer BH item ran outside softirq context");
	WQT_CHECK(peer_after >= 1,
		  "peer BH item ran before the item that queued it (%d)",
		  peer_after);

	/* 3: a remote cpu, kicked from softirq through irq_work. */
	if (remote_target < 0) {
		WQT_DIAG("single online cpu, remote-requeue phase skipped");
	} else {
		WQT_CHECK(atomic_read(&remote_runs) == 1,
			  "remote BH item ran %d times", atomic_read(&remote_runs));
		WQT_CHECK(remote_cpu == remote_target,
			  "remote BH item queued to cpu%d ran on cpu%d",
			  remote_target, remote_cpu);
		WQT_CHECK(remote_bh, "remote BH item ran outside softirq context");
	}

	/* 4: the handoff to a sleepable context. */
	WQT_CHECK(atomic_read(&escalate_runs) == 1,
		  "escalated item ran %d times", atomic_read(&escalate_runs));
	WQT_CHECK(escalate_in_task,
		  "item queued from BH onto a normal wq did not run in task context");

	WQT_DIAG("chain of %d on cpu%d, peer at link %d, remote cpu%d",
		 atomic_read(&chain_runs), chain_cpu, peer_after, remote_cpu);

out:
	if (task_wq)
		destroy_workqueue(task_wq);
	if (bh_wq2)
		destroy_workqueue(bh_wq2);
	if (bh_wq)
		destroy_workqueue(bh_wq);

	return WQT_FINISH();
}
module_init(wqt_34_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: requeueing from inside a WQ_BH handler");
MODULE_LICENSE("GPL");
