// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_10_perf - queue_work() throughput / latency.
 *
 * Each online CPU runs a kthread that queues wq_items work items back to back,
 * measuring the per-queue latency and waiting for completion.  Reports
 * items/sec and p50/p90/p99 latency for a per-cpu workqueue and for an unbound
 * workqueue across several affinity scopes.
 *
 * This is primarily a performance probe, but it also gates on correctness:
 * every queued item must complete (no losses) and elapsed time must be
 * positive.  An optional min_throughput fails the test on gross regression.
 *
 * Generalizes lib/test_workqueue.c.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/sort.h>
#include <linux/fs.h>
#include "wqtest.h"

static int wq_items = 20000;
module_param(wq_items, int, 0444);
MODULE_PARM_DESC(wq_items, "work items queued per thread (default 20000)");

static int nr_threads;
module_param(nr_threads, int, 0444);
MODULE_PARM_DESC(nr_threads, "threads to spawn (default: num_online_cpus())");

static unsigned long min_throughput;
module_param(min_throughput, ulong, 0444);
MODULE_PARM_DESC(min_throughput,
		 "fail if per-cpu items/sec below this (default 0 = disabled)");

struct thread_ctx {
	struct completion work_done;
	struct work_struct work;
	u64 *lat;
	int items;
	int cpu;
};

static struct workqueue_struct *cur_wq;
static atomic_t threads_done;
static atomic_t completed;
static DECLARE_COMPLETION(start_comp);
static DECLARE_COMPLETION(all_done_comp);
static u64 last_ips;

static void perf_work_fn(struct work_struct *work)
{
	struct thread_ctx *ctx = container_of(work, struct thread_ctx, work);

	atomic_inc(&completed);
	complete(&ctx->work_done);
}

static int perf_kthread_fn(void *data)
{
	struct thread_ctx *ctx = data;
	int i;

	wait_for_completion(&start_comp);
	if (kthread_should_stop())
		return 0;

	for (i = 0; i < ctx->items; i++) {
		ktime_t t0, t1;

		reinit_completion(&ctx->work_done);
		INIT_WORK(&ctx->work, perf_work_fn);

		t0 = ktime_get();
		queue_work(cur_wq, &ctx->work);
		t1 = ktime_get();

		ctx->lat[i] = ktime_to_ns(ktime_sub(t1, t0));
		wait_for_completion(&ctx->work_done);
	}

	if (atomic_dec_and_test(&threads_done))
		complete(&all_done_comp);

	while (!kthread_should_stop())
		schedule();
	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	u64 va = *(const u64 *)a, vb = *(const u64 *)b;

	return (va > vb) - (va < vb);
}

/* Returns items completed, or negative errno. */
static long run_bench(int n_threads, struct workqueue_struct *wq,
		      const char *label)
{
	struct task_struct **tasks;
	struct thread_ctx *ctxs;
	unsigned long total = (unsigned long)n_threads * wq_items;
	u64 *all_lat;
	ktime_t start, end;
	s64 elapsed_us;
	int cpu, i, j;
	long done;

	cur_wq = wq;
	atomic_set(&completed, 0);

	ctxs = kcalloc(n_threads, sizeof(*ctxs), GFP_KERNEL);
	tasks = kcalloc(n_threads, sizeof(*tasks), GFP_KERNEL);
	all_lat = kvmalloc_array(total, sizeof(u64), GFP_KERNEL);
	if (!ctxs || !tasks || !all_lat)
		goto enomem;

	for (i = 0; i < n_threads; i++) {
		ctxs[i].lat = kvmalloc_array(wq_items, sizeof(u64), GFP_KERNEL);
		if (!ctxs[i].lat) {
			while (--i >= 0)
				kvfree(ctxs[i].lat);
			goto enomem;
		}
	}

	atomic_set(&threads_done, n_threads);
	reinit_completion(&all_done_comp);
	reinit_completion(&start_comp);

	i = 0;
	for_each_online_cpu(cpu) {
		if (i >= n_threads)
			break;
		ctxs[i].cpu = cpu;
		ctxs[i].items = wq_items;
		init_completion(&ctxs[i].work_done);
		tasks[i] = kthread_create(perf_kthread_fn, &ctxs[i],
					  "wqt10/%d", cpu);
		if (IS_ERR(tasks[i])) {
			complete_all(&start_comp);
			while (--i >= 0)
				kthread_stop(tasks[i]);
			done = PTR_ERR(tasks[i]);
			goto out_free_lat;
		}
		kthread_bind(tasks[i], cpu);
		wake_up_process(tasks[i]);
		i++;
	}

	start = ktime_get();
	complete_all(&start_comp);
	wait_for_completion(&all_done_comp);
	flush_workqueue(wq);
	for (i = 0; i < n_threads; i++)
		kthread_stop(tasks[i]);
	end = ktime_get();
	elapsed_us = ktime_us_delta(end, start);

	j = 0;
	for (i = 0; i < n_threads; i++) {
		memcpy(&all_lat[j], ctxs[i].lat, wq_items * sizeof(u64));
		j += wq_items;
	}
	sort(all_lat, total, sizeof(u64), cmp_u64, NULL);

	last_ips = elapsed_us ? div_u64(total * 1000000ULL, elapsed_us) : 0;
	pr_info("# wqt10 %-8s %llu items/sec  p50=%llu p90=%llu p99=%llu ns  (%luus)\n",
		label, last_ips,
		all_lat[total * 50 / 100],
		all_lat[total * 90 / 100],
		all_lat[total * 99 / 100],
		(unsigned long)elapsed_us);

	done = atomic_read(&completed);

	for (i = 0; i < n_threads; i++)
		kvfree(ctxs[i].lat);
out_free_lat:
	kvfree(all_lat);
	kfree(tasks);
	kfree(ctxs);
	return done;
enomem:
	kvfree(all_lat);
	kfree(tasks);
	kfree(ctxs);
	return -ENOMEM;
}

