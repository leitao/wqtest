#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# wqt_39_sysfs_cpumask - the cpumask and affinity knobs, and whether they bite.
#
# Two masks decide where an unbound workqueue's workers may run: the per-wq
# cpumask, and the global one at /sys/devices/virtual/workqueue/cpumask that
# caps every unbound workqueue at once.  Both are written in the hex form
# cpumask_parse() takes and read back in the zero-padded, comma-grouped form
# "%*pb" produces, so the two never compare equal as strings and this test
# normalises before comparing.
#
# A readback only shows the mask was stored.  Each mask change is followed by a
# batch through the helper module, which reports the cpus its items actually
# ran on -- a per-wq mask of one cpu has to put all 64 of them on that cpu, and
# the global mask has to do the same to a workqueue that still asks for
# everything.
#
# The global cpumask and default_affinity_scope are system-wide, so both are
# saved on entry and restored from an EXIT trap.
#
# Copyright (c) 2026 Breno Leitao <leitao@debian.org>

DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DIR/wqtest.sh"

SCOPES="cpu smt cache cache_shard numa system"

# Strip the grouping commas and leading zeros "%*pb" pads a mask with, so two
# spellings of the same mask can be compared.  Only ever use this on a value
# being compared, never on one being written: cpumask_parse() needs the commas.
norm_mask() {
	echo "$1" | tr -d ',' | tr 'A-Z' 'a-z' | sed 's/^0*//; s/^$/0/'
}

# cpu_mask <cpu>... -- the hex spelling cpumask_parse() takes for a set of
# cpus, sized to cover $last_cpu.  It has to be assembled a 32-bit group at a
# time for two reasons: shell arithmetic is 64-bit, so "1 << 71" silently
# comes back as "1 << 7", and cpumask_parse() reads at most eight hex digits
# per comma-separated group, so one long unbroken string is -EOVERFLOW.
cpu_mask() {
	_ngroups=$((last_cpu / 32 + 1))
	_out=""
	_g="$_ngroups"
	while [ "$_g" -gt 0 ]; do
		_g=$((_g - 1))
		_v=0
		for _c in "$@"; do
			[ $((_c / 32)) -eq "$_g" ] || continue
			_v=$((_v | (1 << (_c % 32))))
		done
		_out="${_out:+$_out,}$(printf '%08x' "$_v")"
	done
	echo "$_out"
}

wqt_init 39 sysfs_cpumask

[ "$(id -u)" = 0 ] || wqt_fail "not root"
if [ ! -d "$WQ_SYS" ]; then
	wqt_fail "$WQ_SYS does not exist (CONFIG_SYSFS off?)"
	wqt_finish
	exit 0
fi

# --- save the system-wide state before touching anything -------------------
ORIG_CPUMASK=$(cat "$WQ_SYS/cpumask")
ORIG_SCOPE=$(cat "$WQ_PARAM/default_affinity_scope" 2>/dev/null)

restore() {
	printf '%s' "$ORIG_CPUMASK" > "$WQ_SYS/cpumask" 2>/dev/null
	[ -n "$ORIG_SCOPE" ] && printf '%s' "$ORIG_SCOPE" \
		> "$WQ_PARAM/default_affinity_scope" 2>/dev/null
	wqh_unload
}

wqh_load "$DIR" || { wqt_finish; exit 0; }
trap 'restore' EXIT INT TERM

U="$WQ_SYS/wqh_unbound"

last_cpu=$(sed 's/.*[,-]//' /sys/devices/system/cpu/online)
if [ "$last_cpu" -lt 1 ]; then
	wqt_diag "single online cpu, cpumask phases skipped"
	restore
	trap - EXIT INT TERM
	wqt_finish
	exit 0
fi
cpu_a="$last_cpu"
cpu_b=$((last_cpu - 1))

# --- the per-wq cpumask confines the workers -------------------------------
last_mask=""
for c in "$cpu_a" "$cpu_b"; do
	mask=$(cpu_mask "$c")
	wqt_write "$U/cpumask" "$mask" "wqh_unbound cpumask" || continue
	last_mask="$mask"
	wqt_eq "$(norm_mask "$(cat "$U/cpumask")")" "$(norm_mask "$mask")" \
		"wqh_unbound: cpumask after writing cpu$c"

	out=$(wqh_probe wqh_unbound)
	wqt_eq "$(wqh_field "$out" ran)" 64 \
		"wqh_unbound: probe with cpumask pinned to cpu$c"
	wqt_eq "$(wqh_field "$out" cpus)" "$c" \
		"wqh_unbound: workers ignored a cpumask of cpu$c only"
done

