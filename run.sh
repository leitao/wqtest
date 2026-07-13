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
#   LLVM    set to build the modules with clang (LLVM=1)

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

SPLAT='BUG:|WARNING:|KASAN|ODEBUG:|Oops|general protection|INFO: possible|list_add|list_del|refcount_|UBSAN|NULL pointer|stack segment|kernel BUG'

echo "wq_testsuite: kernel $(uname -r) on $(uname -n)"
echo "             KDIR=$KDIR quick=$QUICK"
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
