#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Runs *inside* the virtme-ng guest.  Loads each wqt_*.ko in order, judges the
# outcome from the kernel log, and emits kselftest-style TAP.  Every test module
# returns -EAGAIN (so insmod always "fails" and the module auto-unloads); the
# verdict comes from the "WQT-RESULT" marker in dmesg, not from insmod's exit
# code.  A test also fails if any kernel splat (KASAN / lockdep / debugobjects /
# BUG / WARNING) appears while it runs.

set -u

DIR="$(dirname "$0")"
[ -f "$DIR/run.env" ] && . "$DIR/run.env"
QUICK="${QUICK:-1}"
OUT="$DIR/results.tap"

SPLAT='BUG:|WARNING:|KASAN|ODEBUG:|Oops|general protection|INFO: possible|list_add|list_del|refcount_|UBSAN|NULL pointer|stack segment|kernel BUG'

set -- "$DIR"/wqt_*.ko
if [ ! -e "$1" ]; then
	echo "===WQT-START==="
	echo "Bail out! no wqt_*.ko in $DIR (build first: make)"
	echo "===WQT-END==="
	exit 1
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
