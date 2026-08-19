#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# wqt_38_sysfs_write - writing to a workqueue's sysfs attributes.
#
# Every store handler on the workqueue bus parses the buffer itself and answers
# -EINVAL rather than quietly taking a garbage value, and the three that go
# through apply_workqueue_attrs() rebuild the workqueue's pools underneath.
# Reading the value back only shows the parse succeeded, so nice is also
# checked for effect: the helper module runs a batch and reports the nice its
# workers actually ran at.
#
# max_active on the ordered workqueue is deliberately not written to.  The
# attribute is 0444 there, but root has CAP_DAC_OVERRIDE and the write would go
# through to workqueue_set_max_active(), which WARNs on an ordered workqueue --
# a splat the runner would count against this test.  wqt_37 checks the mode.
#
# Copyright (c) 2026 Breno Leitao <leitao@debian.org>

DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DIR/wqtest.sh"

WQ_MAX_ACTIVE=2048		# include/linux/workqueue.h
SCOPES="cpu smt cache cache_shard numa system"

wqt_init 38 sysfs_write

[ "$(id -u)" = 0 ] || wqt_fail "not root"
if [ ! -d "$WQ_SYS" ]; then
	wqt_fail "$WQ_SYS does not exist (CONFIG_SYSFS off?)"
	wqt_finish
	exit 0
fi

wqh_load "$DIR" || { wqt_finish; exit 0; }
trap 'wqh_unload' EXIT INT TERM

U="$WQ_SYS/wqh_unbound"
P="$WQ_SYS/wqh_percpu"

# --- max_active ------------------------------------------------------------
wqt_write "$P/max_active" 3 "wqh_percpu max_active"
wqt_eq "$(cat "$P/max_active")" 3 "wqh_percpu: max_active after writing 3"
wqt_write "$U/max_active" 16 "wqh_unbound max_active"
wqt_eq "$(cat "$U/max_active")" 16 "wqh_unbound: max_active after writing 16"

# Out of range is clamped, not rejected: wq_clamp_max_active() pr_warns and
# clamps into 1..WQ_MAX_ACTIVE.
wqt_write "$U/max_active" 999999 "wqh_unbound max_active clamp"
wqt_eq "$(cat "$U/max_active")" "$WQ_MAX_ACTIVE" \
	"wqh_unbound: max_active above WQ_MAX_ACTIVE should clamp"

# Zero, negative and non-numeric are rejected outright, and change nothing.
wqt_write "$U/max_active" 16 "wqh_unbound max_active reset"
for bad in 0 -1 abc; do
	wqt_write_fails "$U/max_active" "$bad" "wqh_unbound max_active"
done
wqt_eq "$(cat "$U/max_active")" 16 \
	"wqh_unbound: max_active changed by a rejected write"

# --- nice, and whether it reached the workers ------------------------------
base=$(wqh_probe wqh_unbound)
wqt_eq "$(wqh_field "$base" nice)" "0..0" \
	"wqh_unbound: workers should start at nice 0"

for n in -5 19 -20; do
	wqt_write "$U/nice" "$n" "wqh_unbound nice" || continue
	wqt_eq "$(cat "$U/nice")" "$n" "wqh_unbound: nice after writing $n"
	out=$(wqh_probe wqh_unbound)
	wqt_eq "$(wqh_field "$out" ran)" 64 "wqh_unbound: probe at nice $n"
	wqt_eq "$(wqh_field "$out" nice)" "$n..$n" \
		"wqh_unbound: workers did not pick up nice $n"
done

# MIN_NICE..MAX_NICE is -20..19; outside that, and garbage, are rejected.
wqt_write "$U/nice" 0 "wqh_unbound nice reset"
for bad in -21 20 abc; do
	wqt_write_fails "$U/nice" "$bad" "wqh_unbound nice"
done
wqt_eq "$(cat "$U/nice")" 0 "wqh_unbound: nice changed by a rejected write"

# --- affinity_strict -------------------------------------------------------
wqt_write "$U/affinity_strict" 1 "wqh_unbound affinity_strict"
wqt_eq "$(cat "$U/affinity_strict")" 1 "wqh_unbound: affinity_strict after 1"
wqt_write "$U/affinity_strict" 0 "wqh_unbound affinity_strict"
wqt_eq "$(cat "$U/affinity_strict")" 0 "wqh_unbound: affinity_strict after 0"

# The store casts the parsed int to bool, so any non-zero reads back as 1.
wqt_write "$U/affinity_strict" 2 "wqh_unbound affinity_strict"
wqt_eq "$(cat "$U/affinity_strict")" 1 \
	"wqh_unbound: a non-zero affinity_strict should read back as 1"
wqt_write "$U/affinity_strict" 0 "wqh_unbound affinity_strict reset"
wqt_write_fails "$U/affinity_strict" abc "wqh_unbound affinity_strict"

# --- affinity_scope --------------------------------------------------------
for s in $SCOPES; do
	wqt_write "$U/affinity_scope" "$s" "wqh_unbound affinity_scope" || continue
	wqt_eq "$(cat "$U/affinity_scope")" "$s" \
		"wqh_unbound: affinity_scope after writing $s"
	out=$(wqh_probe wqh_unbound)
	wqt_eq "$(wqh_field "$out" ran)" 64 \
		"wqh_unbound: work stopped running under affinity_scope $s"
done

# "default" is a scope of its own: it reads back with the scope it resolves to.
wqt_write "$U/affinity_scope" default "wqh_unbound affinity_scope"
dfl=$(cat "$WQ_PARAM/default_affinity_scope" 2>/dev/null)
wqt_eq "$(cat "$U/affinity_scope")" "default ($dfl)" \
	"wqh_unbound: affinity_scope after writing default"

for bad in bogus 3 "cache "; do
	wqt_write_fails "$U/affinity_scope" "$bad" "wqh_unbound affinity_scope"
done
wqt_eq "$(cat "$U/affinity_scope")" "default ($dfl)" \
	"wqh_unbound: affinity_scope changed by a rejected write"

# A zero-length write never reaches the store handler: sysfs_kf_write() short
# circuits it, so it succeeds and changes nothing.
wqt_write "$U/affinity_scope" "" "wqh_unbound affinity_scope empty write"
wqt_eq "$(cat "$U/affinity_scope")" "default ($dfl)" \
	"wqh_unbound: affinity_scope changed by a zero-length write"

# --- the ordered workqueue still takes the unbound attributes --------------
# Only max_active is off limits there; nice and the affinity knobs go through
# apply_workqueue_attrs(), which keeps the single-pwq ordering guarantee.
O="$WQ_SYS/wqh_ordered"
wqt_write "$O/nice" -3 "wqh_ordered nice"
wqt_eq "$(cat "$O/nice")" -3 "wqh_ordered: nice after writing -3"
out=$(wqh_probe wqh_ordered)
wqt_eq "$(wqh_field "$out" ran)" 64 "wqh_ordered: probe after a nice change"
wqt_eq "$(wqh_field "$out" nice)" "-3..-3" \
	"wqh_ordered: workers did not pick up nice -3"
wqt_eq "$(cat "$O/max_active")" 1 \
	"wqh_ordered: max_active moved off 1"

wqt_diag "max_active/nice/affinity_scope/affinity_strict written and verified"

wqh_unload
trap - EXIT INT TERM

wqt_finish
