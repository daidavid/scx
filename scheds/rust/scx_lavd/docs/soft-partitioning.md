# Soft partitioning in LAVD

Soft partitions give groups of threads preferred CPU cores while allowing idle
capacity to be shared. This is an experimental, opt-in mode. From the repository
root, after building LAVD:

```sh
sudo target/release/scx_lavd --performance \
  --partition-config scheds/rust/scx_lavd/examples/soft-partitions.json
```

`--performance` keeps all CPUs active, avoiding interaction between core
compaction and partition ownership. `--per-cpu-dsq` and a nonzero `--warm-cpu-us`
cannot be combined with this mode. Omit `--partition-config` for normal LAVD.

## Configuration

The [example configuration](../examples/soft-partitions.json) groups `Render*`
threads, `Worker*` or `Job*` threads, and everything else:

```json
{
  "partitions": [
    { "name": "render", "comm_prefixes": ["Render"] },
    { "name": "workers", "comm_prefixes": ["Worker", "Job"] },
    { "name": "default", "comm_prefixes": [] }
  ],
  "interval_ms": 1000
}
```

Matching uses the thread's Linux `comm`, not its command line. A partition
matches when any prefix matches; the first matching specific partition wins.
Prefixes are case sensitive. An empty or omitted `comm_prefixes` declares the
default partition. It receives internal ID 0 regardless of its position in the
file; specific partitions retain their relative order. If omitted, a partition
named `default` is added automatically.

Names must be nonempty and unique. There may be at most 16 partitions including
the default, and at most 8 prefixes per partition. Each prefix must contain 1
to 15 ASCII bytes with no NUL. Multiple defaults, unknown fields, and an implicit
default whose name conflicts with a specific partition are rejected. The
partition list must be nonempty.

`interval_ms` defaults to 1000 and accepts 100 through 60000. It controls demand
sampling and ownership updates. Thread renames are reflected at the next CPU
selection, enqueue, scheduler tick, or solo-task slice refill. Configuration is
read at startup; restart LAVD to apply file edits. Startup and changed assignments are logged with partition names,
logical CPU IDs, and smoothed demand in CPU units.

## Scheduling and sizing

Each partition has a global dispatch queue ordered by LAVD virtual deadlines.
CPUs prefer their owner's work when groups compete. Waking tasks first seek idle
CPUs owned by their partition, then borrow idle CPUs elsewhere. Borrowing does
not change task affinity. A borrowed task is placed again after its slice instead
of retaining another partition's CPU indefinitely.

Tasks with restricted affinity, CPU pinning, or migration disabled use native
per-core queues outside the partition queues. Their legal CPUs remain authoritative.
Native work participates in deadline selection and has a service guard. Groups
whose queues have gone unserved also receive rescue service, including groups
with no owned cores. The rescue threshold is eight maximum slices; it is not a
latency guarantee when several groups compete.

Demand estimates how much CPU a thread would use without queue contention:
`running time / (running time + sleeping time)`. Queue wait is excluded, so a
CPU-bound thread remains hungry when delayed. Tasks smooth this estimate with a
100 ms time constant and accumulate demand over elapsed time in Q10 units
(1024 represents one CPU). Runtime accounting includes continuous execution and
slice extensions. Userspace divides counter deltas by elapsed time and applies
an EWMA with one-quarter weight on each new sample.

Allocation keeps SMT siblings together. Each partition receives one physical
core when enough cores exist, then the remaining cores are divided by demand
using largest remainders. All-zero demand starts with equal allocation. Existing
ownership is retained up to each new quota, limiting movement. If there are fewer
cores than partitions, the highest-demand groups each receive one core, with
partition ID breaking ties; queue rescue keeps the remaining groups schedulable.

These are preferred resources, not hard isolation, fixed shares, or CPU limits.
The allocator counts cores equally; it does not normalize heterogeneous core
capacity. Global partition queues also lack LAVD's normal per-LLC queue locality
and separate steady/turbulent queues. Evaluate these tradeoffs on the target
machine, particularly across NUMA nodes or mixed core types.

