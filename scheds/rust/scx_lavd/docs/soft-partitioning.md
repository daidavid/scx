# Soft partitioning in LAVD

This experimental mode gives thread groups preferred CPU capacity using LAVD's
existing compute-domain queues. Queue homes stay fixed; CPU service grants resize
with demand. It creates **no partition-specific dispatch queues**.

## Start with enough queue homes

Every group, including the default, needs at least one startup LLC or virtual-LLC
unit. All compute domains of the same topology LLC (including different core
types) belong to the same home group. Too few units is a startup error. The
controller does not create virtual LLCs automatically.

For example, on a six-core homogeneous machine with one LLC, three two-core virtual LLCs
provide homes for the three groups in the example configuration:

```sh
sudo target/release/scx_lavd --performance --virt-llc=2-2 \
  --partition-config scheds/rust/scx_lavd/examples/soft-partitions.json
```

Choose the virtual topology for your machine; the example is not a universal
sizing recommendation. Virtualizing an LLC can increase LAVD's native queue count
at startup. Enabling soft partitions on that same topology adds no queues.

`--performance` keeps CPUs active. `--per-cpu-dsq` removes the shared queues this
mode needs and is rejected, as is a nonzero `--warm-cpu-us`. Native pinned-task
queues follow the existing `--pinned-slice-us` option; this feature does not force
them on. Use `--pinned-slice-us=0` to exercise domain queues without those queues.
Omit `--partition-config` for normal LAVD.

## Configuration

```json
{
  "partitions": [
    { "name": "render", "comm_prefixes": ["Render"] },
    { "name": "workers", "comm_prefixes": ["Worker", "Job"] },
    { "name": "default", "comm_prefixes": [] }
  ],
  "interval_ms": 1000,
  "granularity": "core"
}
```

`granularity` controls CPU grants, not queue topology:

| Value | Grant unit | SMT behavior |
| --- | --- | --- |
| `core` (default) | One physical core | Online siblings serve the same group. |
| `cpu` | One logical CPU | Siblings may serve different groups. |

Matching uses Linux thread `comm`, not the command line. Prefixes are case
sensitive; the first matching specific partition wins. An empty or omitted
`comm_prefixes` declares the default. It receives internal ID 0 regardless of
file position; other groups retain their relative order. If omitted, a group
named `default` is added automatically and also needs a queue home.

There may be at most 16 groups including the default, with unique nonempty names
and at most 8 prefixes each. A prefix contains 1–15 ASCII bytes with no NUL.
Multiple defaults, unknown fields, an empty partitions list, and conflicting names are
rejected. `interval_ms` defaults to 1000 and accepts 100–60000. Configuration is
read at startup. Renames take effect at the next classification callback,
including enqueue, selection, tick, or solo-task refill.

## Fixed homes, movable service

At startup, each whole LLC/vLLC unit is assigned to a group, balanced by unit
count. Its native steady and turbulent DSQs remain that group's homes for the
scheduler's lifetime. Ordinary unconstrained tasks queue in their group's homes.
CPU grants say which group each CPU should serve, independently of its physical
compute domain.

For example, Alpha's home may be vLLC 0 and Beta's home vLLC 1. When Alpha grows
onto one core in vLLC 1, that core consumes Alpha's queues in vLLC 0 into its
built-in LOCAL DSQ. After a slice, Alpha's work queues at an Alpha home again.
Beta's queues stay in vLLC 1 throughout. No queue drain or queue-owner transfer
is needed for this grant change.

This deliberately relaxes whole-LLC growth for the requested finer granularity.
Queue affiliation still follows whole LLC/vLLC boundaries; **CPU grants may split
a unit**. The allocator prefers own homes, then the same physical LLC, then the
same NUMA node. It packs necessary transfers in topology order and retains
existing grants when quotas do not change. It does not guarantee whole-unit
expansion, a single partially shared frontier, or continuous defragmentation.

Each group receives one grant unit when enough online units remain. Remaining
units are divided by smoothed demand using largest remainders; when every group has zero demand, allocation is equal. After hotplug, fewer units than groups gives the highest-demand
groups grants while queue rescue preserves a service path for others. Core mode
uses the primary CPU's owner word for both SMT siblings, including when only the
secondary sibling is online. The preferred placement masks are hints; dispatch
and preemption read the service owner.

## Service, borrowing, and exceptions

