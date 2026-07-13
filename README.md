# wq_testsuite — Linux workqueue self-tests

A standalone suite of 10 correctness tests for the Linux kernel **workqueue**
subsystem. Each test is a small out-of-tree kernel module that exercises the
workqueue API and self-checks its behaviour; a runner boots the target kernel
under [virtme-ng](https://github.com/arighi/virtme-ng), loads every module, and
reports results as [kselftest-style TAP](https://docs.kernel.org/dev-tools/kselftest.html).

Workqueue is a kernel-internal API with no direct userspace surface, so the
tests have to live in the kernel. Running against a **debug kernel** (KASAN,
`PROVE_LOCKING`, `DEBUG_OBJECTS_WORK`, `WQ_WATCHDOG`) turns those sanitizers
into a second oracle: a test fails not only on a bad assertion but also on any
use-after-free, deadlock, double-init/free of a `work_struct`, or stall detected
while it runs.

## Requirements

* A built Linux kernel tree with `CONFIG_MODULES=y` (the runner builds modules
  against it and boots it). For meaningful coverage also enable:
  `CONFIG_KASAN`, `CONFIG_PROVE_LOCKING`, `CONFIG_DEBUG_OBJECTS_WORK`,
  `CONFIG_WQ_WATCHDOG`, and the virtme rootfs bits
  (`CONFIG_FUSE_FS`, `CONFIG_VIRTIO_FS`, `CONFIG_OVERLAY_FS`).
* `virtme-ng` and a matching `qemu-system-<arch>`.

## Quick start

```sh
# Build + boot + run everything (defaults: KDIR=/home/leit/Devel/linux-next, x86_64)
./run.sh

# Point at a different tree / arch:
make KDIR=/path/to/linux ARCH=x86_64
KDIR=/path/to/linux ./run.sh

# Full (long) torture + perf instead of the quick default:
QUICK=0 ./run.sh
```

Expected output:

```
1..10
ok 1 - basic
ok 2 - ordered
...
ok 10 - perf
# passed 10/10
ALL TESTS PASSED
```

The clean TAP is also written to `results.tap`; the full console log is in
`/tmp/wq_vng.log`.

## The tests

| # | Module                 | What it checks |
|---|------------------------|----------------|
| 1 | `wqt_01_basic`         | queue/flush on per-cpu, unbound and system wq; every item runs exactly once |
| 2 | `wqt_02_ordered`       | `alloc_ordered_workqueue` runs items in FIFO order, never concurrently |
| 3 | `wqt_03_max_active`    | `max_active=K` never exceeds K concurrent items (K = 1, 2, 4) |
| 4 | `wqt_04_delayed`       | delayed-work timing, `mod_delayed_work`, `cancel_delayed_work_sync`, `flush_delayed_work` |
| 5 | `wqt_05_cancel`        | `cancel_work_sync` return values for pending/running/idle; no double-run; re-queue |
| 6 | `wqt_06_mem_reclaim`   | `WQ_MEM_RECLAIM` workqueue makes forward progress behind a held slot |
| 7 | `wqt_07_queue_on_cpu`  | `queue_work_on(cpu, …)` runs on the target CPU |
| 8 | `wqt_08_flags_matrix`  | per-cpu/unbound × HIGHPRI × CPU_INTENSIVE × FREEZABLE × MEM_RECLAIM all create/run/destroy cleanly |
| 9 | `wqt_09_torture`       | many threads queue/cancel/flush/drain concurrently; `accepted == executed + cancelled`; ordered never overlaps; no splat |
| 10| `wqt_10_perf`          | queue_work throughput + p50/p90/p99 latency (per-cpu vs unbound across affinity scopes); gates on no lost items |

## How a test reports its result

Each module does all its work in `module_init()`, cleans up the workqueues it
created, prints exactly one verdict line, and returns `-EAGAIN` so it unloads
itself immediately (matching `lib/test_workqueue.c` in the kernel tree). The
verdict line is:

```
WQT-RESULT <id> <name> : PASS
WQT-RESULT <id> <name> : FAIL (<reason>)
```

`guest.sh` maps that (plus a scan for kernel splats in the same window) to an
`ok`/`not ok` TAP line. `insmod`'s exit status is intentionally ignored (it is
always non-zero because of the `-EAGAIN`).

## Layout

```
wqtest.h            shared PASS/FAIL harness (WQT_INIT / WQT_CHECK / WQT_FINISH)
wqt_NN_*.c          the 10 test modules
Kbuild / Makefile   out-of-tree module build
guest.sh            runs inside the guest: insmod each module, emit TAP
run.sh              host: build, boot virtme-ng, collect results
```

## Adding a test

1. Create `wqt_NN_name.c`, include `"wqtest.h"`, do your checks in
   `module_init()` between `WQT_INIT(NN, "name")` and `return WQT_FINISH();`.
2. Add `obj-m += wqt_NN_name.o` to `Kbuild`.
3. (Optional) give it quick/full module params in `guest.sh`'s `case "$id"`.