This design draws on [scx_mitosis PR #3817](https://github.com/sched-ext/scx/pull/3817),
reviewed at commit `59337e20`, particularly sibling CPU sharing and demand-based
sizing. LAVD exposes one flat partition layer; it does not implement mitosis
cgroup cells, nested subcells, or cgroup-path matching.

## Validation

From the repository root on a Linux build host with the SCX build dependencies:

```sh
cargo build --release -p scx_lavd
cargo test -p scx_lavd
```

Demand arithmetic tests also run without Linux or a BPF loader. Exercise both
the enabled implementation and disabled fast path:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -O2 \
  scheds/rust/scx_lavd/tests/partition_demand.c -o /tmp/lavd-demand
/tmp/lavd-demand
cc -std=gnu11 -Wall -Wextra -Werror -O2 -DTEST_NR_PARTITIONS=0 \
  scheds/rust/scx_lavd/tests/partition_demand.c -o /tmp/lavd-demand-disabled
/tmp/lavd-demand-disabled
```

For a verifier and attachment smoke test on a Linux kernel supporting this
LAVD build, run as root. `--partial` limits scheduling to tasks opting into SCX:

```sh
sudo timeout --signal=INT 10s target/release/scx_lavd --performance --partial \
  --partition-config scheds/rust/scx_lavd/examples/soft-partitions.json
```

An exit status of 124 is expected when `timeout` ends the test. Check the log
for successful attachment and orderly shutdown; an early verifier/load failure
is not a successful smoke test. Partial attachment alone does not exercise
partition scheduling. For workload validation, run without `--partial` on a test
machine, or arrange for the test tasks to opt into SCX.

The automated Linux smoke test creates its own configuration and enrolls only
its worker processes in SCX. It covers CPU borrowing, returning owner work,
renames (including a solo CPU-bound thread), affinity, and more partitions than
cores. It prints the directory containing retained logs and results:

```sh
sudo python3 scheds/rust/scx_lavd/tests/soft_partition.py \
  --binary target/release/scx_lavd
```

The test requires at least three physical cores and full online CPU affinity;
its module documentation describes larger-host skips. The manual matrix below
also covers workloads beyond this bounded smoke test.

| Scenario | What to check |
| --- | --- |
| Saturation | Run different counts of CPU-bound `Render*` and `Worker*` threads. Ownership should settle toward their demand ratio after floors. |
| Duty cycles | Compare many short-running sleepers with fewer CPU-bound threads. Size should follow work, not just thread count. |
| Idle borrowing | Stop one group's work. Busy groups should use its idle CPUs while its owned-core floor remains. |
| Rename | Have a worker change its own `comm` with `prctl(PR_SET_NAME)`. It should change groups at its next placement. |
| Affinity | Restrict workers with `taskset`, including a single logical CPU. Verify execution stays within their affinity. |
| CPU bandwidth | Enable `--enable-cpu-bw`, apply a cgroup v2 `cpu.max`, and verify throttling still limits the workload. |
| More groups than cores | Use more configured groups than physical cores, up to 16. Every runnable group must continue making progress. |
| CPU hotplug | Offline and restore a test CPU while groups are busy. Check ownership, forward progress, and scheduler logs. |

CPU lists are refreshed at the sampling interval. Ownership during hotplug is a
soft hint; an empty or stale preferred mask falls back to legal CPUs. A CPU whose
topology was absent at startup may require LAVD's existing restart path. Tests
above are a validation procedure, not a claim that a particular kernel or
machine has passed.

## Patch review boundaries

The series separates configuration and core allocation, task demand accounting,
BPF classification and queue service, the userspace sampling/controller loop,
and documentation. Configuration and arithmetic tests can be reviewed without
loading a scheduler. Verifier and workload checks validate the integration.
