// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_26_pool_backing - which worker pool actually backs a workqueue.
 *
 * A workqueue's pool_workqueue points at either one of the static per-cpu
 * pools or a hashed unbound pool, and that choice is what WQ_PERCPU and
 * WQ_UNBOUND select.  The other tests check where work runs; this one checks
 * what it runs on, because a workqueue can land on the wrong kind of pool and
 * still produce correct-looking results: an unbound pool serving a WQ_PERCPU
 * queue runs the item on the right cpu whenever the scheduler happens to leave
 * it there, and only differs in concurrency management and worker identity.
 *
 * The oracle is the worker's name.  format_worker_id() spells a per-cpu pool
 * worker "kworker/<cpu>:<id>", suffixed "H" when the pool's nice is negative,
 * and an unbound pool worker "kworker/u<pool_id>:<id>".  So the item can read
 * current->comm and say which pool served it, and task_nice() says which of
 * the two per-cpu pools on that cpu it was.
 *
 * BH workqueues run their items from softirq context on the interrupted task,
 * so comm and nice say nothing there; those variants are checked for softirq
 * context and cpu instead.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/wait.h>
#include "wqtest.h"

#define WAIT_MS		5000

struct probe {
	struct work_struct work;
	int ran_on;
	int nice;
	bool softirq;
	char comm[TASK_COMM_LEN];
};

static struct probe *probes, *bh_probes;
static atomic_t ran;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void probe_fn(struct work_struct *w)
{
	struct probe *p = container_of(w, struct probe, work);

	/* raw_: an unbound worker is not pinned; see wqt_24. */
	p->ran_on = raw_smp_processor_id();
	p->softirq = in_serving_softirq();
	p->nice = task_nice(current);
	strscpy(p->comm, current->comm, sizeof(p->comm));

	atomic_inc(&ran);
	wake_up(&waitq);
}

/* True when @comm names a worker of an unbound pool rather than a per-cpu one. */
static bool comm_is_unbound_worker(const char *comm)
{
	return !strncmp(comm, "kworker/u", 9);
}

/* True when @comm carries the "H" suffix a negative-nice per-cpu pool gets. */
static bool comm_is_highpri_worker(const char *comm)
{
	size_t len = strlen(comm);

	return len && comm[len - 1] == 'H';
}

static int __init wqt_26_init(void)
{
	static const struct {
		unsigned int flags;
		const char *name;
		bool percpu_pool;	/* expect a static per-cpu pool */
		bool highpri;		/* expect the negative-nice pool */
		bool bh;		/* runs from softirq */
	} variants[] = {
		{ WQ_PERCPU,				"percpu",
		  true,  false, false },
		{ WQ_PERCPU | WQ_HIGHPRI,		"percpu_highpri",
		  true,  true,  false },
		{ WQ_UNBOUND,				"unbound",
		  false, false, false },
		{ WQ_UNBOUND | WQ_HIGHPRI,		"unbound_highpri",
		  false, true,  false },
		{ WQ_BH | WQ_PERCPU,			"bh",
		  true,  false, true },
		{ WQ_BH | WQ_HIGHPRI | WQ_PERCPU,	"bh_highpri",
		  true,  true,  true },
	};
	int v, cpu, n = 0;

	WQT_INIT(26, "pool_backing");

	probes = kcalloc(nr_cpu_ids, sizeof(*probes), GFP_KERNEL);
	bh_probes = kcalloc(nr_cpu_ids, sizeof(*bh_probes), GFP_KERNEL);
	if (!probes || !bh_probes) {
		WQT_FAIL("kcalloc failed");
		goto out;
	}

	/* Two arrays so the softirq and task lockdep classes stay apart. */
	for_each_possible_cpu(cpu)
		INIT_WORK(&probes[cpu].work, probe_fn);
	for_each_possible_cpu(cpu)
		INIT_WORK(&bh_probes[cpu].work, probe_fn);

	for (v = 0; v < ARRAY_SIZE(variants); v++) {
		struct probe *set = variants[v].bh ? bh_probes : probes;
		const char *name = variants[v].name;
		int wrong_pool = 0, wrong_pri = 0, wrong_ctx = 0, wrong_cpu = 0;
		struct workqueue_struct *wq;

		wq = alloc_workqueue("wqt26_%s", variants[v].flags, 0, name);
		if (!wq) {
			WQT_FAIL("%s: alloc_workqueue failed", name);
			continue;
		}

		atomic_set(&ran, 0);
		n = 0;
		for_each_online_cpu(cpu) {
			set[cpu].ran_on = -1;
			set[cpu].comm[0] = '\0';
			WQT_CHECK(queue_work_on(cpu, wq, &set[cpu].work),
				  "%s: queue_work_on(cpu=%d) rejected", name,
				  cpu);
			n++;
		}

		wait_event_timeout(waitq, atomic_read(&ran) == n,
				   msecs_to_jiffies(WAIT_MS));
		flush_workqueue(wq);

		WQT_CHECK(atomic_read(&ran) == n, "%s: %d/%d items ran", name,
			  atomic_read(&ran), n);

		for_each_online_cpu(cpu) {
			struct probe *p = &set[cpu];

			if (p->ran_on < 0)
				continue;

			if (p->softirq != variants[v].bh) {
				wrong_ctx++;
				WQT_DIAG("%s: cpu%d ran with softirq=%d",
					 name, cpu, p->softirq);
			}

			/*
			 * A per-cpu pool pins its worker, so the cpu is exact.
			 * An unbound worker may be migrated, so it is not
			 * checked.
			 */
			if (variants[v].percpu_pool && p->ran_on != cpu) {
				wrong_cpu++;
				WQT_DIAG("%s: cpu%d item ran on cpu%d", name,
					 cpu, p->ran_on);
			}

			/* comm and nice belong to the interrupted task for BH. */
			if (variants[v].bh)
				continue;

			if (comm_is_unbound_worker(p->comm) ==
			    variants[v].percpu_pool) {
				wrong_pool++;
				WQT_DIAG("%s: cpu%d served by %s", name, cpu,
					 p->comm);
			}

			if (p->nice != (variants[v].highpri ? MIN_NICE : 0)) {
				wrong_pri++;
				WQT_DIAG("%s: cpu%d worker %s has nice %d",
					 name, cpu, p->comm, p->nice);
			}

			/* Only a per-cpu pool spells its priority in the name. */
			if (variants[v].percpu_pool &&
			    comm_is_highpri_worker(p->comm) !=
			    variants[v].highpri) {
				wrong_pri++;
				WQT_DIAG("%s: cpu%d worker %s lacks the expected H suffix",
					 name, cpu, p->comm);
			}
		}

		WQT_CHECK(wrong_ctx == 0, "%s: %d/%d items ran in the wrong context",
			  name, wrong_ctx, n);
		WQT_CHECK(wrong_cpu == 0, "%s: %d/%d items ran on the wrong cpu",
			  name, wrong_cpu, n);
		WQT_CHECK(wrong_pool == 0, "%s: %d/%d items served by the wrong kind of pool",
			  name, wrong_pool, n);
		WQT_CHECK(wrong_pri == 0, "%s: %d/%d items served at the wrong priority",
			  name, wrong_pri, n);

		destroy_workqueue(wq);
	}

	WQT_DIAG("checked %zu workqueue variants over %d online cpus",
		 ARRAY_SIZE(variants), n);

out:
	kfree(bh_probes);
	kfree(probes);

	return WQT_FINISH();
}
module_init(wqt_26_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: per-cpu vs unbound pool backing");
MODULE_LICENSE("GPL");
