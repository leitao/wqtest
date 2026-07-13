// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_09_torture - concurrent stress with accounting invariants.
 *
 * Spawns a pool of kthreads that, for a fixed duration, randomly queue, cancel,
 * flush and drain a shared pool of work items across several workqueues
 * (per-cpu, unbound, highpri, bounded max_active) plus an ordered workqueue.
 *
 * Every successful queue_work() maps to exactly one outcome: the item either
 * executes, or is removed by a cancel_work_sync() that returns true.  So after
 * the threads stop and all queues drain, the invariant
 *
 *	accepted == executed + cancelled
 *
 * must hold.  The ordered workqueue must also never run two items at once.  The
 * debug kernel (KASAN, lockdep, debugobjects, WQ_WATCHDOG) provides the rest of
 * the oracle: use-after-free, deadlocks, double-init/free and stalls all fault.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include "wqtest.h"

#define POOL_SIZE	256
#define OPOOL_SIZE	32
#define NR_WQ		4

static int duration_ms = 3000;
module_param(duration_ms, int, 0444);
MODULE_PARM_DESC(duration_ms, "torture duration in ms (default 3000)");

static int nthreads;
module_param(nthreads, int, 0444);
MODULE_PARM_DESC(nthreads, "torture threads (default min(online_cpus,16))");

struct titem { struct work_struct work; };
struct oitem { struct work_struct work; };

static struct titem pool[POOL_SIZE];
static struct oitem opool[OPOOL_SIZE];
static struct workqueue_struct *wqs[NR_WQ];
static struct workqueue_struct *owq;

static atomic_t accepted, executed, cancelled;
static atomic_t olive, ooverlap;

struct targ {
	struct task_struct *task;
	unsigned long end;
	u32 seed;
};

static void t_fn(struct work_struct *w)
{
	atomic_inc(&executed);
}

static void o_fn(struct work_struct *w)
{
	if (atomic_inc_return(&olive) != 1)
		atomic_set(&ooverlap, 1);
	udelay(20);
	atomic_dec(&olive);
}

static int torture_thread(void *arg)
{
	struct targ *ta = arg;
	u32 s = ta->seed;

	while (!kthread_should_stop() && time_before(jiffies, ta->end)) {
		u32 r;
		int op, idx, wi;

		s = s * 1103515245u + 12345u;
		r = s >> 8;
		op = r % 10;
		idx = (r >> 4) % POOL_SIZE;
		wi = (r >> 12) % NR_WQ;

		switch (op) {
		case 0: case 1: case 2: case 3:		/* queue */
			if (queue_work(wqs[wi], &pool[idx].work))
				atomic_inc(&accepted);
			break;
		case 4: case 5:				/* cancel */
			if (cancel_work_sync(&pool[idx].work))
				atomic_inc(&cancelled);
			break;
		case 6:					/* flush one item */
			flush_work(&pool[idx].work);
			break;
		case 7: case 8:				/* ordered chaos */
			queue_work(owq, &opool[(r >> 4) % OPOOL_SIZE].work);
			break;
		case 9:					/* drain a whole wq */
			flush_workqueue(wqs[wi]);
			break;
		}
		cond_resched();
	}

	/* Keep the module text valid until kthread_stop() collects us. */
	while (!kthread_should_stop())
		schedule();

	return 0;
}

static int __init wqt_09_init(void)
{
	struct targ *targs = NULL;
	unsigned long end;
	int i, nthr, acc, ex, can;

	WQT_INIT(9, "torture");

	atomic_set(&accepted, 0);
	atomic_set(&executed, 0);
	atomic_set(&cancelled, 0);
	atomic_set(&olive, 0);
	atomic_set(&ooverlap, 0);

	for (i = 0; i < POOL_SIZE; i++)
		INIT_WORK(&pool[i].work, t_fn);
	for (i = 0; i < OPOOL_SIZE; i++)
		INIT_WORK(&opool[i].work, o_fn);

	wqs[0] = alloc_workqueue("wqt09_pcpu", WQ_PERCPU, 0);
	wqs[1] = alloc_workqueue("wqt09_unb", WQ_UNBOUND, 0);
	wqs[2] = alloc_workqueue("wqt09_hi", WQ_PERCPU | WQ_HIGHPRI, 0);
	wqs[3] = alloc_workqueue("wqt09_m4", WQ_UNBOUND, 4);
	owq = alloc_ordered_workqueue("wqt09_ord", 0);
	for (i = 0; i < NR_WQ; i++) {
		if (!wqs[i]) {
			WQT_FAIL("alloc_workqueue[%d] failed", i);
			goto out;
		}
	}
	if (!owq) {
		WQT_FAIL("alloc_ordered_workqueue failed");
		goto out;
	}

	nthr = nthreads ? nthreads : min(num_online_cpus(), 16);
	targs = kcalloc(nthr, sizeof(*targs), GFP_KERNEL);
	if (!targs) {
		WQT_FAIL("kcalloc failed");
		goto out;
	}

	WQT_DIAG("running %d threads for %dms", nthr, duration_ms);
	end = jiffies + msecs_to_jiffies(duration_ms);

	for (i = 0; i < nthr; i++) {
		targs[i].end = end;
		targs[i].seed = 0x9e3779b9u ^ (i * 2654435761u) ^ (u32)jiffies;
		targs[i].task = kthread_run(torture_thread, &targs[i],
					    "wqt09/%d", i);
		if (IS_ERR(targs[i].task)) {
			WQT_FAIL("kthread_run failed at %d", i);
			targs[i].task = NULL;
			nthr = i;
			break;
		}
	}

	msleep(duration_ms + 100);

	for (i = 0; i < nthr; i++)
		if (targs[i].task)
			kthread_stop(targs[i].task);

	/* Drain everything so all accepted work reaches a terminal state. */
	for (i = 0; i < NR_WQ; i++)
		flush_workqueue(wqs[i]);
	flush_workqueue(owq);

	acc = atomic_read(&accepted);
	ex = atomic_read(&executed);
	can = atomic_read(&cancelled);
	WQT_DIAG("accepted=%d executed=%d cancelled=%d", acc, ex, can);

	WQT_CHECK(acc == ex + can,
		  "accounting mismatch: accepted=%d != executed+cancelled=%d",
		  acc, ex + can);
	WQT_CHECK(ex > 0, "no work executed");
	WQT_CHECK(atomic_read(&olive) == 0, "ordered live counter leaked: %d",
		  atomic_read(&olive));
	WQT_CHECK(atomic_read(&ooverlap) == 0,
		  "ordered workqueue ran items concurrently under torture");

out:
	for (i = 0; i < NR_WQ; i++)
		if (wqs[i])
			destroy_workqueue(wqs[i]);
	if (owq)
		destroy_workqueue(owq);
	kfree(targs);

	return WQT_FINISH();
}
module_init(wqt_09_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: concurrent torture with accounting");
MODULE_LICENSE("GPL");