# Garbage is rejected and leaves the mask alone.
wqt_write_fails "$U/cpumask" "zz" "wqh_unbound cpumask"
wqt_eq "$(norm_mask "$(cat "$U/cpumask")")" "$(norm_mask "$last_mask")" \
	"wqh_unbound: cpumask changed by a rejected write"

# An all-zero mask does not name a usable cpu.  wqattrs_actualize_cpumask()
# falls back to the global unbound mask for the pools it builds, so whichever
# way the store answers, the workqueue has to keep running work.
printf '0' > "$U/cpumask" 2>/dev/null && empty_ret=accepted || empty_ret=rejected
out=$(wqh_probe wqh_unbound)
wqt_eq "$(wqh_field "$out" ran)" 64 \
	"wqh_unbound: work stopped running after an empty cpumask was $empty_ret"
wqt_diag "an empty per-wq cpumask was $empty_ret; the wq kept running"

wqt_write "$U/cpumask" "$ORIG_CPUMASK" "wqh_unbound cpumask reset"

# --- the global cpumask caps every unbound workqueue -----------------------
wqt_eq "$(stat -c %a "$WQ_SYS/cpumask_isolated")" 444 \
	"mode of cpumask_isolated"
cat "$WQ_SYS/cpumask_isolated" > /dev/null 2>&1
wqt_check $? "cpumask_isolated is not readable"

gmask=$(cpu_mask "$cpu_a" 0)
if wqt_write "$WQ_SYS/cpumask" "$gmask" "global cpumask"; then
	wqt_eq "$(norm_mask "$(cat "$WQ_SYS/cpumask")")" "$(norm_mask "$gmask")" \
		"global cpumask after writing cpu0+cpu$cpu_a"
	wqt_eq "$(norm_mask "$(cat "$WQ_SYS/cpumask_requested")")" \
		"$(norm_mask "$gmask")" \
		"cpumask_requested should echo the last accepted write"

	# wqh_unbound still asks for every cpu, so only the global cap can
	# keep its workers off the others.
	out=$(wqh_probe wqh_unbound)
	wqt_eq "$(wqh_field "$out" ran)" 64 "wqh_unbound: probe under a global cap"
	stray=$(echo "$(wqh_field "$out" cpus)" | tr ',' '\n' | tr '-' '\n' \
		| grep -vx -e 0 -e "$cpu_a" | head -1)
	[ -z "$stray" ]
	wqt_check $? "wqh_unbound ran on cpu$stray outside the global cpumask (cpus=$(wqh_field "$out" cpus))"
fi

# An empty global mask is refused outright -- there would be nowhere to run.
wqt_write_fails "$WQ_SYS/cpumask" 0 "global cpumask"
wqt_write_fails "$WQ_SYS/cpumask" zz "global cpumask"
wqt_eq "$(norm_mask "$(cat "$WQ_SYS/cpumask")")" "$(norm_mask "$gmask")" \
	"global cpumask changed by a rejected write"

wqt_write "$WQ_SYS/cpumask" "$ORIG_CPUMASK" "global cpumask restore"
wqt_eq "$(norm_mask "$(cat "$WQ_SYS/cpumask")")" \
	"$(norm_mask "$ORIG_CPUMASK")" "global cpumask after restore"

# --- default_affinity_scope feeds every wq left on "default" ---------------
if [ -n "$ORIG_SCOPE" ]; then
	wqt_write "$U/affinity_scope" default "wqh_unbound affinity_scope"
	for s in $SCOPES; do
		wqt_write "$WQ_PARAM/default_affinity_scope" "$s" \
			"default_affinity_scope" || continue
		wqt_eq "$(cat "$WQ_PARAM/default_affinity_scope")" "$s" \
			"default_affinity_scope after writing $s"
		wqt_eq "$(cat "$U/affinity_scope")" "default ($s)" \
			"a wq left on default should follow default_affinity_scope"
	done

	# "default" is a valid per-wq scope but meaningless as the default of
	# the default, and wq_affn_dfl_set() rejects it.
	wqt_write_fails "$WQ_PARAM/default_affinity_scope" default \
		"default_affinity_scope"
	wqt_write_fails "$WQ_PARAM/default_affinity_scope" bogus \
		"default_affinity_scope"

	wqt_write "$WQ_PARAM/default_affinity_scope" "$ORIG_SCOPE" \
		"default_affinity_scope restore"
	wqt_eq "$(cat "$WQ_PARAM/default_affinity_scope")" "$ORIG_SCOPE" \
		"default_affinity_scope after restore"
else
	wqt_diag "no default_affinity_scope parameter, phase skipped"
fi

wqt_diag "per-wq and global cpumask confined the workers; scope default restored to '$ORIG_SCOPE'"

restore
trap - EXIT INT TERM

wqt_finish
