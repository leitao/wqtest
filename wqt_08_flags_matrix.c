// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_08_flags_matrix - workqueue creation-flag combinations.
 *
 * Allocates workqueues across a matrix of flag combinations (per-cpu / unbound
 * crossed with HIGHPRI, CPU_INTENSIVE, FREEZABLE and MEM_RECLAIM), runs a small
 * batch of work through each, flushes and destroys it.  Every combination must
 * allocate, run all items and tear down cleanly; KASAN, lockdep and
 * debugobjects catch anything subtler.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include "wqtest.h"

#define NR_ITEMS 8

static struct work_struct items[NR_ITEMS];
static atomic_t done;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void flag_fn(struct work_struct *w)
{
	atomic_inc(&done);
	wake_up(&waitq);
}

static const struct {
	unsigned int flag;
	const char *name;
} opt_flags[] = {
	{ WQ_HIGHPRI,		"highpri" },
	{ WQ_CPU_INTENSIVE,	"cpu_intensive" },
	{ WQ_FREEZABLE,		"freezable" },
	{ WQ_MEM_RECLAIM,	"mem_reclaim" },
};

static int __init wqt_08_init(void)
{
	static const unsigned int bases[] = { WQ_PERCPU, WQ_UNBOUND };
	int b, mask, i, tested = 0, failed = 0;

	WQT_INIT(8, "flags_matrix");

	for (b = 0; b < ARRAY_SIZE(bases); b++) {
		for (mask = 0; mask < (1 << ARRAY_SIZE(opt_flags)); mask++) {
			struct workqueue_struct *wq;
			unsigned int flags = bases[b];
			long r;

			for (i = 0; i < ARRAY_SIZE(opt_flags); i++)
				if (mask & (1 << i))
					flags |= opt_flags[i].flag;

			/* CPU_INTENSIVE is a per-cpu concept; skip on unbound. */
			if ((bases[b] == WQ_UNBOUND) &&
			    (flags & WQ_CPU_INTENSIVE))
				continue;

			wq = alloc_workqueue("wqt08_%x", flags, 0, flags);
			if (!wq) {
				failed++;
				WQT_DIAG("alloc failed for flags=0x%x", flags);
				continue;
			}

			tested++;
			atomic_set(&done, 0);
			for (i = 0; i < NR_ITEMS; i++) {
				INIT_WORK(&items[i], flag_fn);
				queue_work(wq, &items[i]);
			}
			r = wait_event_timeout(waitq,
					       atomic_read(&done) == NR_ITEMS,
					       msecs_to_jiffies(5000));
			if (r <= 0) {
				failed++;
				WQT_DIAG("flags=0x%x: only %d/%d items ran",
					 flags, atomic_read(&done), NR_ITEMS);
			}

			flush_workqueue(wq);
			destroy_workqueue(wq);
		}
	}

	WQT_CHECK(failed == 0, "%d flag combination(s) failed", failed);
	WQT_DIAG("tested %d flag combinations", tested);

	return WQT_FINISH();
}
module_init(wqt_08_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: creation-flag combination matrix");
MODULE_LICENSE("GPL");