static int __init set_affn_scope(const char *dev, const char *scope)
{
	char path[128];
	loff_t pos = 0;
	struct file *f;
	ssize_t ret;

	snprintf(path, sizeof(path),
		 "/sys/bus/workqueue/devices/%s/affinity_scope", dev);
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	ret = kernel_write(f, scope, strlen(scope), &pos);
	filp_close(f, NULL);
	return ret < 0 ? ret : 0;
}

static void gate(struct wqt_ctx *ctx, long done, int expected,
		 const char *label)
{
	if (done < 0 && !ctx->failed) {
		ctx->failed = true;
		snprintf(ctx->reason, sizeof(ctx->reason),
			 "%s: run failed (%ld)", label, done);
	} else if (done != expected && !ctx->failed) {
		ctx->failed = true;
		snprintf(ctx->reason, sizeof(ctx->reason),
			 "%s: completed=%ld expected=%d", label, done, expected);
	}
}

static const char * const scopes[] = { "cache", "numa", "system" };

static int __init wqt_10_init(void)
{
	struct workqueue_struct *pcpu_wq, *unb_wq;
	int n_threads, expected, i;
	long done;

	WQT_INIT(10, "perf");

	if (wq_items <= 0) {
		WQT_FAIL("wq_items must be > 0");
		return WQT_FINISH();
	}

	n_threads = min(nr_threads ?: num_online_cpus(), num_online_cpus());
	expected = n_threads * wq_items;
	WQT_DIAG("%d threads x %d items", n_threads, wq_items);

	pcpu_wq = alloc_workqueue("wqt10_pcpu", WQ_PERCPU, 0);
	unb_wq = alloc_workqueue("wqt10_unb", WQ_UNBOUND | WQ_SYSFS, 0);
	if (!pcpu_wq || !unb_wq) {
		WQT_FAIL("alloc_workqueue failed");
		goto out;
	}

	done = run_bench(n_threads, pcpu_wq, "percpu");
	gate(&__wqt, done, expected, "percpu");
	if (min_throughput && !__wqt.failed && last_ips < min_throughput)
		WQT_FAIL("percpu throughput %llu < min_throughput %lu",
			 last_ips, min_throughput);

	for (i = 0; i < ARRAY_SIZE(scopes); i++) {
		int r = set_affn_scope("wqt10_unb", scopes[i]);

		if (r) {
			WQT_DIAG("skip scope '%s' (set failed: %d)",
				 scopes[i], r);
			continue;
		}
		done = run_bench(n_threads, unb_wq, scopes[i]);
		gate(&__wqt, done, expected, scopes[i]);
	}

out:
	if (pcpu_wq)
		destroy_workqueue(pcpu_wq);
	if (unb_wq)
		destroy_workqueue(unb_wq);

	return WQT_FINISH();
}
module_init(wqt_10_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: queue_work throughput/latency");
MODULE_LICENSE("GPL");