A waking task first seeks an idle granted CPU, then an allowed idle CPU elsewhere.
Dispatch prefers the service owner's home queues; unused capacity can pull other
groups' home queues. Regular slices, owner wakeup kicks, and tick checks reclaim
borrowed CPUs. Updating grants kicks affected runners. Already running or LOCAL
work can outlive an update until a scheduling transition; there is no instantaneous
handoff or hard reclaim-latency guarantee.

Pinned tasks, restricted-affinity tasks, and migration-disabled work use native
placement outside the participating groups. Their legal CPUs remain authoritative.
Native exception queue heads compete by deadline. Every eight maximum slices,
physical-queue rescue may also consume ordinary foreign work to uncover constrained
tasks behind it. Unserved home queues have a separate rescue path. These are
periodic service opportunities, not per-task latency guarantees. Native exceptions
and rescue can reduce the service owner's share under contention.

The existing steady/turbulent queue pairs remain in use. Tier thresholds aggregate
the CPUs actually granted to each group rather than the physical CPUs beside its
queues. Ordinary physical-domain balancing is bypassed because queue residence
and execution capacity no longer coincide. Queue-load accounting follows the
actual queue home; runtime accounting follows the executing CPU. Native cpu.max
admission and deferred enqueue paths remain active.

Demand estimates uncontended usage as `running / (running + sleeping)`; queue wait
is excluded. Per-task demand uses a 100 ms time constant and Q10 CPU units. The
controller computes counter deltas and applies an EWMA with one-quarter new
sample weight. Affinity-exempt work does not contribute to participating demand.

This is preferred capacity, not hard isolation, fixed shares, or CPU limits.
Equal grant counts do not imply equal compute capacity on heterogeneous CPUs or
split SMT siblings. Remote home access can add contention and NUMA/cache costs.
Home scans add work proportional to configured compute domains. Fixed homes may
be imbalanced; this prototype does not resize or migrate queue affiliation.

## Observe and validate

Startup logs distinguish `home compute domains` / `home CPUs` from changing
`CPUs` grants. On orderly exit, `owned_dispatch` counts successful pulls from
service-owner homes and `remote_home_dispatch` counts the subset whose queue
compute domain differs from the serving CPU's physical domain. These count
queue service, including native exceptions, rather than all task executions or
throughput. Direct LOCAL dispatch is not included. `native rescues` counts
successful periodic physical-queue pulls.

On a Linux host with the SCX build dependencies:

```sh
cargo build --release -p scx_lavd
cargo test -p scx_lavd
cc -std=gnu11 -Wall -Wextra -Werror -O2 \
  scheds/rust/scx_lavd/tests/partition_demand.c -o /tmp/lavd-demand
/tmp/lavd-demand
cc -std=gnu11 -Wall -Wextra -Werror -O2 -DTEST_NR_PARTITIONS=0 \
  scheds/rust/scx_lavd/tests/partition_demand.c -o /tmp/lavd-demand-disabled
/tmp/lavd-demand-disabled
sudo python3 scheds/rust/scx_lavd/tests/soft_partition.py \
  --binary target/release/scx_lavd --virt-llc=2-2
```

The smoke test enrolls only its worker processes in SCX and retains logs. It
requires at least six physical cores, at most 32 logical CPUs, full online CPU
affinity, and enough startup units for three homes. It covers borrowing, returning
owners, renames, affinity, both grant granularities, positive remote queue service,
insufficient-home rejection, and pinned-task progress with per-core queues disabled.

Before wider use, test actual SMT hardware, multiple NUMA nodes, heterogeneous
cores, hotplug, cpu.max, and saturated latency-sensitive workloads. Unit tests
cover topology grouping, split SMT vs whole cores, offline-primary staging,
capacity conservation, and stable allocation; they cannot establish hardware
performance or verifier portability. Linux build/load and workload results for
the development VM are recorded in the patch review bundle.

## Review boundaries

The prototype follows the earlier demand/classification patches with three
review units: pure configuration and grant policy; BPF queue-home service with
userspace integration; then usage and workload checks. The earlier partition-DSQ
branch remains available for comparison.

The initial demand-sizing work draws on
[scx_mitosis PR #3817](https://github.com/sched-ext/scx/pull/3817), reviewed at
`59337e20`. This LAVD prototype has one flat prefix-matched group layer, without
mitosis cgroup cells, nested subcells, or cgroup-path matching.
