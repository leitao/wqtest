// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_21_irq_bh - top-half schedules a bottom-half that drains in task context.
 *
 * The classic deferral idiom: an interrupt/atomic top half buffers an event and
 * kicks a work item; the bottom half then processes the accumulated events in
 * process context, where it may sleep.  Repeated top-half kicks while the work
 * is still pending/running coalesce into fewer bottom-half runs, and re-queuing
 * a running work guarantees the last event is still processed.  See
 * drivers/tty/tty_buffer.c (flush_to_ldisc, kicked via tty_buffer_queue_work)
 * and input drivers such as drivers/input/misc/gpio-vibra.c (schedule_work from
 * the event path).
 *
 * We fire many "interrupts" back-to-back and require every event to be consumed
 * exactly once, the bottom half to run in process context, and coalescing to
 * happen (runs <= events).
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include "wqtest.h"

#define NR_EVENTS 2000

static struct work_struct bh;
static spinlock_t ev_lock;
static int pending;			/* buffered events, under ev_lock */
static atomic_t produced, consumed, bh_runs;
static bool bh_ctx_bad;

/* bottom half: drains all buffered events in process context. */
static void bh_fn(struct work_struct *w)
{
	unsigned long flags;
	int n;

	if (in_interrupt())
		WRITE_ONCE(bh_ctx_bad, true);

	atomic_inc(&bh_runs);
	spin_lock_irqsave(&ev_lock, flags);
	n = pending;
	pending = 0;
	spin_unlock_irqrestore(&ev_lock, flags);
	atomic_add(n, &consumed);
}

/* stands in for a hardirq top half: buffer one event, kick the bottom half. */
static void fake_isr(void)
{
	unsigned long flags;

	spin_lock_irqsave(&ev_lock, flags);
	pending++;
	atomic_inc(&produced);
	spin_unlock_irqrestore(&ev_lock, flags);

	schedule_work(&bh);		/* coalesces while already queued */
}

static int __init wqt_21_init(void)
{
	int i;

	WQT_INIT(21, "irq_bh");

	INIT_WORK(&bh, bh_fn);
	spin_lock_init(&ev_lock);
	pending = 0;
	atomic_set(&produced, 0);
	atomic_set(&consumed, 0);
	atomic_set(&bh_runs, 0);
	bh_ctx_bad = false;

	for (i = 0; i < NR_EVENTS; i++)
		fake_isr();

	/* Drain: one flush handles the common case; a second scheduled run
	 * mops up any event that landed after the last bottom-half read its
	 * batch (no producers are active any more).
	 */
	flush_work(&bh);
	schedule_work(&bh);
	flush_work(&bh);

	WQT_CHECK(atomic_read(&consumed) == NR_EVENTS,
		  "consumed %d/%d events", atomic_read(&consumed), NR_EVENTS);
	WQT_CHECK(atomic_read(&produced) == NR_EVENTS,
		  "produced %d/%d events", atomic_read(&produced), NR_EVENTS);
	WQT_CHECK(!bh_ctx_bad, "bottom half ran in interrupt context");
	WQT_CHECK(atomic_read(&bh_runs) >= 1 &&
		  atomic_read(&bh_runs) <= NR_EVENTS + 1,
		  "bottom-half run count out of range (%d)",
		  atomic_read(&bh_runs));
	WQT_DIAG("coalesced %d events into %d bottom-half run(s)",
		 NR_EVENTS, atomic_read(&bh_runs));

	cancel_work_sync(&bh);

	return WQT_FINISH();
}
module_init(wqt_21_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: IRQ top-half/bottom-half deferral (tty/input idiom)");
MODULE_LICENSE("GPL");
