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
 * That makes the contract testable: by the time remove_cpu() returns, every BH
 * item queued to that cpu has already run -- and the ones the drain took ran on
 * some *other* cpu, which is the one place a BH item does not run where it was
 * queued.  The off-cpu count is what says the drain was reached rather than the
 * victim having simply finished its backlog first.
 *
 * Two batches are queued per offline.  The first comes from an ONLINE hotplug
 * teardown callback on the victim cpu, with softirqs disabled so that
 * local_bh_enable() consumes only the BH worker's budget.  That used to leave
 * a backlog for workqueue_softirq_dead(), but since commit 4a3b51aab6e2
 * ("smpboot: Don't park the thread if work is pending") ksoftirqd works it
 * off on the victim before it parks at CPUHP_AP_SMPBOOT_THREADS.
 *
 * So the second batch is queued from another cpu once the victim's ksoftirqd
 * has parked.  From then on only irq exits and local_bh_enable() run the
 * victim's softirqs, each within the BH worker's budget, and most of that
 * batch is left for workqueue_softirq_dead().  Each of its items is queued
 * with preemption disabled after checking that the victim is still online:
 * the offline needs stop_machine(), which cannot run while this cpu is
 * non-preemptible, and kicking the BH pool of an offline cpu WARNs.
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
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/wait.h>
#include "wqtest.h"

#define NR_ITEMS	512		/* per BH workqueue, per round */
#define NR_TOTAL	(2 * NR_ITEMS)
#define ITEM_US		100		/* leave work after the BH worker budget */
#define WAIT_MS		10000

static int rounds = 3;
module_param(rounds, int, 0444);
MODULE_PARM_DESC(rounds, "offline/online rounds (default 3)");

struct hp_item {
	struct work_struct work;
	atomic_t *counter;
	int ran_on;
	int runs;
	bool softirq;
	bool queued;		/* late batch only */
};

static struct hp_item *items, *late;
static struct workqueue_struct *bh_wq, *bh_hi_wq;
static struct task_struct *victim_ksoftirqd;
static atomic_t ran, late_ran;
static int late_queued, late_rejected;
static atomic_t hp_calls;
static int hp_rejected;
static int hp_cpu = -1;
static int hp_state = CPUHP_INVALID;
static int victim;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void hp_fn(struct work_struct *w)
{
	struct hp_item *it = container_of(w, struct hp_item, work);

	it->ran_on = smp_processor_id();
	it->softirq = in_serving_softirq();
	it->runs++;
	udelay(ITEM_US);	/* keep the pool backlogged while the cpu dies */

	atomic_inc(it->counter);
	wake_up(&waitq);
}

static int queue_round(int cpu)
{
	int i, rejected = 0;

	atomic_set(&ran, 0);
	for (i = 0; i < NR_TOTAL; i++) {
		items[i].ran_on = -1;
		items[i].runs = 0;
		items[i].softirq = false;
	}

	for (i = 0; i < NR_ITEMS; i++)
		if (!queue_work_on(cpu, bh_wq, &items[i].work))
			rejected++;
	for (i = 0; i < NR_ITEMS; i++)
		if (!queue_work_on(cpu, bh_hi_wq, &items[NR_ITEMS + i].work))
			rejected++;

	return rejected;
}

static int queue_offline_round(unsigned int cpu)
{
	if (cpu != victim)
		return 0;

	atomic_inc(&hp_calls);
	hp_cpu = smp_processor_id();

	local_bh_disable();
	hp_rejected = queue_round(cpu);
	local_bh_enable();

	return 0;
}

/* ksoftirqd is not exported; its task survives offline/online */
static struct task_struct *find_ksoftirqd(int cpu)
{
	struct task_struct *p, *found = NULL;
	char comm[TASK_COMM_LEN];

	snprintf(comm, sizeof(comm), "ksoftirqd/%d", cpu);
	rcu_read_lock();
	for_each_process(p) {
		if ((p->flags & PF_KTHREAD) && !strcmp(p->comm, comm)) {
			found = get_task_struct(p);
			break;
		}
	}
	rcu_read_unlock();

	return found;
}

