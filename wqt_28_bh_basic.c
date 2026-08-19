// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_28_bh_basic - the WQ_BH execution contract.
 *
 * A BH workqueue is a convenience interface to softirq: it is always per-cpu,
 * takes 0 max_active, allows only WQ_HIGHPRI on top of WQ_BH, and runs every
 * item in the queueing cpu's softirq context.  This test pins that down for the
 * two workqueues a caller can allocate (WQ_BH and WQ_BH | WQ_HIGHPRI) and for
 * the two the kernel provides (system_bh_wq, system_bh_highpri_wq): each item
 * runs exactly once, from softirq and not from hardirq, on the cpu it was
 * queued to.
 *
 * queue_work() without a cpu is checked separately, because "the queueing cpu"
 * is the contract there: __queue_work() resolves WORK_CPU_UNBOUND to the
 * current cpu for a per-cpu wq, and a BH wq is always per-cpu.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include "wqtest.h"

#define WAIT_MS		5000

struct bh_probe {
	struct work_struct work;
	int ran_on;
	int runs;
	bool softirq;
	bool hardirq;
};

static struct bh_probe *probes;
static atomic_t ran;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void probe_fn(struct work_struct *w)
{
	struct bh_probe *p = container_of(w, struct bh_probe, work);

	/*
	 * A BH item runs from the softirq of the cpu it was queued to, so
	 * preemption is off here and the reading is exact -- unlike an unbound
	 * worker, which wqt_24 has to read with raw_smp_processor_id().
	 */
	p->ran_on = smp_processor_id();
	p->softirq = in_serving_softirq();
	p->hardirq = in_hardirq();
	p->runs++;

	atomic_inc(&ran);
	wake_up(&waitq);
}

static int __init wqt_28_init(void)
{
	struct workqueue_struct *bh_wq, *bh_hi_wq, *wq;
	struct bh_probe *p;
	int v, cpu, n = 0;

	WQT_INIT(28, "bh_basic");

	probes = kcalloc(nr_cpu_ids, sizeof(*probes), GFP_KERNEL);
	if (!probes) {
		WQT_FAIL("kcalloc failed");
		return WQT_FINISH();
	}
	for_each_possible_cpu(cpu)
		INIT_WORK(&probes[cpu].work, probe_fn);

	/* max_active must be 0 and WQ_HIGHPRI is the only extra flag allowed. */
	bh_wq = alloc_workqueue("wqt28_bh", WQ_BH | WQ_PERCPU, 0);
	bh_hi_wq = alloc_workqueue("wqt28_bh_hi",
				   WQ_BH | WQ_HIGHPRI | WQ_PERCPU, 0);
	if (!bh_wq || !bh_hi_wq) {
		WQT_FAIL("alloc_workqueue(WQ_BH) failed");
		goto out;
	}

	for (v = 0; v < 4; v++) {
		static const char * const names[] = {
			"alloc_bh", "alloc_bh_highpri",
			"system_bh_wq", "system_bh_highpri_wq",
		};
		int wrong_ctx = 0, wrong_cpu = 0, wrong_runs = 0;

		switch (v) {
		case 0: wq = bh_wq;			break;
		case 1: wq = bh_hi_wq;			break;
		case 2: wq = system_bh_wq;		break;
		default: wq = system_bh_highpri_wq;	break;
		}

		atomic_set(&ran, 0);
		n = 0;
		for_each_online_cpu(cpu) {
			probes[cpu].ran_on = -1;
			probes[cpu].runs = 0;
			WQT_CHECK(queue_work_on(cpu, wq, &probes[cpu].work),
				  "%s: queue_work_on(cpu=%d) rejected",
				  names[v], cpu);
			n++;
		}

		wait_event_timeout(waitq, atomic_read(&ran) == n,
				   msecs_to_jiffies(WAIT_MS));
		for_each_online_cpu(cpu)
			flush_work(&probes[cpu].work);

		WQT_CHECK(atomic_read(&ran) == n, "%s: %d/%d items ran",
			  names[v], atomic_read(&ran), n);

		for_each_online_cpu(cpu) {
			p = &probes[cpu];

			if (p->runs != 1) {
				wrong_runs++;
				WQT_DIAG("%s: cpu%d item ran %d times",
					 names[v], cpu, p->runs);
				continue;
			}
			if (!p->softirq || p->hardirq) {
				wrong_ctx++;
				WQT_DIAG("%s: cpu%d ran softirq=%d hardirq=%d",
					 names[v], cpu, p->softirq, p->hardirq);
			}
			if (p->ran_on != cpu) {
				wrong_cpu++;
				WQT_DIAG("%s: cpu%d item ran on cpu%d",
					 names[v], cpu, p->ran_on);
			}
		}

		WQT_CHECK(wrong_runs == 0, "%s: %d/%d items did not run exactly once",
			  names[v], wrong_runs, n);
		WQT_CHECK(wrong_ctx == 0, "%s: %d/%d items ran outside softirq",
			  names[v], wrong_ctx, n);
		WQT_CHECK(wrong_cpu == 0, "%s: %d/%d items ran on the wrong cpu",
			  names[v], wrong_cpu, n);
	}

	/* queue_work() lands on the queueing cpu. */
	p = &probes[0];
	p->ran_on = -1;
	p->runs = 0;
	cpu = get_cpu();
	WQT_CHECK(queue_work(bh_wq, &p->work), "queue_work rejected");
	put_cpu();
	flush_work(&p->work);
	WQT_CHECK(p->ran_on == cpu, "queue_work from cpu%d ran on cpu%d", cpu,
		  p->ran_on);

	/* The same item is re-queueable once it has run, and flushing an idle
	 * item returns false because there was nothing to wait for.
	 */
	WQT_CHECK(!flush_work(&p->work), "flush_work of an idle BH item returned true");
	WQT_CHECK(queue_work(bh_wq, &p->work), "re-queue of a completed BH item rejected");
	flush_work(&p->work);
	WQT_CHECK(p->runs == 2, "re-queued BH item ran %d times, expected 2",
		  p->runs);

	flush_workqueue(bh_wq);
	WQT_DIAG("4 BH workqueues checked over %d online cpus", n);

out:
	if (bh_hi_wq)
		destroy_workqueue(bh_hi_wq);
	if (bh_wq)
		destroy_workqueue(bh_wq);
	kfree(probes);

	return WQT_FINISH();
}
module_init(wqt_28_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: WQ_BH basic execution contract");
MODULE_LICENSE("GPL");
