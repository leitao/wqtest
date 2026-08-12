// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_23_congested - workqueue_congested() reporting.
 *
 * workqueue_congested(cpu, wq) says whether @wq's pool_workqueue for @cpu has
 * items waiting for an active slot.  An idle workqueue is never congested; one
 * whose only slot is taken by a parked item is congested on the cpu the backlog
 * was queued to; and it clears again once the backlog drains.
 *
 * The lookup reads the RCU-protected wq->cpu_pwq[] slot with nothing but
 * preemption disabled, so on a PROVE_RCU kernel this doubles as coverage of the
 * accessor used there: a mismatched one splats "suspicious
 * rcu_dereference_check() usage" and the runner's splat scan fails the test.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/rcupdate.h>
#include "wqtest.h"

#define NR_FILL	8
#define WAIT_MS	5000

static struct work_struct blocker;
static struct work_struct fillers[NR_FILL];
static DECLARE_COMPLETION(blocker_running);
static DECLARE_COMPLETION(blocker_release);
static atomic_t fill_done;

/* Set before the fillers are queued, read by filler_fn(). */
static struct workqueue_struct *probe_wq;
static int congested_from_work;

static void blocker_fn(struct work_struct *w)
{
	complete(&blocker_running);
	wait_for_completion_timeout(&blocker_release, msecs_to_jiffies(WAIT_MS));
}

static void filler_fn(struct work_struct *w)
{
	/* Same query from worker context.  max_active=1 keeps this serial. */
	if (congested_from_work < 0)
		congested_from_work = workqueue_congested(WORK_CPU_UNBOUND,
							  probe_wq);
	atomic_inc(&fill_done);
}

/* Number of online cpus reporting @wq as congested. */
static int count_congested(struct workqueue_struct *wq)
{
	int cpu, n = 0;

	for_each_online_cpu(cpu)
		if (workqueue_congested(cpu, wq))
			n++;

	return n;
}

static int __init wqt_23_init(void)
{
	static const struct {
		unsigned int flags;
		const char *name;
		bool percpu;
	} variants[] = {
		{ WQ_PERCPU,	"percpu",	true },
		{ WQ_UNBOUND,	"unbound",	false },
	};
	int v, i, n, target, other;

	WQT_INIT(23, "congested");

	target = cpumask_first(cpu_online_mask);
	other = cpumask_next(target, cpu_online_mask);
	if (other >= nr_cpu_ids)
		other = -1;

	for (v = 0; v < ARRAY_SIZE(variants); v++) {
		const char *name = variants[v].name;
		struct workqueue_struct *wq;

		wq = alloc_workqueue("wqt23_%s", variants[v].flags, 1, name);
		if (!wq) {
			WQT_FAIL("%s: alloc_workqueue failed", name);
			continue;
		}

		probe_wq = wq;
		congested_from_work = -1;
		atomic_set(&fill_done, 0);
		reinit_completion(&blocker_running);
		reinit_completion(&blocker_release);

		n = count_congested(wq);
		WQT_CHECK(n == 0, "%s: idle wq congested on %d cpu(s)", name, n);

		INIT_WORK(&blocker, blocker_fn);
		queue_work_on(target, wq, &blocker);
		if (!wait_for_completion_timeout(&blocker_running,
						 msecs_to_jiffies(WAIT_MS))) {
			WQT_FAIL("%s: blocker never started", name);
			complete(&blocker_release);
			destroy_workqueue(wq);
			continue;
		}

		/* The single active slot is taken; everything else waits. */
		for (i = 0; i < NR_FILL; i++) {
			INIT_WORK(&fillers[i], filler_fn);
			queue_work_on(target, wq, &fillers[i]);
		}

		WQT_CHECK(workqueue_congested(target, wq),
			  "%s: %d items backlogged on cpu%d, not reported congested",
			  name, NR_FILL, target);

		/*
		 * Per-cpu pwqs are independent, so an idle cpu is not
		 * congested.  Unbound cpus can share one pwq per pod, so the
		 * same does not hold there.
		 */
		if (variants[v].percpu && other >= 0)
			WQT_CHECK(!workqueue_congested(other, wq),
				  "%s: idle cpu%d reported congested", name,
				  other);

		rcu_read_lock();
		WQT_CHECK(workqueue_congested(target, wq),
			  "%s: congested false inside rcu_read_lock()", name);
		rcu_read_unlock();

		complete(&blocker_release);
		flush_workqueue(wq);

		WQT_CHECK(atomic_read(&fill_done) == NR_FILL,
			  "%s: %d/%d backlogged items ran", name,
			  atomic_read(&fill_done), NR_FILL);
		WQT_CHECK(congested_from_work >= 0,
			  "%s: query from worker context never ran", name);

		n = count_congested(wq);
		WQT_CHECK(n == 0, "%s: drained wq still congested on %d cpu(s)",
			  name, n);

		WQT_DIAG("%s: congested=%d from worker context", name,
			 congested_from_work);

		destroy_workqueue(wq);
	}

	if (other < 0)
		WQT_DIAG("single online cpu: per-cpu isolation check skipped");

	return WQT_FINISH();
}
module_init(wqt_23_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: workqueue_congested reporting");
MODULE_LICENSE("GPL");
