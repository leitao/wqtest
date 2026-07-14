// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_17_probe_on_node - run one-shot init on a specific CPU/NUMA node.
 *
 * Models how drivers run initialisation on a CPU local to a device so its
 * allocations land on the right NUMA node:
 *   - drivers/pci/pci-driver.c:pci_call_probe() picks a CPU of the device's
 *     node, queue_work_on()s an on-stack work there and flush_work()s it,
 *     reading the probe's return value back out of the work context;
 *   - drivers/cpufreq/powernow-k8.c uses work_on_cpu(pol->cpu, fn, &pta) to
 *     run the frequency setter on the policy's CPU and get its return value.
 *
 * We reproduce both: a node-targeted on-stack probe (must run on a CPU of the
 * requested node and propagate its return), and work_on_cpu() (must run on the
 * requested CPU and return the callback's value).
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include "wqtest.h"

#define PROBE_MAGIC 0x513a0000L

struct probe_ctx {
	struct work_struct work;
	int ran_cpu;
	long ret;
};

static void probe_fn(struct work_struct *w)
{
	struct probe_ctx *p = container_of(w, struct probe_ctx, work);

	p->ran_cpu = smp_processor_id();
	p->ret = PROBE_MAGIC | p->ran_cpu;	/* stands in for a probe result */
}

static long won_fn(void *arg)
{
	int want = (int)(long)arg;

	if (smp_processor_id() != want)
		return -1;
	return PROBE_MAGIC | want;
}

static int __init wqt_17_init(void)
{
	struct workqueue_struct *wq;
	int node, cpu, nodes = 0;

	WQT_INIT(17, "probe_on_node");

	wq = alloc_workqueue("wqt17", WQ_PERCPU, 0);
	if (!wq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}

	/* 1: pci_call_probe() idiom -- node-local on-stack probe + flush. */
	for_each_online_node(node) {
		struct probe_ctx p = { .ran_cpu = -1, .ret = 0 };

		cpu = cpumask_any_and(cpumask_of_node(node), cpu_online_mask);
		if (cpu >= nr_cpu_ids)
			continue;		/* node has no online CPU */

		INIT_WORK_ONSTACK(&p.work, probe_fn);
		queue_work_on(cpu, wq, &p.work);
		flush_work(&p.work);
		destroy_work_on_stack(&p.work);

		WQT_CHECK(p.ran_cpu >= 0 && cpu_to_node(p.ran_cpu) == node,
			  "node %d: probe ran on cpu %d (node %d)", node,
			  p.ran_cpu, p.ran_cpu >= 0 ? cpu_to_node(p.ran_cpu) : -1);
		WQT_CHECK(p.ret == (PROBE_MAGIC | p.ran_cpu),
			  "node %d: return not propagated (0x%lx)", node, p.ret);
		nodes++;
	}
	WQT_CHECK(nodes > 0, "no online node had an online cpu");
	WQT_DIAG("ran node-local probe on %d node(s)", nodes);

	/* 2: work_on_cpu() idiom -- run on the target cpu, return its value. */
	for_each_online_cpu(cpu) {
		long r = work_on_cpu(cpu, won_fn, (void *)(long)cpu);

		WQT_CHECK(r == (PROBE_MAGIC | cpu),
			  "work_on_cpu(cpu=%d) ran elsewhere / bad ret (0x%lx)",
			  cpu, r);
	}

	destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_17_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: run init on a target cpu/node (pci/cpufreq idiom)");
MODULE_LICENSE("GPL");
