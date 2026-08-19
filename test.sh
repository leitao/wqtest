#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Load the workqueue self-test modules into the CURRENT kernel and emit
# kselftest-style TAP.
#
# This is meant to be run *inside* the VM whose kernel you want to test (e.g. a
# virtme-ng guest booted with --rw --user root).  It loads into the current
# environment -- it does not spawn its own VM.  Build the modules first with
# ./build.sh, on the host or in the guest.  Typical flow:
#
#   # on the host, build the modules for the kernel you want to test:
#   ./build.sh
#   # boot that kernel:
#   virtme-ng --run /home/leit/Devel/linux-next --disable-microvm \
#       --memory 4G --cpu 8 --rw --user root \
#       --qemu /usr/local/bin/qemu-system-x86_64
#   # then, at the guest shell:
#   cd /home/leit/Devel/wq_testsuite && ./test.sh
#
# Env overrides:
#   QUICK   1 = short torture/perf (default), 0 = full

set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
QUICK="${QUICK:-1}"
OUT="$DIR/results.tap"

SPLAT='BUG:|WARNING:|KASAN|ODEBUG:|Oops|general protection|INFO: possible|list_add|list_del|refcount_|UBSAN|NULL pointer|stack segment|kernel BUG'

echo "wq_testsuite: kernel $(uname -r) on $(uname -n)"
echo "             quick=$QUICK"
[ "$(id -u)" = 0 ] || echo "WARNING: not root -- insmod will fail"

# --- run each test, judge, emit TAP ----------------------------------------
#
# A test is either a module (wqt_NN_*.ko), which reports its verdict to dmesg
# from module_init(), or a script (wqt_NN_*.sh), which drives /sys from
# userspace and reports the same verdict on stdout.  Both are judged the same
# way, and both get the same scan for kernel splats over the window they ran
# in -- a sysfs write that parses fine but WARNs still fails its test.
tests=$(ls "$DIR"/wqt_[0-9]*.ko "$DIR"/wqt_[0-9]*.sh 2>/dev/null | sort)
if [ -z "$tests" ]; then
	echo "Bail out! no wqt_NN_* tests in $DIR -- run ./build.sh first"
	exit 2
fi
# shellcheck disable=SC2086 # deliberate word splitting: one path per test
set -- $tests
nr=$#

{
	echo "===WQT-START==="
	echo "1..$nr"
	i=0
	pass=0
	for t in "$@"; do
		i=$((i + 1))
		base=$(basename "$t" .ko); base=$(basename "$base" .sh)
		id=$(echo "$base" | sed -E 's/^wqt_([0-9]+)_.*/\1/')
		name=$(echo "$base" | sed -E 's/^wqt_[0-9]+_//')

		params=""
		case "$id" in
		09) [ "$QUICK" = "1" ] && params="duration_ms=3000" \
					|| params="duration_ms=20000" ;;
		10) [ "$QUICK" = "1" ] && params="wq_items=2000" \
					|| params="wq_items=50000" ;;
		esac

		echo "# --- $base ${params:+($params)} ---"
		marker="WQTMARK-$id-$$"
		echo "$marker" > /dev/kmsg 2>/dev/null
		out=""
		case "$t" in
		*.ko)	# -EAGAIN makes insmod exit non-zero: intentional, ignore.
			insmod "$t" $params 2>/dev/null ;;
		*.sh)	out=$(sh "$t" 2>&1) ;;
		esac
		sleep 0.5

		log=$(dmesg | sed -n "/$marker/,\$p")
		# A script reports on stdout; judge it out of the same window.
		[ -n "$out" ] && log="$log
$out"

		# KFENCE's toggle_allocation_gate idle-waits (wait_event_idle) for
		# the next sampled allocation; on a mostly-idle VM the WQ watchdog
		# mislabels that as a "workqueue lockup".  It is unrelated to the
		# workqueue under test, so drop it before the splat scan -- a real
		# wq lockup (no toggle_allocation_gate in flight) is still caught.
		if echo "$log" | grep -q 'toggle_allocation_gate'; then
			log=$(echo "$log" | grep -v 'BUG: workqueue lockup')
		fi

		# Echo the module's own diagnostics + verdict as TAP comments.
		echo "$log" | grep -E "(# wqt$id|WQT-RESULT $id)" \
			| sed -E 's/^\[[^]]*\] //; s/^([^#])/# \1/'

		if echo "$log" | grep -q "WQT-RESULT $id .* : PASS" \
		   && ! echo "$log" | grep -Eq "$SPLAT"; then
			echo "ok $i - $name"
			pass=$((pass + 1))
		else
			reason=$(echo "$log" \
				| grep -oE "WQT-RESULT $id .* : FAIL \(.*\)" \
				| head -1)
			[ -z "$reason" ] && \
				reason="no PASS marker / kernel splat detected"
			echo "not ok $i - $name # $reason"
			echo "$log" | grep -E "$SPLAT" | head -5 \
				| sed 's/^/#   splat: /'
		fi
	done
	echo "# passed $pass/$nr"
	echo "===WQT-END==="
} 2>&1 | tee "$OUT"

sync

# Exit non-zero if anything failed.
! grep -q '^not ok' "$OUT"
