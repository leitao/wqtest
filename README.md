# wq_testsuite — Linux workqueue self-tests

A standalone suite of 40 tests for the Linux kernel **workqueue**
subsystem. Most are small out-of-tree kernel modules that exercise the
workqueue API and self-check their behaviour; the sysfs tests are shell scripts
that drive the interface from userspace. A runner boots the target kernel
under [virtme-ng](https://github.com/arighi/virtme-ng), runs every test, and
reports results as [kselftest-style TAP](https://docs.kernel.org/dev-tools/kselftest.html).

Workqueue is almost entirely a kernel-internal API, so almost all of the tests
have to live in the kernel. Its one userspace surface is sysfs — there is no
configfs interface — and tests 37–40 cover that from a shell. Running against a
**debug kernel** (KASAN,
`PROVE_LOCKING`, `DEBUG_OBJECTS_WORK`, `WQ_WATCHDOG`) turns those sanitizers
into a second oracle: a test fails not only on a bad assertion but also on any
use-after-free, deadlock, double-init/free of a `work_struct`, or stall detected
while it runs.

## Requirements

* A built Linux kernel tree with `CONFIG_MODULES=y` (the runner builds modules
  against it and boots it). For meaningful coverage also enable:
  `CONFIG_KASAN`, `CONFIG_PROVE_LOCKING`, `CONFIG_DEBUG_OBJECTS_WORK`,
  `CONFIG_WQ_WATCHDOG`, `CONFIG_DEBUG_ATOMIC_SLEEP` (the oracle for `wqt_33`),
  and the virtme rootfs bits
  (`CONFIG_FUSE_FS`, `CONFIG_VIRTIO_FS`, `CONFIG_OVERLAY_FS`).
  `CONFIG_SYSFS` is what tests 37–40 need; `CONFIG_HOTPLUG_CPU` is what
  `wqt_25` and `wqt_35` need. Both skip themselves with a diagnostic if it is
  off.
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
1..40
ok 1 - basic
ok 2 - ordered
...
ok 40 - sysfs_max_active_domains
# passed 40/40
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
| 26| `wqt_26_pool_backing`  | a workqueue is served by the kind of pool its flags ask for: per-cpu vs unbound, normal vs highpri, task vs softirq |
| 27| `wqt_27_percpu_lifecycle`| repeated create/destroy of per-cpu, highpri, BH and unbound queues, some torn down with work still queued |
| 28| `wqt_28_bh_basic`      | `WQ_BH` and `WQ_BH \| WQ_HIGHPRI`, private and system: every item runs once, in softirq, on the queueing cpu |
| 29| `wqt_29_bh_ordering`   | BH items run in queueing order and never overlap, including across two BH wqs sharing one cpu's pool |
| 30| `wqt_30_bh_softirq_gate`| `local_bh_disable()` holds a BH item pending; the outermost `local_bh_enable()` runs it; a remote cpu is kicked by irq_work |
| 31| `wqt_31_bh_highpri`    | `WQ_BH \| WQ_HIGHPRI` drains on HI_SOFTIRQ, ahead of a normal BH batch queued before it |
| 32| `wqt_32_bh_delayed`    | `queue_delayed_work[_on]`, `mod_delayed_work`, `flush_delayed_work`, `cancel_delayed_work_sync` on a BH wq |
| 33| `wqt_33_bh_cancel`     | `cancel_work_sync`/`disable_work_sync` on BH work from atomic context; `disable_work` depth; `enable_and_queue_work` |
| 34| `wqt_34_bh_requeue`    | queueing from inside a BH handler: self-requeue (never nested), a second BH wq, a remote cpu, and a sleepable wq |
| 35| `wqt_35_bh_hotplug`    | `workqueue_softirq_dead()` drains both BH pools of a cpu being offlined; the re-onlined cpu takes BH work again |
| 36| `wqt_36_bh_driver_idiom`| hardirq `irq_work` producer → BH consumer (dm-verity idiom); `disable_work_sync`/`enable_and_queue_work` teardown |
| 37| `wqt_37_sysfs_attrs.sh` | *userspace*: which workqueues appear under the sysfs bus, which attributes they get, and their modes |
| 38| `wqt_38_sysfs_write.sh` | *userspace*: writes to `max_active`/`nice`/`affinity_scope`/`affinity_strict`, their rejects, and whether `nice` reaches the workers |
| 39| `wqt_39_sysfs_cpumask.sh`| *userspace*: the per-wq and global unbound cpumasks actually confine the workers; `default_affinity_scope` |
| 40| `wqt_40_sysfs_max_active_domains.sh`| *userspace*: `max_active` readback stays in the active accounting domain for per-cpu and unbound workqueues |

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

### Tests 26–27: which pool backs a workqueue

* **`wqt_26_pool_backing`** — a pwq points at either one of the static per-cpu
  pools or a hashed unbound pool, and the flags are what choose. The other
  tests check *where* work runs; this one checks *what* it runs on, because an
  unbound pool serving a `WQ_PERCPU` queue still lands on the right cpu
  whenever the scheduler leaves it there, and differs only in concurrency
  management and worker identity. The oracle is the worker's name:
  `format_worker_id()` spells a per-cpu pool worker `kworker/<cpu>:<id>`,
  suffixed `H` when the pool's nice is negative, and an unbound pool worker
  `kworker/u<pool_id>:<id>`, so the item reads `current->comm` and
  `task_nice()` back. Six variants are covered — per-cpu, per-cpu highpri,
  unbound, unbound highpri, BH and BH highpri. BH items run from softirq on the
  interrupted task, so those are checked for softirq context and cpu only.

* **`wqt_27_percpu_lifecycle`** — volume through the install and release
  paths. Creating a workqueue allocates a pwq for every possible cpu and
  installs it; destroying one releases them through `pwq_release_workfn()`,
  which has to tell a refcounted unbound pool from a static per-cpu one.
  `wqt_24` churns plain `WQ_PERCPU`; this adds the highpri, freezable, BH and
  unbound variants and destroys half the queues with work still pending so the
  drain path runs. It asserts little beyond "every item ran" — a pwq freed
  twice, a pool refcount driven negative or a leaked pwq surfaces in KASAN,
  debugobjects or lockdep rather than in an assertion. `rounds=` sets the
  create/destroy count per variant (default 24).

### Tests 28–36: BH workqueues

A `WQ_BH` workqueue is a convenience interface to softirq, added as the
replacement for tasklets. It is always per-cpu, takes `0` `max_active`, allows
only `WQ_HIGHPRI` on top of `WQ_BH`, and runs every item in the queueing cpu's
softirq context in the queueing order. Its items cannot sleep; everything else
— delayed queueing, flushing, cancelling — is supported. The rest of the suite
touches BH only in passing (24, 26, 27 include it in their flag matrices);
these nine cover it on its own terms.

* **`wqt_28_bh_basic`** — the execution contract, for four workqueues: a
  private `WQ_BH`, a private `WQ_BH | WQ_HIGHPRI`, `system_bh_wq` and
  `system_bh_highpri_wq`. One item per online cpu on each: it runs exactly
  once, with `in_serving_softirq()` and not `in_hardirq()`, on the cpu it was
  queued to. `queue_work()` without a cpu is checked separately, because "the
  queueing cpu" is the contract there.

* **`wqt_29_bh_ordering`** — ordering is a property of the *pool*, not of the
  workqueue: every non-highpri BH wq on a cpu is served by that cpu's single BH
  pool, so a second phase queues alternately to two BH workqueues and the batch
  must still come out in one FIFO. A shared live counter must never exceed one,
  since a BH pool has a single execution context per cpu. Everything is queued
  with softirqs off so the whole batch is on the worklist before any of it runs.

* **`wqt_30_bh_softirq_gate`** — the softirq gate is the execution gate. An item
  queued locally while softirqs are off stays pending and runs before
  `local_bh_enable()` returns — there is no worker to wake. Nesting is honoured:
  only the outermost enable opens the gate. Queueing to a *remote* cpu from the
  same section takes the other kick path, since `kick_bh_pool()` cannot raise a
  softirq on another cpu and sends an irq_work instead.

* **`wqt_31_bh_highpri`** — on a BH workqueue `WQ_HIGHPRI` does not mean a
  negative-nice worker; there is no worker. It selects the cpu's second BH pool,
  driven by `HI_SOFTIRQ` (bit 0) rather than `TASKLET_SOFTIRQ` (bit 6). The
  normal batch is queued *first*, so queueing order would put it first and pool
  order puts the highpri batch first.

* **`wqt_32_bh_delayed`** — delayed queueing runs through a second deferral
  layer: the timer is armed on a housekeeping cpu and `delayed_work_timer_fn()`,
  itself in softirq, queues to `dwork->cpu`. The handoff has to preserve both
  the target cpu and the BH context, on top of the timing contract `wqt_04`
  checks for a normal wq.

* **`wqt_33_bh_cancel`** — `cancel_work_sync()` normally has to sleep, so BH is
  the exception: `__flush_work()` spots the `WORK_OFFQ_BH` tag the pool left in
  `work->data` and busy-waits instead, which is what makes it callable "from
  non-hardirq atomic contexts including BH". Driven once from a
  softirq-disabled section and once from inside a BH handler cancelling the item
  queued behind it. On a `DEBUG_ATOMIC_SLEEP` kernel a regression here shows up
  as "sleeping function called from invalid context", so the splat scan is a
  second oracle. Also covers `disable_work()` depth and
  `enable_and_queue_work()`.

* **`wqt_34_bh_requeue`** — four destinations from inside a BH handler: itself
  (which must never nest), a second BH wq on the same pool, a remote cpu, and a
  normal per-cpu wq — the handoff a BH item has to make when the work needs to
  sleep. The chain is finite on purpose: `is_chained_work()` identifies a worker
  by `PF_WQ_WORKER` on `current`, which no BH item has, so a requeue from a BH
  handler during `drain_workqueue()` (and therefore `destroy_workqueue()`) is
  not recognised as chained work and is dropped with a WARN.

* **`wqt_35_bh_hotplug`** — a BH pool has no worker to keep across an offline
  and nothing will raise its softirq again, so `CPUHP_SOFTIRQ_DEAD` drains it by
  hand: `workqueue_softirq_dead()` runs the dead pool's `bh_worker()` from a
  live cpu and waits. By the time `remove_cpu()` returns every BH item queued to
  that cpu has run, and the drained ones ran on *another* cpu — the one place a
  BH item does not run where it was queued. The batch is sized to outlast the
  offline (queueing is much cheaper than running, so the backlog builds while it
  is queued); the test fails if nothing came out of the drain, because then it
  stopped covering the path. Needs `CONFIG_HOTPLUG_CPU` and a second cpu.

* **`wqt_36_bh_driver_idiom`** — what `WQ_BH` was added for. A completion
  arrives in hardirq and the driver defers the short non-sleeping part to
  softirq: `drivers/md/dm-verity-target.c` (`verity_end_io()` →
  `queue_work(system_bh_wq, &io->work)`), `dm-crypt`, and a row of media and
  mailbox ISRs. The producer is an `irq_work` initialised with
  `IRQ_WORK_INIT_HARD()` so it really runs in hardirq. The second half is the
  teardown idiom from `drivers/media/pci/smipcie` and mantis:
  `disable_work_sync()` while the ISR may still fire, `enable_and_queue_work()`
  to bring it back.

### Tests 37–40: the sysfs interface, from userspace

Workqueue has **no configfs interface**. Its only userspace surface is the
sysfs bus registered at `core_initcall` by `wq_sysfs_init()`, visible as
`/sys/devices/virtual/workqueue` (and `/sys/bus/workqueue/devices`):

```
/sys/devices/virtual/workqueue/
├── cpumask               RW  global cap on every unbound workqueue
├── cpumask_requested     RO  the last mask accepted by a write
├── cpumask_isolated      RO  cpus excluded by isolation
└── <wq-name>/                only for workqueues created with WQ_SYSFS
    ├── per_cpu           RO  0 for unbound, 1 for per-cpu
    ├── max_active        RW  read-only for ordered and BH workqueues
    ├── nice              RW  ┐
    ├── cpumask           RW  │ unbound workqueues only
    ├── affinity_scope    RW  │
    └── affinity_strict   RW  ┘
```

Plus `/sys/module/workqueue/parameters/default_affinity_scope`, which supplies
the scope for every workqueue still set to `default`.

These tests are shell scripts rather than modules, so `test.sh` runs them
directly; they share `wqtest.sh`, the counterpart of `wqtest.h`, and print the
same `WQT-RESULT` verdict line on stdout. The runner folds that into the same
dmesg window it scans for splats, so a sysfs write that parses cleanly but
trips a `WARN` in the kernel still fails its test.

They need workqueues to look at, and the interface only shows workqueues
created with `WQ_SYSFS`, so **`wqh_sysfs.ko`** provides them: a per-cpu, an
unbound, an ordered and a highpri one, plus `wqh_hidden` without `WQ_SYSFS` as
a negative control. It is a helper, not a test — it stays loaded until removed,
and is deliberately not named `wqt_*` so `test.sh` does not try to run it as
one. There is no BH workqueue among them: `__WQ_BH_ALLOWS` permits only
`WQ_HIGHPRI` and `WQ_PERCPU` alongside `WQ_BH`, so `WQ_BH | WQ_SYSFS` is
rejected outright and a BH workqueue can never appear on the bus.

* **`wqt_37_sysfs_attrs`** — the surface. A `WQ_SYSFS` workqueue appears on the
  bus when created and is gone after `destroy_workqueue()`; one without the
  flag never appears. `per_cpu` is read-only and reports 1 only for the per-cpu
  queue (an ordered workqueue is unbound, so it reads 0). The unbound-only
  attributes exist on the unbound queues and not on the per-cpu one, and
  `max_active` is 0644 everywhere except the ordered queue, where
  `wq_sysfs_is_visible()` demotes it to 0444 because changing it would break
  the ordering guarantee. Defaults are checked too, including the
  `default (<resolved>)` form `affinity_scope` reports when unset.

* **`wqt_38_sysfs_write`** — writing. Every store handler parses its own buffer
  and answers `-EINVAL` rather than taking a garbage value, so `max_active`
  rejects `0`/`-1`/`abc` (but *clamps* rather than rejects above
  `WQ_MAX_ACTIVE`), `nice` rejects anything outside `-20..19`, and
  `affinity_scope` rejects any name `sysfs_match_string()` does not know. A
  reading back only shows the parse succeeded, so `nice` is also checked for
  effect: the helper runs a batch and reports the nice its workers ran at.
  Quirks pinned down: `affinity_strict` casts to bool, so `2` reads back as
  `1`; a zero-length write never reaches the store handler at all
  (`sysfs_kf_write()` short-circuits it) and changes nothing; and the ordered
  workqueue still takes `nice` and the affinity knobs, since only `max_active`
  is off limits there. That last one is never written to — the file is 0444,
  but root has `CAP_DAC_OVERRIDE` and the write would reach
  `workqueue_set_max_active()`, which `WARN`s on an ordered workqueue.

* **`wqt_39_sysfs_cpumask`** — the masks, and whether they bite. Both masks are
  written in the hex form `cpumask_parse()` takes and read back in the
  zero-padded, comma-grouped form `%*pb` produces, so the test normalises
  before comparing. A per-wq cpumask naming one cpu has to put all 64 probe
  items on that cpu; the global cpumask has to do the same to a workqueue that
  still asks for every cpu. An empty *global* mask is refused outright, while
  an empty *per-wq* mask is accepted and
  `wqattrs_actualize_cpumask()` falls back to the global mask for the pools it
  builds — so the test asserts only the invariant that survives either
  behaviour, that the workqueue keeps running work, and reports which way it
  went. `default_affinity_scope` is walked through every scope and must show up
  in a workqueue left on `default`; it rejects `default` itself and any unknown
  name. The global cpumask and `default_affinity_scope` are system-wide, so
  both are saved on entry and restored from an `EXIT` trap.

* **`wqt_40_sysfs_max_active_domains`** — the `max_active` value users see
  stays in the workqueue's active accounting domain. Per-cpu workqueues keep a
  scaled unbound-side shadow limit and unbound workqueues keep a per-cpu shadow
  limit, but `/sys/.../max_active` must report the value that the user set for
  the queue's actual domain. The test checks the creation-time defaults, writes
  per-cpu and unbound values that differ from their scaled shadows on a
  multi-CPU guest, verifies clamping/rejects on the per-cpu file, and runs a
  helper batch after the writes so a bad store path also shows up as lost work.

## How a test reports its result

A module test does all its work in `module_init()`, cleans up the workqueues it
created, prints exactly one verdict line, and returns `-EAGAIN` so it unloads
itself immediately (matching `lib/test_workqueue.c` in the kernel tree). The
verdict line is:

```
WQT-RESULT <id> <name> : PASS
WQT-RESULT <id> <name> : FAIL (<reason>)
```

A script test (37–40) does the same from userspace against `/sys`, using the
`wqtest.sh` helpers, and prints the identical line on stdout.

`test.sh` maps that (plus a scan for kernel splats in the same window) to an
`ok`/`not ok` TAP line. `insmod`'s exit status is intentionally ignored (it is
always non-zero because of the `-EAGAIN`).

## Layout

```
wqtest.h            shared PASS/FAIL harness (WQT_INIT / WQT_CHECK / WQT_FINISH)
wqt_NN_*.c          the 36 test modules
wqt_NN_*.sh         the 4 userspace (sysfs) tests
wqtest.sh           shared PASS/FAIL harness for those (counterpart of wqtest.h)
wqh_sysfs.c         helper module: the WQ_SYSFS workqueues they poke at
Kbuild / Makefile   out-of-tree module build
build.sh            build the modules against $KDIR (host or guest)
test.sh             in-VM: run every wqt_NN_* test, emit TAP
```

## Adding a test

A kernel-side test:

1. Create `wqt_NN_name.c`, include `"wqtest.h"`, do your checks in
   `module_init()` between `WQT_INIT(NN, "name")` and `return WQT_FINISH();`.
2. Add `obj-m += wqt_NN_name.o` to `Kbuild`.
3. (Optional) give it quick/full module params in `test.sh`'s `case "$id"`.

A userspace test:

1. Create an executable `wqt_NN_name.sh`, source `wqtest.sh`, do your checks
   between `wqt_init NN name` and `wqt_finish`.
2. Nothing to add to `Kbuild` — `test.sh` picks up `wqt_NN_*.sh` alongside the
   modules and orders both by `NN`. If it needs workqueues to look at, use
   `wqh_load`/`wqh_unload` and extend `wqh_sysfs.c`.
