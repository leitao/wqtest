// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_36_bh_driver_idiom - the hardirq -> BH handoff drivers actually use.
 *
 * This is what WQ_BH was added for: the tasklet replacement.  A completion
 * arrives in hardirq context, where almost nothing may be done, and the driver
 * defers the short non-sleeping part of the work to softirq with
 * queue_work(system_bh_wq, ...) instead of a tasklet.  dm-verity does exactly
 * that in verity_end_io() ("if (in_hardirq() || irqs_disabled()) queue_work(
 * system_bh_wq, &io->work)"), dm-crypt in kcryptd_queue_crypt(), and a row of
 * media and mailbox drivers do the same from their ISRs.
 *
 * The producer here is an irq_work initialised with IRQ_WORK_INIT_HARD(), so
 * the "ISR" really does run in hardirq context rather than merely in an atomic
 * one, and the queueing path under test is the one a driver takes.
 *
 * The second half is the teardown idiom from drivers/media/pci/smipcie and
 * mantis: disable_work_sync() to shut the bottom half off while the ISR may
 * still be firing, and enable_and_queue_work() to bring it back.  A work item
 * queued by an ISR that runs after teardown must not execute.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/irq_work.h>
#include <linux/wait.h>
#include "wqtest.h"

#define NR_REQS		32
#define BATCH		8
#define WAIT_MS		5000

struct req {
	struct work_struct work;
	int ran_on;
	int runs;
	bool softirq;
};

static struct workqueue_struct *bh_wq;
static struct req reqs[NR_REQS];
static struct req gate;

static atomic_t next_req, done, isr_runs, isr_rejected;
static bool isr_hardirq = true;
static bool gate_isr_queued;
static DECLARE_WAIT_QUEUE_HEAD(waitq);

static void bh_fn(struct work_struct *w)
{
	struct req *r = container_of(w, struct req, work);

	r->ran_on = smp_processor_id();
	r->softirq = in_serving_softirq();
	r->runs++;

	atomic_inc(&done);
	wake_up(&waitq);
}

/* Stands in for a device ISR: completes a batch and defers it to softirq. */
static void isr_fn(struct irq_work *w)
{
	int i, base;

	if (!in_hardirq())
		isr_hardirq = false;

	base = atomic_read(&next_req);
	for (i = base; i < base + BATCH && i < NR_REQS; i++)
		if (!queue_work(bh_wq, &reqs[i].work))
			atomic_inc(&isr_rejected);
	atomic_set(&next_req, i);

	atomic_inc(&isr_runs);
}

/* A late ISR arriving while the bottom half is disabled. */
static void gate_isr_fn(struct irq_work *w)
{
	if (!in_hardirq())
		isr_hardirq = false;

	gate_isr_queued = queue_work(bh_wq, &gate.work);
	atomic_inc(&isr_runs);
}

static struct irq_work isr = IRQ_WORK_INIT_HARD(isr_fn);
static struct irq_work gate_isr = IRQ_WORK_INIT_HARD(gate_isr_fn);

static int __init wqt_36_init(void)
{
	int i, round, wrong_runs = 0, wrong_ctx = 0;

	WQT_INIT(36, "bh_driver_idiom");

	bh_wq = alloc_workqueue("wqt36", WQ_BH | WQ_PERCPU, 0);
	if (!bh_wq) {
		WQT_FAIL("alloc_workqueue(WQ_BH) failed");
		return WQT_FINISH();
	}

	for (i = 0; i < NR_REQS; i++) {
		INIT_WORK(&reqs[i].work, bh_fn);
		reqs[i].ran_on = -1;
	}
	INIT_WORK(&gate.work, bh_fn);

	/* --- 1: hardirq producer, softirq consumer ------------------------ */
	atomic_set(&next_req, 0);
	atomic_set(&done, 0);
	atomic_set(&isr_runs, 0);
	atomic_set(&isr_rejected, 0);

	for (round = 0; round < NR_REQS / BATCH; round++) {
		irq_work_queue(&isr);
		irq_work_sync(&isr);		/* the ISR has returned */
	}

	wait_event_timeout(waitq, atomic_read(&done) == NR_REQS,
			   msecs_to_jiffies(WAIT_MS));
	flush_workqueue(bh_wq);

	WQT_CHECK(isr_hardirq, "the irq_work producer did not run in hardirq context");
	WQT_CHECK(atomic_read(&isr_runs) == NR_REQS / BATCH,
		  "ISR ran %d times, expected %d", atomic_read(&isr_runs),
		  NR_REQS / BATCH);
	WQT_CHECK(atomic_read(&isr_rejected) == 0,
		  "%d queue_work() calls from hardirq were rejected",
		  atomic_read(&isr_rejected));
	WQT_CHECK(atomic_read(&done) == NR_REQS, "%d/%d completions processed",
		  atomic_read(&done), NR_REQS);

	for (i = 0; i < NR_REQS; i++) {
		if (reqs[i].runs != 1) {
			wrong_runs++;
			continue;
		}
		if (!reqs[i].softirq) {
			wrong_ctx++;
			WQT_DIAG("req %d ran outside softirq on cpu%d", i,
				 reqs[i].ran_on);
		}
	}
	WQT_CHECK(wrong_runs == 0, "%d/%d completions did not run exactly once",
		  wrong_runs, NR_REQS);
	WQT_CHECK(wrong_ctx == 0, "%d/%d bottom halves ran outside softirq",
		  wrong_ctx, NR_REQS);

	/* --- 2: teardown while the ISR may still fire --------------------- */
	atomic_set(&done, 0);
	atomic_set(&isr_runs, 0);
	gate.runs = 0;
	queue_work(bh_wq, &gate.work);		/* normal traffic first */
	flush_work(&gate.work);
	WQT_CHECK(gate.runs == 1, "gate item ran %d times before teardown",
		  gate.runs);

	disable_work_sync(&gate.work);
	gate_isr_queued = true;
	irq_work_queue(&gate_isr);
	irq_work_sync(&gate_isr);
	flush_workqueue(bh_wq);

	WQT_CHECK(!gate_isr_queued,
		  "queue_work from an ISR was accepted after disable_work_sync");
	WQT_CHECK(gate.runs == 1, "disabled bottom half ran anyway (%d runs)",
		  gate.runs);

	WQT_CHECK(enable_and_queue_work(bh_wq, &gate.work),
		  "enable_and_queue_work did not re-arm the bottom half");
	flush_work(&gate.work);
	WQT_CHECK(gate.runs == 2, "re-armed bottom half ran %d times, expected 2",
		  gate.runs);

	WQT_DIAG("%d completions deferred hardirq -> softirq in %d ISR batches",
		 NR_REQS, NR_REQS / BATCH);

	cancel_work_sync(&gate.work);
	destroy_workqueue(bh_wq);

	return WQT_FINISH();
}
module_init(wqt_36_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: hardirq to WQ_BH handoff (dm-verity/media idiom)");
MODULE_LICENSE("GPL");
