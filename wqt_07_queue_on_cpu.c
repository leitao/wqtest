// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_07_queue_on_cpu - per-cpu binding via queue_work_on().
 *
 * On a per-cpu workqueue, queue_work_on(cpu, ...) must run the work item on
 * that specific CPU.  We queue one item per online CPU and verify each ran on
 * its target CPU.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include "wqtest.h"

struct cpu_item {
	struct work_struct work;
	int ran_on;
};

static struct cpu_item *citems;

static void cpu_fn(struct work_struct *w)
{
	struct cpu_item *it = container_of(w, struct cpu_item, work);

	it->ran_on = smp_processor_id();
}

static int __init wqt_07_init(void)
{
	struct workqueue_struct *wq;
	int cpu, n = 0, mism = 0;

	WQT_INIT(7, "queue_on_cpu");

	citems = kcalloc(nr_cpu_ids, sizeof(*citems), GFP_KERNEL);
	if (!citems) {
		WQT_FAIL("kcalloc failed");
		return WQT_FINISH();
	}

	wq = alloc_workqueue("wqt07", WQ_PERCPU, 0);
	if (!wq) {
		WQT_FAIL("alloc_workqueue failed");
		kfree(citems);
		return WQT_FINISH();
	}

	for_each_online_cpu(cpu) {
		citems[cpu].ran_on = -1;
		INIT_WORK(&citems[cpu].work, cpu_fn);
		WQT_CHECK(queue_work_on(cpu, wq, &citems[cpu].work),
			  "queue_work_on(cpu=%d) rejected", cpu);
		n++;
	}

	flush_workqueue(wq);

	for_each_online_cpu(cpu) {
		if (citems[cpu].ran_on != cpu) {
			mism++;
			WQT_DIAG("cpu %d: work ran on cpu %d", cpu,
				 citems[cpu].ran_on);
		}
	}

	WQT_CHECK(mism == 0, "%d/%d items did not run on their target cpu",
		  mism, n);
	WQT_DIAG("verified per-cpu binding across %d online cpus", n);
	if (n < 2)
		WQT_DIAG("only %d online cpu(s): affinity coverage limited", n);

	destroy_workqueue(wq);
	kfree(citems);

	return WQT_FINISH();
}
module_init(wqt_07_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: queue_work_on per-cpu binding");
MODULE_LICENSE("GPL");
