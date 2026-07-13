/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wqtest.h - shared reporting harness for the workqueue self-test modules.
 *
 * Each test module runs all of its checks synchronously in module_init(),
 * cleans up the workqueues it created, and returns WQT_FINISH().  Every module
 * returns -EAGAIN so it auto-unloads after running (matching the style of
 * lib/test_workqueue.c in the kernel tree).  The test runner parses the
 * "WQT-RESULT" marker from dmesg; insmod's exit status is intentionally
 * ignored (it is always non-zero because of the -EAGAIN).
 *
 * Usage:
 *
 *	static int __init my_test_init(void)
 *	{
 *		WQT_INIT(1, "basic");
 *		...
 *		WQT_CHECK(counter == n, "counter=%d expected=%d", counter, n);
 *		...
 *		return WQT_FINISH();
 *	}
 *
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */
#ifndef _WQTEST_H
#define _WQTEST_H

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/stringify.h>

struct wqt_ctx {
	int id;
	const char *name;
	bool failed;
	char reason[160];
};

/* Declare and start a test.  Defines the local __wqt context. */
#define WQT_INIT(_id, _name)						\
	struct wqt_ctx __wqt = {					\
		.id = (_id), .name = (_name), .failed = false,		\
	};								\
	pr_info("# wqt%02d %s: starting\n", __wqt.id, __wqt.name)

/* TAP-style diagnostic line, prefixed so the runner treats it as a comment. */
#define WQT_DIAG(fmt, ...)						\
	pr_info("# wqt%02d " fmt "\n", __wqt.id, ##__VA_ARGS__)

/*
 * Assert a condition.  On failure the check is logged and the first failure's
 * message is recorded as the verdict reason.  Checks keep running after a
 * failure so a single insmod reports every problem it finds.
 */
#define WQT_CHECK(cond, fmt, ...)					\
do {									\
	if (!(cond)) {							\
		pr_err("# wqt%02d FAIL: " fmt " [%s]\n",		\
		       __wqt.id, ##__VA_ARGS__, __stringify(cond));	\
		if (!__wqt.failed) {					\
			__wqt.failed = true;				\
			snprintf(__wqt.reason, sizeof(__wqt.reason),	\
				 fmt, ##__VA_ARGS__);			\
		}							\
	}								\
} while (0)

/* Force a failure with a message (e.g. from an error path). */
#define WQT_FAIL(fmt, ...)	WQT_CHECK(false, fmt, ##__VA_ARGS__)

/*
 * Print the single verdict line and yield the module_init() return value.
 * Always returns -EAGAIN so the module unloads itself after the run.
 */
#define WQT_FINISH()							\
({									\
	if (__wqt.failed)						\
		pr_err("WQT-RESULT %02d %s : FAIL (%s)\n",		\
		       __wqt.id, __wqt.name, __wqt.reason);		\
	else								\
		pr_info("WQT-RESULT %02d %s : PASS\n",			\
			__wqt.id, __wqt.name);				\
	-EAGAIN;							\
})

#endif /* _WQTEST_H */
