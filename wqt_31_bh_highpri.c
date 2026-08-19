// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_31_bh_highpri - WQ_HIGHPRI picks the cpu's other BH pool.
 *
 * WQ_HIGHPRI is the only flag a BH workqueue may add, and on a BH workqueue it
 * does not mean a negative-nice worker -- there is no worker.  It selects the
 * second static BH pool of the cpu, which is driven by HI_SOFTIRQ instead of
 * TASKLET_SOFTIRQ.  __do_softirq() walks the pending mask from bit 0 upwards
 * and HI_SOFTIRQ is bit 0 while TASKLET_SOFTIRQ is bit 6, so with both raised
 * the highpri pool is drained first.
 *
 * The normal batch is queued *before* the highpri one, with softirqs disabled
 * so both are pending when the gate opens.  Order by queueing time would put
 * the normal items first; order by pool puts every highpri item first, which is
 * what the two-pool split is for.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/bottom_half.h>
#include <linux/wait.h>
#include "wqtest.h"

/*
 * bh_worker() stops after BH_WORKER_RESTARTS (10) items or 2ms and leaves the
 * rest for the next softirq round, which would let the pools interleave.  Keep
 * each batch below that so one round drains it.
 */
#define NR_EACH		8
#define WAIT_MS		5000

struct pri_item {
	struct work_struct work;
	int seq;
};

static struct pri_item normal[NR_EACH], highpri[NR_EACH];
static atomic_t seq, ran;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void pri_fn(struct work_struct *w)
{
	struct pri_item *it = container_of(w, struct pri_item, work);

	it->seq = atomic_inc_return(&seq);
	atomic_inc(&ran);
	wake_up(&waitq);
}

static int __init wqt_31_init(void)
{
	struct workqueue_struct *wq = NULL, *hi_wq = NULL;
	int i, cpu, last_hi = 0, first_normal = 0, late_hi = 0;

	WQT_INIT(31, "bh_highpri");

	wq = alloc_workqueue("wqt31", WQ_BH | WQ_PERCPU, 0);
	hi_wq = alloc_workqueue("wqt31_hi", WQ_BH | WQ_HIGHPRI | WQ_PERCPU, 0);
	if (!wq || !hi_wq) {
		WQT_FAIL("alloc_workqueue(WQ_BH) failed");
		goto out;
	}

	for (i = 0; i < NR_EACH; i++) {
		INIT_WORK(&normal[i].work, pri_fn);
		normal[i].seq = -1;
	}
	for (i = 0; i < NR_EACH; i++) {
		INIT_WORK(&highpri[i].work, pri_fn);
		highpri[i].seq = -1;
	}

	atomic_set(&seq, 0);
	atomic_set(&ran, 0);

	local_bh_disable();
	cpu = smp_processor_id();
	for (i = 0; i < NR_EACH; i++)		/* normal first ... */
		queue_work(wq, &normal[i].work);
	for (i = 0; i < NR_EACH; i++)		/* ... highpri behind it */
		queue_work(hi_wq, &highpri[i].work);
	local_bh_enable();

	wait_event_timeout(waitq, atomic_read(&ran) == 2 * NR_EACH,
			   msecs_to_jiffies(WAIT_MS));
	for (i = 0; i < NR_EACH; i++) {
		flush_work(&normal[i].work);
		flush_work(&highpri[i].work);
	}

	WQT_CHECK(atomic_read(&ran) == 2 * NR_EACH, "%d/%d items ran",
		  atomic_read(&ran), 2 * NR_EACH);

	/* Every highpri item must land before every normal one. */
	for (i = 0; i < NR_EACH; i++) {
		if (highpri[i].seq > last_hi)
			last_hi = highpri[i].seq;
		if (!first_normal || normal[i].seq < first_normal)
			first_normal = normal[i].seq;
	}
	for (i = 0; i < NR_EACH; i++) {
		if (highpri[i].seq < 0 || highpri[i].seq > first_normal)
			late_hi++;
	}

	WQT_CHECK(late_hi == 0,
		  "%d/%d highpri BH items ran after the normal batch (hi last=%d, normal first=%d)",
		  late_hi, NR_EACH, last_hi, first_normal);

	/* Both pools are still per-cpu and the same order holds within each. */
	for (i = 1; i < NR_EACH; i++) {
		WQT_CHECK(highpri[i].seq > highpri[i - 1].seq,
			  "highpri item %d ran at %d, before item %d at %d", i,
			  highpri[i].seq, i - 1, highpri[i - 1].seq);
		WQT_CHECK(normal[i].seq > normal[i - 1].seq,
			  "normal item %d ran at %d, before item %d at %d", i,
			  normal[i].seq, i - 1, normal[i - 1].seq);
	}

	WQT_DIAG("cpu%d: highpri %d..%d, normal %d..%d", cpu, highpri[0].seq,
		 last_hi, first_normal, normal[NR_EACH - 1].seq);

out:
	if (hi_wq)
		destroy_workqueue(hi_wq);
	if (wq)
		destroy_workqueue(wq);

	return WQT_FINISH();
}
module_init(wqt_31_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: WQ_BH | WQ_HIGHPRI runs ahead of the normal BH pool");
MODULE_LICENSE("GPL");
