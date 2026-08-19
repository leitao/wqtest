// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_35_bh_hotplug - BH work left on a cpu that goes away.
 *
 * A normal per-cpu pool keeps its workers and its worklist across an offline;
 * the items are picked up again later.  A BH pool has no worker to keep: it is
 * driven by that cpu's softirq, and once the cpu is down nothing will ever
 * raise it again.  So the offline path has to drain the pool by hand.
 * CPUHP_SOFTIRQ_DEAD calls workqueue_softirq_dead(), which queues a work item
 * onto a live cpu's BH pool, runs the dead pool's bh_worker() from there and
 * waits for it, for both the normal and the highpri BH pool.
 *
 * That makes the contract testable without a race: by the time remove_cpu()
 * returns, every BH item queued to that cpu has already run -- and the ones the
 * drain took ran on some *other* cpu, which is the one place a BH item does not
 * run where it was queued.  The off-cpu count is what says the drain was
 * reached rather than the victim having simply finished its backlog first, so
 * the batch is sized to be far more work than the offline takes: queueing an
 * item is much cheaper than running one, so the backlog grows while it is being
 * queued and cannot be worked off in time.
 *
 * Nothing here re-queues itself.  A BH item that did would be handed back to
 * the dead pool by the non-reentrancy check in __queue_work() -- it is still
 * "executing" on that pool from the drain's point of view -- and the drain
 * would never see the pool go empty.
 *
 * Needs CONFIG_HOTPLUG_CPU and a second cpu; skipped with a diagnostic
 * otherwise.  rounds= sets the offline/online count (default 3).
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include "wqtest.h"

#define NR_ITEMS	512		/* per BH workqueue, per round */
#define NR_TOTAL	(2 * NR_ITEMS)
#define ITEM_US		100		/* so the batch outlasts the offline */
#define WAIT_MS		10000

static int rounds = 3;
module_param(rounds, int, 0444);
MODULE_PARM_DESC(rounds, "offline/online rounds (default 3)");

struct hp_item {
	struct work_struct work;
	int ran_on;
	int runs;
	bool softirq;
};

static struct hp_item *items;
static atomic_t ran;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void hp_fn(struct work_struct *w)
{
	struct hp_item *it = container_of(w, struct hp_item, work);

	it->ran_on = smp_processor_id();
	it->softirq = in_serving_softirq();
	it->runs++;
	udelay(ITEM_US);	/* keep the pool backlogged while the cpu dies */

	atomic_inc(&ran);
	wake_up(&waitq);
}

/* Reset every item and queue the batch, half to each workqueue, onto @cpu. */
static int queue_round(struct workqueue_struct *a, struct workqueue_struct *b,
		       int cpu)
{
	int i, rejected = 0;

	atomic_set(&ran, 0);
	for (i = 0; i < NR_TOTAL; i++) {
		items[i].ran_on = -1;
		items[i].runs = 0;
		items[i].softirq = false;
	}

	for (i = 0; i < NR_ITEMS; i++)
		if (!queue_work_on(cpu, a, &items[i].work))
			rejected++;
	for (i = 0; i < NR_ITEMS; i++)
		if (!queue_work_on(cpu, b, &items[NR_ITEMS + i].work))
			rejected++;

	return rejected;
}

