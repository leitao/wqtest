#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Build the workqueue self-test modules against the CURRENT kernel and run them,
# emitting kselftest-style TAP.
#
# This is meant to be run *inside* the VM whose kernel you want to test (e.g. a
# virtme-ng guest booted with --rw --user root).  It builds in, and loads into,
# the current environment -- it does not spawn its own VM.  Typical flow:
#
#   # on the host, boot the kernel you want to test:
#   virtme-ng --run /home/leit/Devel/linux-next --disable-microvm \
#       --memory 4G --cpu 8 --rw --user root \
#       --qemu /usr/local/bin/qemu-system-x86_64
#   # then, at the guest shell:
#   cd /home/leit/Devel/wq_testsuite && ./run.sh
#
# Env overrides:
#   KDIR    kernel build tree for the running kernel
#           (default: /lib/modules/$(uname -r)/build, else /home/leit/Devel/linux-next)
#   QUICK   1 = short torture/perf (default), 0 = full
#   LLVM    default builds the modules with clang (the kernels under test are
#           clang-built); set LLVM= (empty) to force gcc instead.

set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
QUICK="${QUICK:-1}"
OUT="$DIR/results.tap"

if [ -z "${KDIR:-}" ]; then
	if [ -d "/lib/modules/$(uname -r)/build" ]; then
		KDIR="/lib/modules/$(uname -r)/build"
	else
		KDIR="/home/leit/Devel/linux-next"
	fi
fi

# We always build the test modules with clang -- the kernels under test are
# clang-built.  Set LLVM= (empty) to force gcc instead.
if [ -z "${LLVM+set}" ]; then
	LLVM=1
fi
[ -n "${LLVM:-}" ] && CC=clang || CC=gcc

# The modules load into the running kernel, so flag one not built with $CC.
grep -q clang /proc/version 2>/dev/null && run_cc=clang || run_cc=gcc
[ "$run_cc" = "$CC" ] || echo "WARNING: building $CC modules, but the running" \
	"kernel was built with $run_cc -- they may not match the kernel under test"

# $KDIR/.config decides which compiler-specific flags Kbuild injects, so it must
# match $CC or the build fails (clang rejects gcc's -mpreferred-stack-boundary=,
# gcc rejects clang's -mstack-alignment=).  A plain `make` in the tree with the
# other compiler rewrites CONFIG_CC_IS_* via syncconfig; re-sync when that has
# left the tree out of step with $CC.
if [ -f "$KDIR/.config" ]; then
	[ "$CC" = clang ] && want=CONFIG_CC_IS_CLANG=y || want=CONFIG_CC_IS_GCC=y
	if ! grep -q "^$want" "$KDIR/.config"; then
		echo "== $KDIR/.config is not configured for $CC -- re-syncing" \
			"tree (make ${LLVM:+LLVM=1 }olddefconfig modules_prepare) =="
		make -C "$KDIR" ${LLVM:+LLVM=1} olddefconfig modules_prepare \
			|| { echo "Bail out! could not re-sync $KDIR to $CC"; exit 2; }
	fi
fi

SPLAT='BUG:|WARNING:|KASAN|ODEBUG:|Oops|general protection|INFO: possible|list_add|list_del|refcount_|UBSAN|NULL pointer|stack segment|kernel BUG'

echo "wq_testsuite: kernel $(uname -r) on $(uname -n)"
echo "             KDIR=$KDIR quick=$QUICK cc=$CC"
[ "$(id -u)" = 0 ] || echo "WARNING: not root -- insmod will fail"

# --- build against the current environment ---------------------------------
echo "== building modules (make -C $KDIR M=$DIR) =="
if ! make -C "$KDIR" M="$DIR" ${LLVM:+LLVM=1} modules; then
	echo "Bail out! module build failed (is KDIR=$KDIR correct for $(uname -r)?)"
	exit 2
fi

# --- load each module, judge, emit TAP -------------------------------------
set -- "$DIR"/wqt_*.ko
if [ ! -e "$1" ]; then
	echo "Bail out! no wqt_*.ko produced in $DIR"
	exit 2
fi
nr=$#

{
	echo "===WQT-START==="
	echo "1..$nr"
	i=0
	pass=0
	for ko in "$@"; do
		i=$((i + 1))
		base=$(basename "$ko" .ko)			# wqt_01_basic
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
		# -EAGAIN return makes insmod exit non-zero: intentional, ignore.
		insmod "$ko" $params 2>/dev/null
		sleep 0.5

		log=$(dmesg | sed -n "/$marker/,\$p")

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
