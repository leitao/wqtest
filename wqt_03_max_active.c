// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_03_max_active - concurrency cap (max_active) enforcement.
 *
 * An unbound workqueue created with max_active=K must never run more than K
 * work items concurrently across the system.  Each item bumps a live counter,
 * sleeps briefly, and records the maximum concurrency observed; we assert that
 * the peak never exceeds K and that every item ran.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include "wqtest.h"

#define NR_ITEMS 32

static struct work_struct items[NR_ITEMS];
static atomic_t live;
static atomic_t max_live;
static atomic_t executed;

static void ma_fn(struct work_struct *w)
{
	int cur, old;

	cur = atomic_inc_return(&live);

	/* max_live = max(max_live, cur) */
	old = atomic_read(&max_live);
	while (cur > old) {
		int prev = atomic_cmpxchg(&max_live, old, cur);

		if (prev == old)
			break;
		old = prev;
	}

	msleep(20);
	atomic_inc(&executed);
	atomic_dec(&live);
}

static void run_one(struct wqt_ctx *ctx, int k)
{
	struct workqueue_struct *wq;
	int i, peak;

	atomic_set(&live, 0);
	atomic_set(&max_live, 0);
	atomic_set(&executed, 0);

	wq = alloc_workqueue("wqt03_k%d", WQ_UNBOUND, k, k);
	if (!wq) {
		/* Record failure through the caller's context. */
		ctx->failed = true;
		snprintf(ctx->reason, sizeof(ctx->reason),
			 "alloc_workqueue(max_active=%d) failed", k);
		return;
	}

	for (i = 0; i < NR_ITEMS; i++) {
		INIT_WORK(&items[i], ma_fn);
		queue_work(wq, &items[i]);
	}
	flush_workqueue(wq);
	destroy_workqueue(wq);

	peak = atomic_read(&max_live);
	pr_info("# wqt03 max_active=%d: peak concurrency=%d, executed=%d/%d\n",
		k, peak, atomic_read(&executed), NR_ITEMS);

	if (peak > k && !ctx->failed) {
		ctx->failed = true;
		snprintf(ctx->reason, sizeof(ctx->reason),
			 "max_active=%d but peak concurrency=%d", k, peak);
	}
	if (atomic_read(&executed) != NR_ITEMS && !ctx->failed) {
		ctx->failed = true;
		snprintf(ctx->reason, sizeof(ctx->reason),
			 "max_active=%d: executed=%d expected=%d", k,
			 atomic_read(&executed), NR_ITEMS);
	}
}

static int __init wqt_03_init(void)
{
	static const int ks[] = { 1, 2, 4 };
	int i;

	WQT_INIT(3, "max_active");

	for (i = 0; i < ARRAY_SIZE(ks); i++)
		run_one(&__wqt, ks[i]);

	return WQT_FINISH();
}
module_init(wqt_03_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: max_active concurrency cap");
MODULE_LICENSE("GPL");