static int __init wqt_35_init(void)
{
	struct workqueue_struct *bh_wq = NULL, *bh_hi_wq = NULL;
	int round, i, victim, ret, rejected;
	int total_off_cpu = 0, done_rounds = 0;

	WQT_INIT(35, "bh_hotplug");

	if (!IS_ENABLED(CONFIG_HOTPLUG_CPU) || num_online_cpus() < 2) {
		WQT_DIAG("no CONFIG_HOTPLUG_CPU or single cpu, test skipped");
		return WQT_FINISH();
	}

	items = kcalloc(NR_TOTAL, sizeof(*items), GFP_KERNEL);
	if (!items) {
		WQT_FAIL("kcalloc failed");
		return WQT_FINISH();
	}

	bh_wq = alloc_workqueue("wqt35_bh", WQ_BH | WQ_PERCPU, 0);
	bh_hi_wq = alloc_workqueue("wqt35_bh_hi",
				   WQ_BH | WQ_HIGHPRI | WQ_PERCPU, 0);
	if (!bh_wq || !bh_hi_wq) {
		WQT_FAIL("alloc_workqueue(WQ_BH) failed");
		goto out;
	}

	for (i = 0; i < NR_TOTAL; i++)
		INIT_WORK(&items[i].work, hp_fn);

	victim = cpumask_last(cpu_online_mask);

	for (round = 0; round < rounds; round++) {
		int off_cpu = 0, wrong_runs = 0, wrong_ctx = 0;

		/* --- batch queued to a cpu that is then taken down --------- */
		rejected = queue_round(bh_wq, bh_hi_wq, victim);
		WQT_CHECK(rejected == 0, "round %d: %d idle items rejected",
			  round, rejected);

		ret = remove_cpu(victim);
		if (ret) {
			WQT_DIAG("remove_cpu(%d) returned %d, hotplug phase skipped",
				 victim, ret);
			wait_event_timeout(waitq, atomic_read(&ran) == NR_TOTAL,
					   msecs_to_jiffies(WAIT_MS));
			break;
		}

		/*
		 * workqueue_softirq_dead() drained both BH pools synchronously
		 * before remove_cpu() returned, so nothing may be outstanding.
		 * Do not flush here: a barrier inserted into a dead BH pool
		 * would never complete.
		 */
		WQT_CHECK(atomic_read(&ran) == NR_TOTAL,
			  "round %d: %d/%d BH items ran by the time cpu%d was down",
			  round, atomic_read(&ran), NR_TOTAL, victim);

		for (i = 0; i < NR_TOTAL; i++) {
			if (items[i].runs != 1)
				wrong_runs++;
			else if (!items[i].softirq)
				wrong_ctx++;
			else if (items[i].ran_on != victim)
				off_cpu++;
		}
		total_off_cpu += off_cpu;

		WQT_CHECK(wrong_runs == 0,
			  "round %d: %d items did not run exactly once", round,
			  wrong_runs);
		WQT_CHECK(wrong_ctx == 0,
			  "round %d: %d drained items ran outside softirq", round,
			  wrong_ctx);
		WQT_DIAG("round %d: %d/%d items drained off cpu%d", round,
			 off_cpu, NR_TOTAL, victim);

		ret = add_cpu(victim);
		if (ret) {
			WQT_FAIL("add_cpu(%d) returned %d", victim, ret);
			break;
		}
		done_rounds++;

		/* --- the re-onlined cpu takes BH work on its own pools again */
		rejected = queue_round(bh_wq, bh_hi_wq, victim);
		WQT_CHECK(rejected == 0,
			  "round %d: %d items rejected after re-online", round,
			  rejected);
		wait_event_timeout(waitq, atomic_read(&ran) == NR_TOTAL,
				   msecs_to_jiffies(WAIT_MS));
		for (i = 0; i < NR_TOTAL; i++)
			flush_work(&items[i].work);

		WQT_CHECK(atomic_read(&ran) == NR_TOTAL,
			  "round %d: %d/%d items ran after cpu%d came back",
			  round, atomic_read(&ran), NR_TOTAL, victim);

		off_cpu = 0;
		for (i = 0; i < NR_TOTAL; i++)
			if (items[i].ran_on != victim)
				off_cpu++;
		WQT_CHECK(off_cpu == 0,
			  "round %d: %d items left cpu%d after it was re-onlined",
			  round, off_cpu, victim);
	}

	/*
	 * Everything above holds whether or not the drain was reached; this is
	 * what says it was.  If it ever comes out zero the assertions above
	 * stopped covering workqueue_softirq_dead() and the batch needs to be
	 * bigger or slower, so it is a failure rather than a diagnostic.
	 */
	WQT_CHECK(total_off_cpu > 0,
		  "no item was drained by workqueue_softirq_dead() in %d rounds",
		  done_rounds);
	WQT_DIAG("cpu%d: %d rounds, %d/%d items drained off-cpu by workqueue_softirq_dead()",
		 victim, done_rounds, total_off_cpu, done_rounds * NR_TOTAL);

out:
	if (bh_hi_wq)
		destroy_workqueue(bh_hi_wq);
	if (bh_wq)
		destroy_workqueue(bh_wq);
	kfree(items);

	return WQT_FINISH();
}
module_init(wqt_35_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: WQ_BH pools drained on cpu offline");
MODULE_LICENSE("GPL");
