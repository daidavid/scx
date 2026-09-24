/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LAVD_PARTITION_DEMAND_H
#define __LAVD_PARTITION_DEMAND_H

/* Included after lavd.bpf.h: shares task_ctx and the Q10 LAVD scale. */
extern const volatile u32 nr_partitions;
extern u64 partition_demand[LAVD_PARTITION_MAX];

#define LAVD_PARTITION_DEMAND_TAU_NS (100ULL * 1000 * 1000)
#define LAVD_PARTITION_DEMAND_MAX_NS (1ULL << 40)

/*
 * A task contributes run / (run + sleep), rather than run / elapsed time.
 * Time queued for a CPU is excluded from the ratio: a CPU-bound task still
 * demands one CPU when contended. The estimate is integrated over elapsed
 * time, so userspace obtains the partition's demand by dividing counter
 * deltas by the sampling interval and LAVD_SCALE.
 *
 * Close ordinary windows at stopping, after a complete sleep/wait/run cycle.
 * Never close at wakeup. Long runs publish after 100 ms of actual execution
 * as well, including when dispatch extends the previous task's slice. Using
 * elapsed time for that threshold would prematurely close the first tick
 * after a long sleep with only a fraction of the following running burst.
 */
static __always_inline void soft_partition_reset(task_ctx *taskc)
{
	WRITE_ONCE(taskc->partition_id, 0);
	WRITE_ONCE(taskc->partition_account_id, 0);
	WRITE_ONCE(taskc->partition_window_at, 0);
	WRITE_ONCE(taskc->partition_run_ns, 0);
	WRITE_ONCE(taskc->partition_sleep_ns, 0);
	WRITE_ONCE(taskc->partition_sleep_at, 0);
	WRITE_ONCE(taskc->partition_demand_est, 0);
}

/* Membership may change at placement, after the old run was accounted. */
static __always_inline void soft_partition_sync_id(task_ctx *taskc, u64 now)
{
	if (taskc->partition_account_id == taskc->partition_id)
		return;

	taskc->partition_account_id = taskc->partition_id;
	taskc->partition_window_at = now;
	taskc->partition_run_ns = 0;
	taskc->partition_sleep_ns = 0;
	taskc->partition_sleep_at = 0;
	taskc->partition_demand_est = 0;
}

static __always_inline void soft_partition_publish(task_ctx *taskc, u64 now)
{
	u64 window, active, sample, demand, *sum;
	u32 id = taskc->partition_account_id;

	/* A sleeping task must not keep contributing its stale estimate. */
	if (!taskc->partition_run_ns || id >= nr_partitions ||
	    id >= LAVD_PARTITION_MAX)
		return;

	sum = MEMBER_VPTR(partition_demand, [id]);
	if (!sum)
		return;

	window = min(time_delta(now, taskc->partition_window_at),
		     LAVD_PARTITION_DEMAND_MAX_NS);
	if (!window)
		return;

	active = taskc->partition_run_ns + taskc->partition_sleep_ns;
	sample = (taskc->partition_run_ns << LAVD_SHIFT) / active;
	demand = taskc->partition_demand_est;
	/* BPF has no signed division. Both products are bounded by 2^50. */
	if (sample >= demand)
		demand += (sample - demand) * window /
			  (window + LAVD_PARTITION_DEMAND_TAU_NS);
	else
		demand -= (demand - sample) * window /
			  (window + LAVD_PARTITION_DEMAND_TAU_NS);

	taskc->partition_demand_est = demand;
	__sync_fetch_and_add(sum, demand * window);
	taskc->partition_window_at = now;
	taskc->partition_run_ns = 0;
	taskc->partition_sleep_ns = 0;
}

/* Account the old run first, including when reclassifying at slice refill. */
static __always_inline void soft_partition_reclassify(task_ctx *taskc,
						     u32 new_id, u64 now)
{
	if (taskc->partition_id == new_id)
		return;

	if (nr_partitions)
		soft_partition_publish(taskc, now);
	taskc->partition_id = new_id;
	taskc->partition_account_id = new_id;
	taskc->partition_window_at = now;
	taskc->partition_run_ns = 0;
	taskc->partition_sleep_ns = 0;
	if (taskc->partition_sleep_at)
		taskc->partition_sleep_at = now;
	/* Preserve the estimate: membership does not change task behavior. */
}

/* Call with the same wall-runtime delta charged by account_task_runtime(). */
static __always_inline void soft_partition_account(task_ctx *taskc,
						  u64 runtime_delta, u64 now)
{
	if (!nr_partitions)
		return;

	soft_partition_sync_id(taskc, now);
	if (!taskc->partition_window_at)
		taskc->partition_window_at = now - min(runtime_delta, now);
	taskc->partition_run_ns += min(runtime_delta,
		LAVD_PARTITION_DEMAND_MAX_NS - taskc->partition_run_ns);
	if (taskc->partition_run_ns >= LAVD_PARTITION_DEMAND_TAU_NS)
		soft_partition_publish(taskc, now);
}

/* Call after update_stat_for_stopping() has charged the final run delta. */
static __always_inline void soft_partition_stopping(task_ctx *taskc, u64 now)
{
	if (!nr_partitions)
		return;

	soft_partition_sync_id(taskc, now);
	soft_partition_publish(taskc, now);
}

static __always_inline void soft_partition_runnable(task_ctx *taskc, u64 now)
{
	if (!nr_partitions)
		return;

	soft_partition_sync_id(taskc, now);
	if (!taskc->partition_window_at)
		taskc->partition_window_at = now;
	if (taskc->partition_sleep_at) {
		taskc->partition_sleep_ns += min(
			time_delta(now, taskc->partition_sleep_at),
			LAVD_PARTITION_DEMAND_MAX_NS - taskc->partition_sleep_ns);
		taskc->partition_sleep_at = 0;
	}
}

/* Call only for SCX_DEQ_SLEEP; migration is not time voluntarily asleep. */
static __always_inline void soft_partition_quiescent(task_ctx *taskc, u64 now)
{
	if (!nr_partitions)
		return;

	soft_partition_sync_id(taskc, now);
	if (!taskc->partition_window_at)
		taskc->partition_window_at = now;
	if (!taskc->partition_sleep_at)
		taskc->partition_sleep_at = now;
}

#endif /* __LAVD_PARTITION_DEMAND_H */
