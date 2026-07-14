// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_18_vmstat_shepherd - self-rearming per-cpu delayed work + shepherd.
 *
 * Models mm/vmstat.c: each CPU has a deferrable delayed_work (vmstat_update)
 * that folds pending per-cpu deltas and, while more remain, re-queues *itself*
 * on the same CPU (queue_delayed_work_on(smp_processor_id(), ...)) -- and stops
 * once quiescent.  A periodic shepherd (vmstat_shepherd) scans CPUs and kicks
 * any that have pending work but no worker already running/queued.
 *
 * We give each CPU a pending count, drive it both ways (self-rearm after one
 * kick, and shepherd-driven), and require every unit to drain, the workers to
 * self-disarm (no infinite re-queue), and teardown to cancel cleanly.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include "wqtest.h"

#define PER_CPU_UNITS 5

struct pc_state {
	struct delayed_work dwork;
	atomic_t pending;
};

static DEFINE_PER_CPU(struct pc_state, pcs);
static struct workqueue_struct *vwq;
static atomic_t total_drained;
static DECLARE_WAIT_QUEUE_HEAD(drain_waitq);

/* vmstat_update idiom: fold one unit, re-arm self on this cpu while more remain. */
static void update_fn(struct work_struct *w)
{
	struct pc_state *pc = container_of(to_delayed_work(w),
					   struct pc_state, dwork);

	if (atomic_read(&pc->pending) <= 0)
		return;

	atomic_dec(&pc->pending);
	atomic_inc(&total_drained);
	wake_up(&drain_waitq);

	if (atomic_read(&pc->pending) > 0)
		queue_delayed_work_on(smp_processor_id(), vwq, &pc->dwork, 0);
}

/* vmstat_shepherd idiom: kick cpus that have work pending but no worker yet. */
static void shepherd_kick(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct pc_state *pc = per_cpu_ptr(&pcs, cpu);

		if (atomic_read(&pc->pending) > 0 &&
		    !delayed_work_pending(&pc->dwork) &&
		    !work_busy(&pc->dwork.work))
			queue_delayed_work_on(cpu, vwq, &pc->dwork, 0);
	}
}

static void dirty_all_cpus(void)
{
	int cpu;

	atomic_set(&total_drained, 0);
	for_each_online_cpu(cpu)
		atomic_set(&per_cpu_ptr(&pcs, cpu)->pending, PER_CPU_UNITS);
}

static int __init wqt_18_init(void)
{
	int cpu, expected, i, still;
	long r;

	WQT_INIT(18, "vmstat_shepherd");

	vwq = alloc_workqueue("wqt18", WQ_PERCPU, 0);
	if (!vwq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}

	for_each_online_cpu(cpu)
		INIT_DEFERRABLE_WORK(&per_cpu_ptr(&pcs, cpu)->dwork, update_fn);

	expected = num_online_cpus() * PER_CPU_UNITS;

	/* 1: one kick per cpu; each self-rearms until its count reaches 0. */
	dirty_all_cpus();
	for_each_online_cpu(cpu)
		queue_delayed_work_on(cpu, vwq, &per_cpu_ptr(&pcs, cpu)->dwork, 0);

	r = wait_event_timeout(drain_waitq,
			       atomic_read(&total_drained) == expected,
			       msecs_to_jiffies(10000));
	WQT_CHECK(r > 0, "self-rearm drained %d/%d units",
		  atomic_read(&total_drained), expected);

	/* Self-disarm: nothing left pending or armed once quiescent. */
	msleep(50);
	still = 0;
	for_each_online_cpu(cpu) {
		struct pc_state *pc = per_cpu_ptr(&pcs, cpu);

		if (atomic_read(&pc->pending) != 0 ||
		    delayed_work_pending(&pc->dwork))
			still++;
	}
	WQT_CHECK(still == 0, "%d cpu(s) still pending/armed after drain", still);

	/* 2: shepherd-driven -- re-dirty without kicking, let the shepherd find them. */
	dirty_all_cpus();
	for (i = 0; i < 10; i++) {
		shepherd_kick();
		r = wait_event_timeout(drain_waitq,
				       atomic_read(&total_drained) == expected,
				       msecs_to_jiffies(1000));
		if (r > 0)
			break;
	}
	WQT_CHECK(atomic_read(&total_drained) == expected,
		  "shepherd drained %d/%d units", atomic_read(&total_drained),
		  expected);
	WQT_DIAG("drained %d units across %d cpus (self-rearm + shepherd)",
		 expected, num_online_cpus());

	for_each_online_cpu(cpu)
		cancel_delayed_work_sync(&per_cpu_ptr(&pcs, cpu)->dwork);
	destroy_workqueue(vwq);

	return WQT_FINISH();
}
module_init(wqt_18_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: self-rearming per-cpu work + shepherd (vmstat idiom)");
MODULE_LICENSE("GPL");
