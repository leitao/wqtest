#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Host-side runner: build the modules out-of-tree, boot the target kernel under
# virtme-ng, run guest.sh inside it, and report TAP.  Exits non-zero if any test
# fails or the suite does not complete.
#
# Env overrides:
#   KDIR      kernel build tree      (default /home/leit/Devel/linux-next)
#   ARCH      target arch            (default x86_64)
#   QEMU      qemu binary            (default /usr/local/bin/qemu-system-x86_64)
#   MEMORY    guest memory           (default 4G)
#   CPUS      guest cpus             (default 8)
#   QUICK     1=short torture/perf   (default 1)
#   TIMEOUT   overall seconds        (default 600)

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
KDIR="${KDIR:-/home/leit/Devel/linux-next}"
ARCH="${ARCH:-x86_64}"
QEMU="${QEMU:-/usr/local/bin/qemu-system-x86_64}"
MEMORY="${MEMORY:-4G}"
CPUS="${CPUS:-8}"
QUICK="${QUICK:-1}"
TIMEOUT="${TIMEOUT:-600}"
LOG="${LOG:-/tmp/wq_vng.log}"

echo "== building modules against $KDIR =="
make -C "$KDIR" M="$DIR" ARCH="$ARCH" modules

printf 'QUICK=%s\n' "$QUICK" > "$DIR/run.env"
rm -f "$DIR/results.tap"

echo "== booting virtme-ng ($KDIR, mem=$MEMORY cpus=$CPUS quick=$QUICK) =="
# script(1) gives virtme-ng the controlling tty it needs from a non-interactive
# caller; the single quotes around --exec's argument are consumed by script's
# inner shell, so virtme-ng receives "sh <path>/guest.sh".
CMD="virtme-ng --run $KDIR --disable-microvm --memory $MEMORY --cpu $CPUS \
--rw --user root --qemu $QEMU --exec 'sh $DIR/guest.sh'"
timeout "$TIMEOUT" script -qfec "$CMD" "$LOG" >/dev/null 2>&1 || true

echo
echo "== results =="
if [ -f "$DIR/results.tap" ]; then
	RES="$DIR/results.tap"
	cat "$RES"
else
	RES="$LOG"
	sed -n '/===WQT-START===/,/===WQT-END===/p' "$LOG" || true
fi

if ! grep -q '===WQT-END===' "$RES" 2>/dev/null; then
	echo "ERROR: suite did not complete; tail of $LOG:" >&2
	tail -n 40 "$LOG" >&2 || true
	exit 2
fi

if grep -q '^not ok' "$RES"; then
	echo "FAILED"
	exit 1
fi

echo "ALL TESTS PASSED"
