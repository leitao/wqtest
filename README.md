# wq_testsuite — Linux workqueue self-tests

A standalone suite of 16 correctness tests for the Linux kernel **workqueue**
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

`run.sh` is meant to run **inside the VM** whose kernel you want to test: it
builds the modules against the current kernel and loads them right there (it
does not spawn its own VM). So boot the kernel first, then run it at the guest
shell:

```sh
# 1. On the host, boot the kernel under test (host FS is visible via --rw):
virtme-ng --run /home/leit/Devel/linux-next --disable-microvm \
    --memory 4G --cpu 8 --rw --user root \
    --qemu /usr/local/bin/qemu-system-x86_64

# 2. At the guest shell:
cd /home/leit/Devel/wq_testsuite
./run.sh                     # quick run
QUICK=0 ./run.sh             # full (long) torture + perf
KDIR=/path/to/linux ./run.sh # override the build tree
LLVM=1 ./run.sh              # build the modules with clang
```

`KDIR` defaults to `/lib/modules/$(uname -r)/build` when present, otherwise
`/home/leit/Devel/linux-next`; it must match the running kernel (module
vermagic). You can build the modules on the host ahead of time with `make`, but
they must be loaded inside a VM.

Expected output:

```
1..16
ok 1 - basic
ok 2 - ordered
...
ok 16 - blocking_progress
# passed 16/16
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
| 11| `wqt_11_flush`         | `flush_work` blocks until a running item completes and returns true; false for an idle item |
| 12| `wqt_12_drain`         | double-`queue_work` of a pending item is rejected/runs once; `drain_workqueue` drains a self-requeuing chain |
| 13| `wqt_13_rcu_work`      | `queue_rcu_work`/`flush_rcu_work` run after a grace period, exactly once, and are re-queueable |
| 14| `wqt_14_set_max_active`| `workqueue_set_max_active` raises/lowers live concurrency; peak honours the current cap |
| 15| `wqt_15_cancel_delayed`| async `cancel_delayed_work` return values + `delayed_work_pending`; `queue_delayed_work_on` cpu binding |
| 16| `wqt_16_blocking_progress` | a batch queued behind a blocked worker on an unbound wq still drains (no pool stall) |

### Tests 11–16 in detail

The newer tests widen coverage beyond the basics above:

* **`wqt_11_flush`** — the `flush_work()` contract. Queues an item that sleeps
  for a fixed 300 ms and asserts that `flush_work()` blocks for the whole
  duration and returns `true` (it had to wait), that the item has finished by
  the time it returns (flush orders *after* completion), and that flushing an
  idle or already-completed item returns `false` immediately.

* **`wqt_12_drain`** — queue idempotency and chain draining. A `work_struct`
  has a single pending slot, so queuing an item that is already pending (held
  behind a blocker on a `max_active=1` wq) returns `false` and the item still
  runs exactly once. Separately, a finite self-requeuing item is queued once
  and `drain_workqueue()` must run the whole chain before it returns — coverage
  that plain `flush_workqueue()` would not follow.

* **`wqt_13_rcu_work`** — deferred execution via RCU. `queue_rcu_work()` runs
  the item only after a full grace period; the test checks it runs exactly
  once, that `flush_rcu_work()` waits for it (grace period included), and that
  the same `rcu_work` can be re-queued after it has run.

* **`wqt_14_set_max_active`** — the concurrency cap can change on a live wq.
  Starting at `max_active=1` the observed peak concurrency is 1; after
  `workqueue_set_max_active(wq, 4)` the peak climbs above 1 while never
  exceeding 4; lowering it back to 1 serialises the wq again. Concurrency is
  measured with a live-counter peak, as in `wqt_03`.

* **`wqt_15_cancel_delayed`** — the async (non-`_sync`) delayed-cancel path.
  On a delayed item whose timer is still armed, `cancel_delayed_work()` returns
  `true`, clears `delayed_work_pending()` and the item never runs; on an idle
  item it returns `false`. Also checks that `queue_delayed_work_on(cpu, …)`
  binds execution to the requested CPU.

* **`wqt_16_blocking_progress`** — the forward-progress guarantee. One worker
  is parked in a wait on an unbound wq with default `max_active`, a batch is
  queued behind it, and that batch must finish *while the blocker is still
  parked* — the pool has to grow/wake another worker rather than stall. This is
  the exact class of stall a long-blocking handler (e.g. KFENCE's
  `toggle_allocation_gate`) can expose; `WQ_WATCHDOG` is the second oracle.

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
wqt_NN_*.c          the 16 test modules
Kbuild / Makefile   out-of-tree module build
run.sh              in-VM: build against the current kernel, load each module, emit TAP
```

## Adding a test

1. Create `wqt_NN_name.c`, include `"wqtest.h"`, do your checks in
   `module_init()` between `WQT_INIT(NN, "name")` and `return WQT_FINISH();`.
2. Add `obj-m += wqt_NN_name.o` to `Kbuild`.
3. (Optional) give it quick/full module params in `run.sh`'s `case "$id"`.
