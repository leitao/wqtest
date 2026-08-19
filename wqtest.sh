# SPDX-License-Identifier: GPL-2.0
#
# wqtest.sh - shared reporting harness for the userspace workqueue tests.
#
# The counterpart of wqtest.h.  The module tests do their checks in
# module_init() and print the verdict to dmesg; these do theirs against
# /sys from a shell and print the same verdict to both stdout and dmesg,
# landing in the same window test.sh scans for kernel splats.  So a sysfs
# write that parses fine but trips a WARN in the kernel still fails the test,
# and a run watched on the console shows a script test's verdict exactly
# where it shows a module's.
#
# Usage:
#
#	. "$(dirname "$0")/wqtest.sh"
#	wqt_init 37 sysfs_attrs
#	wqt_eq "$(cat "$d/per_cpu")" 1 "per_cpu of a percpu wq"
#	[ -e "$d/nice" ]; wqt_check $? "unbound wq has no nice attribute"
#	wqt_finish
#
# Copyright (c) 2026 Breno Leitao <leitao@debian.org>

WQT_ID=""
WQT_NAME=""
WQT_FAILED=0
WQT_REASON=""

# _wqt_say <loglevel> <line> -- print to stdout and mirror into the kernel
# log.  A module test reports through printk, so its verdict is on the console
# of the machine under test; without the mirror a script test would be visible
# only in the runner's TAP file.  The levels are the ones wqtest.h prints at,
# so a FAIL is as loud here as it is there.
#
# The redirection is inside the group because a failing ">" is reported by the
# shell itself, before any 2>/dev/null on the same command takes effect.
_wqt_say() {
	echo "$2"
	{ echo "<$1>$2" > /dev/kmsg; } 2>/dev/null
}

wqt_init() {
	WQT_ID="$1"
	WQT_NAME="$2"
	WQT_FAILED=0
	WQT_REASON=""
	_wqt_say 6 "# wqt$WQT_ID $WQT_NAME: starting"
}

# Diagnostic line, prefixed so the runner treats it as a comment.
wqt_diag() {
	_wqt_say 6 "# wqt$WQT_ID $*"
}

# Record a failure.  Checks keep running afterwards so a single run reports
# every problem it finds; the first message becomes the verdict reason.
wqt_fail() {
	_wqt_say 3 "# wqt$WQT_ID FAIL: $*"
	if [ "$WQT_FAILED" -eq 0 ]; then
		WQT_FAILED=1
		WQT_REASON="$*"
	fi
}

# wqt_check <status> <message...> -- fail unless <status> is 0.  Meant to be
# fed $? so the condition reads as an ordinary shell test.
wqt_check() {
	_st="$1"
	shift
	[ "$_st" -eq 0 ] || wqt_fail "$@"
}

# wqt_eq <got> <want> <what> -- fail unless the two strings match.
wqt_eq() {
	[ "$1" = "$2" ] || wqt_fail "$3: got '$1', want '$2'"
}

# wqt_write <file> <value> <what> -- the write must succeed.  A rejected sysfs
# write is an errno on the redirection, which the shell announces itself, so
# the group is what keeps "Invalid argument" out of the log.
wqt_write() {
	if ! { printf '%s' "$2" > "$1"; } 2>/dev/null; then
		wqt_fail "$3: write of '$2' was rejected"
		return 1
	fi
	return 0
}

# wqt_write_fails <file> <value> <what> -- the write must be rejected.
wqt_write_fails() {
	if { printf '%s' "$2" > "$1"; } 2>/dev/null; then
		wqt_fail "$3: write of '$2' was accepted"
		return 1
	fi
	return 0
}

# Print the single verdict line test.sh maps to ok/not ok.
wqt_finish() {
	if [ "$WQT_FAILED" -ne 0 ]; then
		_wqt_say 3 "WQT-RESULT $WQT_ID $WQT_NAME : FAIL ($WQT_REASON)"
	else
		_wqt_say 6 "WQT-RESULT $WQT_ID $WQT_NAME : PASS"
	fi
	return 0
}

# --- shared workqueue-sysfs plumbing ---------------------------------------

# /sys/devices/virtual/workqueue is where subsys_virtual_register() puts the
# workqueue bus; /sys/bus/workqueue/devices is the same set by another path.
WQ_SYS=/sys/devices/virtual/workqueue
WQ_BUS=/sys/bus/workqueue/devices
WQ_PARAM=/sys/module/workqueue/parameters
WQH_PROBE=/sys/module/wqh_sysfs/parameters/probe

# Load the helper module that owns the wqh_* workqueues.  Returns non-zero
# (after recording the failure) if it could not be loaded.
wqh_load() {
	_dir="$1"

	rmmod wqh_sysfs 2>/dev/null
	if ! insmod "$_dir/wqh_sysfs.ko" 2>/dev/null; then
		wqt_fail "could not insmod wqh_sysfs.ko"
		return 1
	fi
	return 0
}

wqh_unload() {
	rmmod wqh_sysfs 2>/dev/null
}

# wqh_probe <wq-name> -- run a batch on that wq and echo the recorded line.
wqh_probe() {
	printf '%s' "$1" > "$WQH_PROBE" 2>/dev/null || return 1
	cat "$WQH_PROBE" 2>/dev/null
}

# Pull "cpus=<list>" or "nice=<lo>..<hi>" out of a wqh_probe line.
wqh_field() {
	echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"
}
