#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# wqt_40_sysfs_max_active_domains - max_active readback across domains.
#
# Workqueues carry both the unbound-wide and per-cpu max_active limits.  The
# sysfs knob is user-facing, so it must report the limit for the domain the
# workqueue actually uses, not the scaled shadow value kept for the other one.
#
# Copyright (c) 2026 Breno Leitao <leitao@debian.org>

DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DIR/wqtest.sh"

WQ_MAX_ACTIVE=2048		# include/linux/workqueue.h

count_online_cpus() {
	_n=0
	for _range in $(tr ',' ' ' < /sys/devices/system/cpu/online); do
		case "$_range" in
		*-*)
			_lo=${_range%-*}
			_hi=${_range#*-}
			_n=$((_n + _hi - _lo + 1))
			;;
		*)
			_n=$((_n + 1))
			;;
		esac
	done
	echo "$_n"
}

check_max_active() {
	_wq="$1"
	_want="$2"
	_what="$3"

	wqt_eq "$(cat "$WQ_SYS/$_wq/max_active")" "$_want" "$_what"
}

write_max_active() {
	_wq="$1"
	_value="$2"
	_what="$3"

	wqt_write "$WQ_SYS/$_wq/max_active" "$_value" "$_what"
	check_max_active "$_wq" "$_value" "$_what after writing $_value"
}

wqt_init 40 sysfs_max_active_domains

[ "$(id -u)" = 0 ] || wqt_fail "not root"
if [ ! -d "$WQ_SYS" ]; then
	wqt_fail "$WQ_SYS does not exist (CONFIG_SYSFS off?)"
	wqt_finish
	exit 0
fi

wqh_load "$DIR" || { wqt_finish; exit 0; }
trap 'wqh_unload' EXIT INT TERM

nr_online=$(count_online_cpus)
if [ "$nr_online" -lt 2 ]; then
	wqt_diag "single online cpu: scaled-domain checks are less discriminating"
fi

# Creation seeds both saved domains.  The visible value must still be the
# domain users asked for at alloc_workqueue().
check_max_active wqh_percpu 8 "wqh_percpu: initial max_active"
check_max_active wqh_unbound 8 "wqh_unbound: initial max_active"
check_max_active wqh_hipri 4 "wqh_hipri: initial max_active"

# A per-cpu workqueue's hidden unbound-wide limit scales with CPU count.  The
# sysfs file must continue to report the per-cpu limit.
write_max_active wqh_percpu 3 "wqh_percpu: per-cpu max_active"
out=$(wqh_probe wqh_percpu)
wqt_eq "$(wqh_field "$out" ran)" 64 \
	"wqh_percpu: work stopped running after max_active=3"

write_max_active wqh_percpu 1 "wqh_percpu: per-cpu max_active"
out=$(wqh_probe wqh_percpu)
wqt_eq "$(wqh_field "$out" ran)" 64 \
	"wqh_percpu: work stopped running after max_active=1"

wqt_write "$WQ_SYS/wqh_percpu/max_active" 999999 \
	"wqh_percpu max_active clamp"
check_max_active wqh_percpu "$WQ_MAX_ACTIVE" \
	"wqh_percpu: max_active above WQ_MAX_ACTIVE should clamp"

for bad in 0 -1 abc; do
	wqt_write_fails "$WQ_SYS/wqh_percpu/max_active" "$bad" \
		"wqh_percpu max_active"
done
check_max_active wqh_percpu "$WQ_MAX_ACTIVE" \
	"wqh_percpu: max_active changed by a rejected write"

# For unbound workqueues the visible value is the whole-workqueue limit.  With
# more than one CPU, the per-cpu shadow value is lower for these writes.
write_max_active wqh_unbound 17 "wqh_unbound: unbound max_active"
out=$(wqh_probe wqh_unbound)
wqt_eq "$(wqh_field "$out" ran)" 64 \
	"wqh_unbound: work stopped running after max_active=17"

write_max_active wqh_hipri 19 "wqh_hipri: unbound highpri max_active"
out=$(wqh_probe wqh_hipri)
wqt_eq "$(wqh_field "$out" ran)" 64 \
	"wqh_hipri: work stopped running after max_active=19"

wqt_diag "max_active readbacks stayed in their active accounting domains" \
	"($nr_online online cpus)"

wqh_unload
trap - EXIT INT TERM

wqt_finish
