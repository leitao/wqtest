#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# wqt_37_sysfs_attrs - the shape of /sys/devices/virtual/workqueue.
#
# The workqueue bus is registered unconditionally at core_initcall, but a
# workqueue only appears under it if it was created with WQ_SYSFS, and only
# until it is destroyed.  Which attributes it gets, and whether they are
# writable, is decided by its flags: per_cpu and max_active for everyone,
# nice/cpumask/affinity_scope/affinity_strict only for unbound workqueues, and
# max_active demoted to read-only when changing it would either break the
# ordering guarantee or mean nothing (WQ_BH).
#
# This is the surface; wqt_38 and wqt_39 write to it.
#
# Copyright (c) 2026 Breno Leitao <leitao@debian.org>

DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DIR/wqtest.sh"

UNBOUND_ATTRS="nice cpumask affinity_scope affinity_strict"

wqt_init 37 sysfs_attrs

[ "$(id -u)" = 0 ] || wqt_fail "not root"

# --- the bus itself --------------------------------------------------------
if [ ! -d "$WQ_SYS" ]; then
	wqt_fail "$WQ_SYS does not exist (CONFIG_SYSFS off?)"
	wqt_finish
	exit 0
fi

for a in cpumask cpumask_requested cpumask_isolated; do
	[ -f "$WQ_SYS/$a" ]
	wqt_check $? "the workqueue bus has no $a attribute"
done
wqt_eq "$(stat -c %a "$WQ_SYS/cpumask")" 644 "mode of the global cpumask"
wqt_eq "$(stat -c %a "$WQ_SYS/cpumask_requested")" 444 \
	"mode of cpumask_requested"

# --- nothing of ours is registered yet -------------------------------------
wqh_unload
for w in wqh_percpu wqh_unbound wqh_ordered wqh_hipri wqh_hidden; do
	[ ! -e "$WQ_SYS/$w" ]
	wqt_check $? "$w is in sysfs before the helper module is loaded"
done

wqh_load "$DIR" || { wqt_finish; exit 0; }

# --- registration ----------------------------------------------------------
for w in wqh_percpu wqh_unbound wqh_ordered wqh_hipri; do
	[ -d "$WQ_SYS/$w" ]
	wqt_check $? "WQ_SYSFS workqueue $w did not appear in sysfs"
	[ -d "$WQ_BUS/$w" ]
	wqt_check $? "$w is not reachable under $WQ_BUS"
done

# A workqueue without WQ_SYSFS must stay invisible.
[ ! -e "$WQ_SYS/wqh_hidden" ]
wqt_check $? "a workqueue created without WQ_SYSFS appeared in sysfs"

# --- per_cpu: read-only, and says which side of the fence the wq is on -----
for w in wqh_percpu wqh_unbound wqh_ordered wqh_hipri; do
	d="$WQ_SYS/$w"
	[ -d "$d" ] || continue
	wqt_eq "$(stat -c %a "$d/per_cpu")" 444 "$w: mode of per_cpu"
done
wqt_eq "$(cat "$WQ_SYS/wqh_percpu/per_cpu")"  1 "wqh_percpu: per_cpu"
wqt_eq "$(cat "$WQ_SYS/wqh_unbound/per_cpu")" 0 "wqh_unbound: per_cpu"
wqt_eq "$(cat "$WQ_SYS/wqh_ordered/per_cpu")" 0 \
	"wqh_ordered: per_cpu (an ordered wq is unbound)"
wqt_eq "$(cat "$WQ_SYS/wqh_hipri/per_cpu")"   0 "wqh_hipri: per_cpu"

# --- max_active: value, and read-only for the ordered one ------------------
wqt_eq "$(cat "$WQ_SYS/wqh_percpu/max_active")"  8 "wqh_percpu: max_active"
wqt_eq "$(cat "$WQ_SYS/wqh_unbound/max_active")" 8 "wqh_unbound: max_active"
wqt_eq "$(cat "$WQ_SYS/wqh_hipri/max_active")"   4 "wqh_hipri: max_active"
wqt_eq "$(cat "$WQ_SYS/wqh_ordered/max_active")" 1 "wqh_ordered: max_active"

for w in wqh_percpu wqh_unbound wqh_hipri; do
	wqt_eq "$(stat -c %a "$WQ_SYS/$w/max_active")" 644 \
		"$w: mode of max_active"
done
wqt_eq "$(stat -c %a "$WQ_SYS/wqh_ordered/max_active")" 444 \
	"wqh_ordered: mode of max_active (changing it would break ordering)"

# --- the unbound-only attributes -------------------------------------------
for w in wqh_unbound wqh_ordered wqh_hipri; do
	for a in $UNBOUND_ATTRS; do
		[ -f "$WQ_SYS/$w/$a" ]
		wqt_check $? "unbound wq $w has no $a attribute"
		wqt_eq "$(stat -c %a "$WQ_SYS/$w/$a")" 644 "$w: mode of $a"
	done
done
for a in $UNBOUND_ATTRS; do
	[ ! -e "$WQ_SYS/wqh_percpu/$a" ]
	wqt_check $? "per-cpu wq wqh_percpu has an unbound-only $a attribute"
done

# --- their defaults --------------------------------------------------------
wqt_eq "$(cat "$WQ_SYS/wqh_unbound/nice")" 0 "wqh_unbound: default nice"
wqt_eq "$(cat "$WQ_SYS/wqh_hipri/nice")" -20 \
	"wqh_hipri: default nice (WQ_HIGHPRI)"
wqt_eq "$(cat "$WQ_SYS/wqh_unbound/affinity_strict")" 0 \
	"wqh_unbound: default affinity_strict"

# An unset scope reports "default" plus the scope it currently resolves to.
scope=$(cat "$WQ_SYS/wqh_unbound/affinity_scope")
dfl=$(cat "$WQ_PARAM/default_affinity_scope" 2>/dev/null)
wqt_eq "$scope" "default ($dfl)" "wqh_unbound: default affinity_scope"

# The default cpumask covers every cpu the unbound pool may use.
wqt_eq "$(cat "$WQ_SYS/wqh_unbound/cpumask")" "$(cat "$WQ_SYS/cpumask")" \
	"wqh_unbound: default cpumask should be the global unbound cpumask"

wqt_diag "bus attrs ok, 4 registered + 1 hidden workqueue, scope '$scope'"

# --- unregistration --------------------------------------------------------
wqh_unload
for w in wqh_percpu wqh_unbound wqh_ordered wqh_hipri; do
	[ ! -e "$WQ_SYS/$w" ]
	wqt_check $? "$w still in sysfs after destroy_workqueue()"
done

wqt_finish
