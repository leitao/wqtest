// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_20_highpri - WQ_HIGHPRI latency work is not stuck behind normal work.
 *
 * Latency-sensitive paths run on a WQ_HIGHPRI workqueue so they are serviced by
 * the high-priority worker pool rather than queueing behind ordinary work:
 * e.g. drivers/gpu/drm/i915/display/intel_display_driver.c allocates an
 * "i915_flip" WQ_HIGHPRI queue for page flips, and
 * drivers/gpu/drm/amd/amdkfd/kfd_interrupt.c a WQ_HIGHPRI IH queue.
 *
 * On a single CPU we flood the normal per-cpu pool with slow work, then queue a
 * high-priority item on the *same* CPU; because HIGHPRI has its own pool it must
 * run without waiting for the whole normal backlog to drain.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include "wqtest.h"

#define NR_NORMAL 16
#define NORMAL_MS 20

static struct work_struct normal[NR_NORMAL];
static struct work_struct hi;
static atomic_t normal_done;
static int hi_saw_normal_done;
static struct completion hi_comp;

static void normal_fn(struct work_struct *w)
{
	msleep(NORMAL_MS);		/* slow: yields the cpu while "working" */
	atomic_inc(&normal_done);
}

static void hi_fn(struct work_struct *w)
{
	hi_saw_normal_done = atomic_read(&normal_done);
	complete(&hi_comp);
}

static int __init wqt_20_init(void)
{
	struct workqueue_struct *normal_wq, *hi_wq;
	int cpu, i;
	long r;

	WQT_INIT(20, "highpri");

	normal_wq = alloc_workqueue("wqt20_norm", WQ_PERCPU, 0);
	hi_wq = alloc_workqueue("wqt20_hi", WQ_PERCPU | WQ_HIGHPRI, 0);
	if (!normal_wq || !hi_wq) {
		WQT_FAIL("alloc_workqueue failed (norm=%p hi=%p)",
			 normal_wq, hi_wq);
		goto out;
	}

	/* Pin everything to one cpu so pool priority is what decides ordering. */
	cpu = cpumask_first(cpu_online_mask);
	atomic_set(&normal_done, 0);
	hi_saw_normal_done = -1;
	init_completion(&hi_comp);

	/* Flood the normal pool, then queue the high-priority item behind it. */
	for (i = 0; i < NR_NORMAL; i++) {
		INIT_WORK(&normal[i], normal_fn);
		queue_work_on(cpu, normal_wq, &normal[i]);
	}
	INIT_WORK(&hi, hi_fn);
	queue_work_on(cpu, hi_wq, &hi);

	r = wait_for_completion_timeout(&hi_comp, msecs_to_jiffies(5000));
	WQT_CHECK(r > 0, "highpri work never ran");
	WQT_CHECK(hi_saw_normal_done >= 0 && hi_saw_normal_done < NR_NORMAL,
		  "highpri waited behind the whole normal backlog (normal_done=%d/%d)",
		  hi_saw_normal_done, NR_NORMAL);
	WQT_DIAG("highpri ran after only %d/%d normal items completed",
		 hi_saw_normal_done, NR_NORMAL);

	flush_workqueue(normal_wq);
	WQT_CHECK(atomic_read(&normal_done) == NR_NORMAL,
		  "normal backlog: %d/%d ran", atomic_read(&normal_done),
		  NR_NORMAL);

out:
	if (normal_wq)
		destroy_workqueue(normal_wq);
	if (hi_wq)
		destroy_workqueue(hi_wq);

	return WQT_FINISH();
}
module_init(wqt_20_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: WQ_HIGHPRI latency work (i915/kfd idiom)");
MODULE_LICENSE("GPL");
