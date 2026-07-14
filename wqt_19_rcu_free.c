// SPDX-License-Identifier: GPL-2.0
/*
 * wqt_19_rcu_free - RCU-delayed free of a looked-up refcounted object.
 *
 * Models fs/aio.c: a kioctx is published in an RCU-protected table and looked
 * up under rcu_read_lock() (lookup_ioctx); when its last reference drops the
 * object is freed via queue_rcu_work() (free_ioctx), so the free waits for a
 * grace period (no use-after-free against concurrent RCU readers) *and* runs in
 * process context (free_ioctx calls aio_free_ring()).
 *
 * We publish one object, run a reader kthread that keeps looking it up and
 * taking a transient ref, then unpublish and drop the last ref.  The object
 * must be freed exactly once, in process context, after the readers are safe;
 * KASAN is the oracle for any straggling-reader use-after-free.
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include "wqtest.h"

#define RES_ID 0x1957

struct res {
	struct rcu_work rwork;
	refcount_t ref;
	int id;
};

static struct res __rcu *g_res;
static struct workqueue_struct *rwq;
static atomic_t freed, used;
static bool freed_in_process_ctx;
static struct completion freed_comp;

/* free_ioctx idiom: runs after a grace period, in process context. */
static void res_free_fn(struct work_struct *w)
{
	struct res *r = container_of(to_rcu_work(w), struct res, rwork);

	freed_in_process_ctx = !in_interrupt();
	atomic_inc(&freed);
	complete(&freed_comp);
	kfree(r);			/* KASAN catches any late RCU reader */
}

static void res_put(struct res *r)
{
	if (refcount_dec_and_test(&r->ref)) {
		INIT_RCU_WORK(&r->rwork, res_free_fn);
		queue_rcu_work(rwq, &r->rwork);
	}
}

/* lookup_ioctx idiom: rcu-protected deref + try to take a ref. */
static struct res *res_lookup(void)
{
	struct res *r;

	rcu_read_lock();
	r = rcu_dereference(g_res);
	if (r && !refcount_inc_not_zero(&r->ref))
		r = NULL;
	rcu_read_unlock();
	return r;
}

static int reader_thread(void *arg)
{
	while (!kthread_should_stop()) {
		struct res *r = res_lookup();

		if (r) {
			if (READ_ONCE(r->id) == RES_ID)
				atomic_inc(&used);
			res_put(r);
		}
		cond_resched();
	}
	return 0;
}

static int __init wqt_19_init(void)
{
	struct task_struct *reader;
	struct res *r;
	long wr;

	WQT_INIT(19, "rcu_free");

	rwq = alloc_workqueue("wqt19", WQ_PERCPU, 0);
	if (!rwq) {
		WQT_FAIL("alloc_workqueue failed");
		return WQT_FINISH();
	}
	atomic_set(&freed, 0);
	atomic_set(&used, 0);
	freed_in_process_ctx = false;
	init_completion(&freed_comp);

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r) {
		WQT_FAIL("kzalloc failed");
		destroy_workqueue(rwq);
		return WQT_FINISH();
	}
	r->id = RES_ID;
	refcount_set(&r->ref, 1);		/* the "published" reference */
	rcu_assign_pointer(g_res, r);

	reader = kthread_run(reader_thread, NULL, "wqt19/rd");
	if (IS_ERR(reader)) {
		WQT_FAIL("kthread_run failed");
		rcu_assign_pointer(g_res, NULL);
		synchronize_rcu();
		kfree(r);
		destroy_workqueue(rwq);
		return WQT_FINISH();
	}

	/* Let the reader exercise the lookup/use path against the live object. */
	msleep(20);
	WQT_CHECK(atomic_read(&used) > 0, "reader never used the object");

	/* Teardown: unpublish, then drop the published ref. */
	rcu_assign_pointer(g_res, NULL);
	res_put(r);			/* last ref may be held transiently by reader */

	wr = wait_for_completion_timeout(&freed_comp, msecs_to_jiffies(10000));
	WQT_CHECK(wr > 0, "object never freed after last put");
	WQT_CHECK(atomic_read(&freed) == 1, "object freed %d times",
		  atomic_read(&freed));
	WQT_CHECK(freed_in_process_ctx, "free ran in interrupt context");
	WQT_DIAG("reader used object %d time(s); freed once after grace period",
		 atomic_read(&used));

	kthread_stop(reader);		/* g_res is NULL now: reader takes no refs */
	destroy_workqueue(rwq);

	return WQT_FINISH();
}
module_init(wqt_19_init);

MODULE_AUTHOR("Breno Leitao <leitao@debian.org>");
MODULE_DESCRIPTION("workqueue selftest: rcu_work deferred free of a looked-up object (aio idiom)");
MODULE_LICENSE("GPL");
