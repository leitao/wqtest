// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_30_bh_softirq_gate - local_bh_disable() gates BH work on that cpu.
 *
 * A BH workqueue is softirq, so the softirq gate is its execution gate: an item
 * queued to the local cpu while softirqs are off stays pending, and
 * local_bh_enable() runs it before it returns -- there is no worker to wake and
 * nothing else to wait for.  Nesting has to be honoured too: only the outermost
 * local_bh_enable() opens the gate.
 *
 * Queueing to a *remote* cpu from the same section takes the other kick path.
 * kick_bh_pool() cannot raise a softirq on another cpu directly, so it sends an
 * irq_work to that cpu, which raises it there; that item runs even though the
 * queueing cpu still has softirqs disabled.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/bottom_half.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include "wqtest.h"

#define SPIN_US		200
#define WAIT_MS		5000

static struct work_struct gate_work, nest_work, remote_work;
static atomic_t ran;
static int ran_on = -1;
static bool softirq_ctx;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void gate_fn(struct work_struct *w)
{
	ran_on = smp_processor_id();
	softirq_ctx = in_serving_softirq();
	atomic_inc(&ran);
	wake_up(&waitq);
}

static int __init wqt_30_init(void)
{
	int cpu, remote, ran_early, ran_inner, ran_outer, ran_late;
	struct workqueue_struct *wq;
	unsigned int busy;
	bool queued;

	WQT_INIT(30, "bh_softirq_gate");

	wq = alloc_workqueue("wqt30", WQ_BH | WQ_PERCPU, 0);
	if (!wq) {
		WQT_FAIL("alloc_workqueue(WQ_BH) failed");
		return WQT_FINISH();
	}
	INIT_WORK(&gate_work, gate_fn);
	INIT_WORK(&nest_work, gate_fn);
	INIT_WORK(&remote_work, gate_fn);

	/* 1: softirqs off holds the item; local_bh_enable() runs it. */
	atomic_set(&ran, 0);
	ran_on = -1;
	local_bh_disable();
	cpu = smp_processor_id();
	queued = queue_work(wq, &gate_work);
	udelay(SPIN_US);
	ran_early = atomic_read(&ran);
	busy = work_busy(&gate_work);
	local_bh_enable();
	ran_late = atomic_read(&ran);

	WQT_CHECK(queued, "queue_work rejected an idle item");
	WQT_CHECK(ran_early == 0, "BH item ran with softirqs disabled (%d)",
		  ran_early);
	WQT_CHECK(busy & WORK_BUSY_PENDING,
		  "gated BH item not reported pending (busy=0x%x)", busy);
	WQT_CHECK(ran_late == 1,
		  "BH item did not run by the time local_bh_enable() returned (%d)",
		  ran_late);
	WQT_CHECK(softirq_ctx, "BH item ran outside softirq context");
	WQT_CHECK(ran_on == cpu, "BH item queued on cpu%d ran on cpu%d", cpu,
		  ran_on);

	/* 2: only the outermost local_bh_enable() opens the gate. */
	atomic_set(&ran, 0);
	local_bh_disable();
	local_bh_disable();
	queue_work(wq, &nest_work);
	udelay(SPIN_US);
	ran_early = atomic_read(&ran);
	local_bh_enable();
	ran_inner = atomic_read(&ran);
	local_bh_enable();
	ran_outer = atomic_read(&ran);

	WQT_CHECK(ran_early == 0, "nested: BH item ran with softirqs disabled (%d)",
		  ran_early);
	WQT_CHECK(ran_inner == 0, "nested: BH item ran on the inner local_bh_enable() (%d)",
		  ran_inner);
	WQT_CHECK(ran_outer == 1, "nested: BH item did not run on the outer local_bh_enable() (%d)",
		  ran_outer);

	/* 3: a remote cpu is kicked by irq_work, not by this cpu's softirq. */
	if (num_online_cpus() < 2) {
		WQT_DIAG("single online cpu, remote-kick phase skipped");
		goto out;
	}

	atomic_set(&ran, 0);
	ran_on = -1;
	local_bh_disable();
	cpu = smp_processor_id();
	remote = cpumask_next(cpu, cpu_online_mask);
	if (remote >= nr_cpu_ids)
		remote = cpumask_first(cpu_online_mask);
	queued = queue_work_on(remote, wq, &remote_work);
	local_bh_enable();
	wait_event_timeout(waitq, atomic_read(&ran) == 1,
			   msecs_to_jiffies(WAIT_MS));
	flush_work(&remote_work);

	WQT_CHECK(remote != cpu, "remote-kick phase picked the local cpu%d", cpu);
	WQT_CHECK(queued, "queue_work_on(cpu=%d) rejected", remote);
	WQT_CHECK(atomic_read(&ran) == 1,
		  "remote BH item ran %d times, expected 1", atomic_read(&ran));
	WQT_CHECK(ran_on == remote, "remote BH item queued to cpu%d ran on cpu%d",
		  remote, ran_on);
	WQT_CHECK(softirq_ctx, "remote BH item ran outside softirq context");
	WQT_DIAG("remote kick cpu%d -> cpu%d via irq_work", cpu, remote);

out:
	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_30_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: WQ_BH gated by local_bh_disable()");
MODULE_LICENSE("GPL");
