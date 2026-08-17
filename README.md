# wq_testsuite — Linux workqueue self-tests

A standalone suite of 25 tests for the Linux kernel **workqueue**
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

Building and running are two separate scripts. `build.sh` builds the modules
against a kernel tree and can run anywhere; `test.sh` must run **inside the VM**
whose kernel you want to test, because it loads the modules into the running
kernel (it does not spawn its own VM).

```sh
# 1. Build the modules (host or guest):
./build.sh                     # against the default tree, with clang
KDIR=/path/to/linux ./build.sh # override the build tree
LLVM= ./build.sh               # build with gcc instead

# 2. On the host, boot the kernel under test (host FS is visible via --rw):
virtme-ng --run /home/leit/Devel/linux-next --disable-microvm \
    --memory 4G --cpu 8 --rw --user root \
    --qemu /usr/local/bin/qemu-system-x86_64

# 3. At the guest shell:
cd /home/leit/Devel/wq_testsuite
./test.sh                    # quick run
QUICK=0 ./test.sh            # full (long) torture + perf
```

`KDIR` defaults to `/lib/modules/$(uname -r)/build` when present, otherwise
`/home/leit/Devel/linux-next`; it must match the kernel `test.sh` runs on
(module vermagic). Both steps can also be driven from the Makefile: `make`
builds, `make test` runs, `make run` does both.

Expected output:

