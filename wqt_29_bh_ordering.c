// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_29_bh_ordering - BH items run in queueing order, one at a time per cpu.
 *
 * "All BH work items are executed in the queueing CPU's softirq context in the
 * queueing order" is the whole ordering guarantee of a BH workqueue, and it is
 * a property of the pool rather than of the workqueue: every non-highpri BH
 * workqueue on a cpu is served by that cpu's single BH worker pool, so items
 * from two different BH workqueues share one worklist and one execution
 * context.  The second phase queues alternately to two workqueues to cover
 * that.
 *
 * Everything is queued to one cpu with softirqs disabled, so the whole batch is
 * on the worklist before any of it can run and the expected order is exactly
 * the queueing order.  Items also stamp a shared live counter, which can never
 * exceed one: a BH pool has a single execution context per cpu, so nothing can
 * overlap there.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/bottom_half.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include "wqtest.h"

#define NR_ITEMS	64
#define WAIT_MS		5000

struct seq_item {
	struct work_struct work;
	int seq;			/* execution order, 1-based */
	int ran_on;
};

static struct seq_item items[NR_ITEMS];
static atomic_t seq, ran, live;
static int overlaps;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void seq_fn(struct work_struct *w)
{
	struct seq_item *it = container_of(w, struct seq_item, work);

	if (atomic_inc_return(&live) != 1)
		WRITE_ONCE(overlaps, READ_ONCE(overlaps) + 1);
	it->seq = atomic_inc_return(&seq);
	it->ran_on = smp_processor_id();
	udelay(2);			/* widen the window an overlap would use */
	atomic_dec(&live);

	atomic_inc(&ran);
	wake_up(&waitq);
}

static int __init wqt_29_init(void)
{
	struct workqueue_struct *wq_a = NULL, *wq_b = NULL;
	int phase, i, cpu, rejected;

	WQT_INIT(29, "bh_ordering");

	wq_a = alloc_workqueue("wqt29_a", WQ_BH | WQ_PERCPU, 0);
	wq_b = alloc_workqueue("wqt29_b", WQ_BH | WQ_PERCPU, 0);
	if (!wq_a || !wq_b) {
		WQT_FAIL("alloc_workqueue(WQ_BH) failed");
		goto out;
	}

	for (i = 0; i < NR_ITEMS; i++)
		INIT_WORK(&items[i].work, seq_fn);

	for (phase = 0; phase < 2; phase++) {
		/* phase 0: one workqueue.  phase 1: two sharing the BH pool. */
		struct workqueue_struct *odd = phase ? wq_b : wq_a;
		const char *name = phase ? "two_wq" : "one_wq";
		int out_of_order = 0, wrong_cpu = 0;

		atomic_set(&seq, 0);
		atomic_set(&ran, 0);
		atomic_set(&live, 0);
		WRITE_ONCE(overlaps, 0);
		rejected = 0;
		for (i = 0; i < NR_ITEMS; i++) {
			items[i].seq = -1;
			items[i].ran_on = -1;
		}

		/*
		 * Softirqs are off, so nothing on this cpu can drain the pool
		 * until the whole batch is queued; local_bh_enable() then runs
		 * it.
		 */
		local_bh_disable();
		cpu = smp_processor_id();
		for (i = 0; i < NR_ITEMS; i++) {
			if (!queue_work(i & 1 ? odd : wq_a, &items[i].work))
				rejected++;
		}
		local_bh_enable();

		wait_event_timeout(waitq, atomic_read(&ran) == NR_ITEMS,
				   msecs_to_jiffies(WAIT_MS));
		for (i = 0; i < NR_ITEMS; i++)
			flush_work(&items[i].work);

		WQT_CHECK(rejected == 0, "%s: queue_work rejected %d idle items",
			  name, rejected);
		WQT_CHECK(atomic_read(&ran) == NR_ITEMS, "%s: %d/%d items ran",
			  name, atomic_read(&ran), NR_ITEMS);

		for (i = 0; i < NR_ITEMS; i++) {
			if (items[i].seq != i + 1) {
				out_of_order++;
				if (out_of_order <= 4)
					WQT_DIAG("%s: item %d ran %d%s in order",
						 name, i, items[i].seq,
						 items[i].seq < 0 ? " (never)" : "");
			}
			if (items[i].ran_on != cpu) {
				wrong_cpu++;
				if (wrong_cpu <= 4)
					WQT_DIAG("%s: item %d queued on cpu%d ran on cpu%d",
						 name, i, cpu, items[i].ran_on);
			}
		}

		WQT_CHECK(out_of_order == 0,
			  "%s: %d/%d items ran out of queueing order", name,
			  out_of_order, NR_ITEMS);
		WQT_CHECK(wrong_cpu == 0, "%s: %d/%d items left the queueing cpu",
			  name, wrong_cpu, NR_ITEMS);
		WQT_CHECK(READ_ONCE(overlaps) == 0,
			  "%s: %d BH items overlapped on one cpu", name,
			  READ_ONCE(overlaps));
	}

	WQT_DIAG("%d items ordered per phase across 2 BH workqueues on one cpu",
		 NR_ITEMS);

out:
	if (wq_b)
		destroy_workqueue(wq_b);
	if (wq_a)
		destroy_workqueue(wq_a);

	return WQT_FINISH();
}
module_init(wqt_29_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: WQ_BH queueing order and serialisation");
MODULE_LICENSE("GPL");
