#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Build the workqueue self-test modules against a kernel tree.
#
# This can run on the host ahead of time or inside the VM under test -- the
# modules only have to be *loaded* inside the VM, which is what test.sh does.
#
# Env overrides:
#   KDIR    kernel build tree to build against
#           (default: /lib/modules/$(uname -r)/build, else /home/leit/Devel/linux-next)
#   LLVM    default builds the modules with clang (the kernels under test are
#           clang-built); set LLVM= (empty) to force gcc instead.

set -u

DIR="$(cd "$(dirname "$0")" && pwd)"

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

echo "wq_testsuite: KDIR=$KDIR cc=$CC"
echo "== building modules (make -C $KDIR M=$DIR) =="
if ! make -C "$KDIR" M="$DIR" ${LLVM:+LLVM=1} modules; then
	echo "Bail out! module build failed (is KDIR=$KDIR correct for $(uname -r)?)"
	exit 2
fi