static void reset_late(void)
{
	int i;

	atomic_set(&late_ran, 0);
	late_queued = 0;
	late_rejected = 0;
	for (i = 0; i < NR_TOTAL; i++) {
		late[i].ran_on = -1;
		late[i].runs = 0;
		late[i].softirq = false;
		late[i].queued = false;
	}
}

/* Runs on another cpu: queue the late batch once the victim's ksoftirqd parks. */
static int late_pump(void *unused)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(WAIT_MS);
	int i;

	while (READ_ONCE(victim_ksoftirqd->__state) != TASK_PARKED) {
		if (kthread_should_stop() || time_after(jiffies, deadline))
			goto idle;
		usleep_range(10, 20);
	}

	for (i = 0; i < NR_TOTAL; i++) {
		struct workqueue_struct *wq = (i & 1) ? bh_hi_wq : bh_wq;
		bool online, queued = false;

		preempt_disable();
		online = cpu_online(victim);
		if (online)
			queued = queue_work_on(victim, wq, &late[i].work);
		preempt_enable();

		if (!online)
			break;
		if (queued) {
			late[i].queued = true;
			late_queued++;
		} else {
			late_rejected++;
		}
	}

idle:
	/* kthread_stop() needs this thread alive */
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);

	return 0;
}

static int __init wqt_35_init(void)
{
	int round, i, ret, rejected;
	int total_off_cpu = 0, total_queued = 0, done_rounds = 0;

	WQT_INIT(35, "bh_hotplug");

	if (!IS_ENABLED(CONFIG_HOTPLUG_CPU) || num_online_cpus() < 2) {
		WQT_DIAG("no CONFIG_HOTPLUG_CPU or single cpu, test skipped");
		return WQT_FINISH();
	}

	items = kcalloc(NR_TOTAL, sizeof(*items), GFP_KERNEL);
	late = kcalloc(NR_TOTAL, sizeof(*late), GFP_KERNEL);
	if (!items || !late) {
		WQT_FAIL("kcalloc failed");
		kfree(items);
		kfree(late);
		return WQT_FINISH();
	}

	bh_wq = alloc_workqueue("wqt35_bh", WQ_BH | WQ_PERCPU, 0);
	bh_hi_wq = alloc_workqueue("wqt35_bh_hi",
				   WQ_BH | WQ_HIGHPRI | WQ_PERCPU, 0);
	if (!bh_wq || !bh_hi_wq) {
		WQT_FAIL("alloc_workqueue(WQ_BH) failed");
		goto out;
	}

	for (i = 0; i < NR_TOTAL; i++) {
		INIT_WORK(&items[i].work, hp_fn);
		items[i].counter = &ran;
		INIT_WORK(&late[i].work, hp_fn);
		late[i].counter = &late_ran;
	}

	victim = cpumask_last(cpu_online_mask);
	victim_ksoftirqd = find_ksoftirqd(victim);
	if (!victim_ksoftirqd)
		WQT_DIAG("ksoftirqd/%d not found, no late batch", victim);

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"wqtest/bh_hotplug:online",
					NULL, queue_offline_round);
	if (ret < 0) {
		WQT_FAIL("cpuhp_setup_state_nocalls() returned %d", ret);
		goto out;
	}
	hp_state = ret;

	for (round = 0; round < rounds; round++) {
		int off_cpu = 0, late_off_cpu = 0, wrong_runs = 0, wrong_ctx = 0;
		struct task_struct *pump = NULL;

		atomic_set(&hp_calls, 0);
		hp_rejected = 0;
		hp_cpu = -1;

		reset_late();
		if (victim_ksoftirqd) {
			pump = kthread_create(late_pump, NULL, "wqt35_pump");
			if (IS_ERR(pump)) {
				WQT_FAIL("kthread_create() returned %ld",
					 PTR_ERR(pump));
				break;
			}
			kthread_bind(pump, cpumask_any_but(cpu_online_mask, victim));
			wake_up_process(pump);
		}

		/* --- queued from the victim cpu as it starts going down ---- */
		ret = remove_cpu(victim);
		if (pump)
			kthread_stop(pump);
		if (ret) {
			WQT_DIAG("remove_cpu(%d) returned %d, hotplug phase skipped",
				 victim, ret);
			wait_event_timeout(waitq, atomic_read(&ran) == NR_TOTAL &&
					   atomic_read(&late_ran) == late_queued,
					   msecs_to_jiffies(WAIT_MS));
			break;
		}

		WQT_CHECK(atomic_read(&hp_calls) == 1,
			  "round %d: hotplug callback ran %d times", round,
			  atomic_read(&hp_calls));
		WQT_CHECK(hp_cpu == victim,
			  "round %d: hotplug callback ran on cpu%d, expected cpu%d",
			  round, hp_cpu, victim);
		WQT_CHECK(hp_rejected == 0,
			  "round %d: %d teardown items rejected",
			  round, hp_rejected);

		/*
		 * workqueue_softirq_dead() drained both BH pools synchronously
		 * before remove_cpu() returned, so nothing may be outstanding.
		 * Do not flush here: a barrier inserted into a dead BH pool
		 * would never complete.
		 */
		WQT_CHECK(atomic_read(&ran) == NR_TOTAL,
			  "round %d: %d/%d BH items ran by the time cpu%d was down",
			  round, atomic_read(&ran), NR_TOTAL, victim);
		WQT_CHECK(late_rejected == 0,
			  "round %d: %d late items rejected", round, late_rejected);
		WQT_CHECK(atomic_read(&late_ran) == late_queued,
			  "round %d: %d/%d late BH items ran by the time cpu%d was down",
			  round, atomic_read(&late_ran), late_queued, victim);

		for (i = 0; i < NR_TOTAL; i++) {
			if (items[i].runs != 1)
				wrong_runs++;
			else if (!items[i].softirq)
				wrong_ctx++;
			else if (items[i].ran_on != victim)
				off_cpu++;

			if (late[i].runs != late[i].queued)
				wrong_runs++;
			else if (!late[i].queued)
				continue;
			else if (!late[i].softirq)
				wrong_ctx++;
			else if (late[i].ran_on != victim)
				late_off_cpu++;
		}
		total_off_cpu += off_cpu + late_off_cpu;
		total_queued += NR_TOTAL + late_queued;

		WQT_CHECK(wrong_runs == 0,
			  "round %d: %d items did not run exactly once", round,
			  wrong_runs);
		WQT_CHECK(wrong_ctx == 0,
			  "round %d: %d drained items ran outside softirq", round,
			  wrong_ctx);
		WQT_DIAG("round %d: drained off cpu%d: %d/%d of the teardown batch, %d/%d of the late batch",
			 round, victim, off_cpu, NR_TOTAL, late_off_cpu,
			 late_queued);

		ret = add_cpu(victim);
		if (ret) {
			WQT_FAIL("add_cpu(%d) returned %d", victim, ret);
			break;
		}
		done_rounds++;

		/* --- the re-onlined cpu takes BH work on its own pools again */
		rejected = queue_round(victim);
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
	 * stopped covering workqueue_softirq_dead(): the late batch was never
	 * queued after ksoftirqd parked, or the batches need to be bigger or
	 * slower, so it is a failure rather than a diagnostic.
	 */
	WQT_CHECK(total_off_cpu > 0,
		  "no item was drained by workqueue_softirq_dead() in %d rounds",
		  done_rounds);
	WQT_DIAG("cpu%d: %d rounds, %d/%d items drained off-cpu by workqueue_softirq_dead()",
		 victim, done_rounds, total_off_cpu, total_queued);

out:
	if (hp_state != CPUHP_INVALID)
		cpuhp_remove_state_nocalls(hp_state);
	if (bh_hi_wq)
		destroy_workqueue(bh_hi_wq);
	if (bh_wq)
		destroy_workqueue(bh_wq);
	if (victim_ksoftirqd)
		put_task_struct(victim_ksoftirqd);
	kfree(items);
	kfree(late);

	return WQT_FINISH();
}
module_init(wqt_35_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: WQ_BH pools drained on cpu offline");
MODULE_LICENSE("GPL");