```
1..25
ok 1 - basic
ok 2 - ordered
...
ok 25 - nr_active_paths
# passed 25/25
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
| 17| `wqt_17_probe_on_node`  | run one-shot init on a target CPU/node via `queue_work_on`+`flush_work` and `work_on_cpu` (pci/cpufreq idiom) |
| 18| `wqt_18_vmstat_shepherd`| per-cpu delayed work that re-arms itself while work remains + a shepherd that kicks idle cpus (vmstat idiom) |
| 19| `wqt_19_rcu_free`       | RCU-published refcounted object freed via `queue_rcu_work` after its last ref drops (aio idiom) |
| 20| `wqt_20_highpri`        | `WQ_HIGHPRI` work runs ahead of a normal-priority backlog on the same cpu (i915/kfd idiom) |
| 21| `wqt_21_irq_bh`         | top-half `schedule_work` → bottom-half drains events in task context; kicks coalesce (tty/input idiom) |
| 22| `wqt_22_timeout`        | `delayed_work` deadline armed on issue, cancelled on completion, fires on stall (nvme idiom) |
| 23| `wqt_23_congested`     | `workqueue_congested()` is false when idle, true behind a full active slot, false again once drained |
| 24| `wqt_24_percpu_pwq`    | per-cpu pwq per cpu for normal/highpri/BH pools: items run on the cpu they were queued to; wq churn |
| 25| `wqt_25_nr_active_paths`| `max_active` raise/lower and a cpu offline/online with work in flight, per-cpu and unbound |

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

### Tests 17–22: modeled on real workqueue users

These reproduce, in miniature, how specific in-tree subsystems actually use
workqueues — one idiom per test, each citing the code it mirrors:

* **`wqt_17_probe_on_node`** — running one-shot init on a CPU local to a device,
  after `drivers/pci/pci-driver.c:pci_call_probe()` (queue an on-stack work on a
  node-local CPU, `flush_work()`, read back the return) and
  `drivers/cpufreq/powernow-k8.c` (`work_on_cpu()`). Asserts the work ran on the
  requested node/CPU and its return value propagated.

* **`wqt_18_vmstat_shepherd`** — after `mm/vmstat.c`. Each CPU has a deferrable
  `delayed_work` (`vmstat_update`) that folds pending per-cpu work and re-queues
  *itself* on the same CPU while more remains, disarming when quiescent; a
  shepherd (`vmstat_shepherd`) kicks CPUs that have work but no worker running.
  Asserts everything drains, workers self-disarm (no runaway re-queue), and
  teardown cancels cleanly.

* **`wqt_19_rcu_free`** — after `fs/aio.c` (`free_ioctx`). An RCU-published,
  refcounted object is looked up under `rcu_read_lock()` with
  `refcount_inc_not_zero()`; when its last ref drops it is freed via
  `queue_rcu_work()` so the free waits a grace period and runs in process
  context. A reader kthread races the teardown; the object must be freed exactly
  once with no use-after-free (KASAN is the oracle).

* **`wqt_20_highpri`** — after the `WQ_HIGHPRI` queues in
  `drivers/gpu/drm/i915/display` (page flips) and amdkfd (interrupt handling).
  A normal per-cpu pool is flooded with slow work and a high-priority item is
  queued behind it on the same CPU; the highpri item must run without waiting
  for the whole normal backlog.

* **`wqt_21_irq_bh`** — the top-half/bottom-half deferral used by
  `drivers/tty/tty_buffer.c` (`flush_to_ldisc`) and input drivers: an atomic
  "ISR" buffers an event and `schedule_work()`s a bottom half that drains all
  buffered events in process context. Asserts every event is consumed exactly
  once, the bottom half runs in task context, and repeated kicks coalesce.

* **`wqt_22_timeout`** — after `drivers/nvme/host/core.c` (`nvme_failfast_work`,
  keep-alive). A `delayed_work` deadline is armed when a command is issued and
  cancelled on the happy path; it fires only when the command stalls. A cmpxchg
  state machine makes the completion/timeout race resolve to one winner.

### Tests 23–25: pwq slots, congestion and nr_active

* **`wqt_23_congested`** — the `workqueue_congested()` contract. An idle
  workqueue is congested on no cpu; with the single active slot of a
  `max_active=1` wq held by a parked item and a backlog behind it, the cpu that
  backlog was queued to reports congested (and, on a per-cpu wq, an idle cpu
  does not); after the drain it is clear again. The query is also issued from
  worker context and from inside an `rcu_read_lock()` section. It reads the
  RCU-protected `wq->cpu_pwq[]` slot with only preemption disabled, so on a
  `PROVE_RCU` kernel a mismatched accessor there splats "suspicious
  rcu_dereference_check() usage" and the runner's splat scan fails the test.

* **`wqt_24_percpu_pwq`** — the per-cpu pwq slots themselves. One item per
  online cpu on a normal per-cpu wq, a `WQ_HIGHPRI` one and a `WQ_BH` one (the
  three static per-cpu pools), plus an unbound wq for contrast; each item must
  run on the cpu it was queued to. Then 32 short-lived per-cpu workqueues are
  created, used and destroyed back to back so the install and teardown of those
  slots runs many times over, with KASAN and debugobjects as the oracle. The BH
  variant gets its own `work_struct`s: it runs in softirq while the others run
  in task context, and `INIT_WORK()` takes the work's lockdep class from its
  call site.

* **`wqt_25_nr_active_paths`** — the paths that recompute an unbound wq's
  per-node nr_active budget, which a per-cpu wq must stay out of. With a parked
  item and a backlog queued, `max_active` is raised and lowered; then a batch is
  queued to a cpu which is immediately offlined and brought back. Work queued to
  a cpu that goes away still has to run, and the re-onlined cpu has to take work
  on its own pwq again. Needs `CONFIG_HOTPLUG_CPU` and a second cpu; the hotplug
  phase is skipped with a diagnostic otherwise.

## How a test reports its result

Each module does all its work in `module_init()`, cleans up the workqueues it
created, prints exactly one verdict line, and returns `-EAGAIN` so it unloads
itself immediately (matching `lib/test_workqueue.c` in the kernel tree). The
verdict line is:

```
WQT-RESULT <id> <name> : PASS
WQT-RESULT <id> <name> : FAIL (<reason>)
```

`test.sh` maps that (plus a scan for kernel splats in the same window) to an
`ok`/`not ok` TAP line. `insmod`'s exit status is intentionally ignored (it is
always non-zero because of the `-EAGAIN`).

## Layout

```
wqtest.h            shared PASS/FAIL harness (WQT_INIT / WQT_CHECK / WQT_FINISH)
wqt_NN_*.c          the 25 test modules
Kbuild / Makefile   out-of-tree module build
build.sh            build the modules against $KDIR (host or guest)
test.sh             in-VM: load each module into the running kernel, emit TAP
```

## Adding a test

1. Create `wqt_NN_name.c`, include `"wqtest.h"`, do your checks in
   `module_init()` between `WQT_INIT(NN, "name")` and `return WQT_FINISH();`.
2. Add `obj-m += wqt_NN_name.o` to `Kbuild`.
3. (Optional) give it quick/full module params in `test.sh`'s `case "$id"`.
