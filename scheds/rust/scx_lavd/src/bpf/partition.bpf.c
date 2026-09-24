/* SPDX-License-Identifier: GPL-2.0 */
#include <scx/common.bpf.h>
#include <bpf_arena_common.bpf.h>
#include "intf.h"
#include "lavd.bpf.h"
#include "util.bpf.h"
#include "partition.bpf.h"
#include "partition_demand.bpf.h"
#include <errno.h>

extern const volatile u64 slice_max_ns;

const volatile struct partition_rule partition_rules[LAVD_PARTITION_MAX];
u32 partition_next_owner[LAVD_CPU_ID_MAX];
u32 partition_cpu_owner[LAVD_CPU_ID_MAX];
private(LAVD_PARTITION) struct bpf_cpumask partition_cpumask[LAVD_PARTITION_MAX];
static u64 partition_last_service[LAVD_PARTITION_MAX];
static u64 partition_native_service[LAVD_CPU_ID_MAX];

static u32 cpu_owner(u32 cpu)
{
	u32 *owner = MEMBER_VPTR(partition_cpu_owner, [cpu]);

	return owner ? READ_ONCE(*owner) : LAVD_PARTITION_MAX;
}

/* All writes are preferences: readers may safely see old or partial masks. */
static int apply_owners(void)
{
	struct bpf_cpumask *mask;
	u32 *next, *owner;
	int id, cpu;

	if (!nr_partitions)
		return 0;
	if (nr_partitions > LAVD_PARTITION_MAX)
		return -EINVAL;

	bpf_rcu_read_lock();
	bpf_for(id, 0, LAVD_PARTITION_MAX) {
		if (id >= nr_partitions)
			break;
		mask = MEMBER_VPTR(partition_cpumask, [id]);
		if (mask)
			bpf_cpumask_clear(mask);
	}
	bpf_for(cpu, 0, LAVD_CPU_ID_MAX) {
		if (cpu >= nr_cpu_ids)
			break;
		next = MEMBER_VPTR(partition_next_owner, [cpu]);
		owner = MEMBER_VPTR(partition_cpu_owner, [cpu]);
		if (!next || !owner)
			continue;
		id = READ_ONCE(*next);
		WRITE_ONCE(*owner, id);
		if (id < 0 || id >= nr_partitions)
			continue;
		mask = MEMBER_VPTR(partition_cpumask, [id]);
		if (mask)
			bpf_cpumask_set_cpu(cpu, mask);
	}
	bpf_rcu_read_unlock();
	return 0;
}

SEC("syscall")
int soft_partition_apply(void *ctx)
{
	return apply_owners();
}

__hidden int soft_partition_init(void)
{
	int id, err;

	if (!nr_partitions)
		return 0;
	if (nr_partitions > LAVD_PARTITION_MAX)
		return -EINVAL;
	bpf_for(id, 0, LAVD_PARTITION_MAX) {
		if (id >= nr_partitions)
			break;
		err = scx_bpf_create_dsq(partition_to_dsq(id), -1);
		if (err)
			return err;
	}
	return apply_owners();
}

/* First matching specific partition wins; zero is the catch-all. */
__hidden void soft_partition_refresh(struct task_struct *p, task_ctx *taskc)
{
	const volatile struct partition_rule *rule;
	char comm[TASK_COMM_LEN];
	u32 selected = 0;
	int id, prefix, ch;
	bool match;

	if (!nr_partitions)
		return;

	/* Hard affinity and migrate_disable remain on the native core queues. */
	if (test_task_flag(taskc, LAVD_FLAG_IS_AFFINITIZED) ||
	    is_effectively_pinned(taskc) || is_migration_disabled(p)) {
		WRITE_ONCE(taskc->partition_comm_valid, false);
		selected = LAVD_PARTITION_MAX;
		goto out;
	}

	if (bpf_probe_read_kernel(comm, sizeof(comm), p->comm))
		goto out;
	match = taskc->partition_comm_valid;
	bpf_for(ch, 0, TASK_COMM_LEN) {
		if (taskc->partition_comm[ch] != comm[ch])
			match = false;
		WRITE_ONCE(taskc->partition_comm[ch], comm[ch]);
	}
	if (match)
		return;
	WRITE_ONCE(taskc->partition_comm_valid, true);
	bpf_for(id, 1, LAVD_PARTITION_MAX) {
		if (id >= nr_partitions)
			break;
		rule = MEMBER_VPTR(partition_rules, [id]);
		if (!rule)
			break;
		bpf_for(prefix, 0, LAVD_PARTITION_PREFIX_MAX) {
			if (prefix >= rule->nr_prefixes)
				break;
			match = false;
			bpf_for(ch, 0, TASK_COMM_LEN) {
				if (!rule->prefixes[prefix][ch]) {
					match = ch > 0;
					break;
				}
				if (rule->prefixes[prefix][ch] != comm[ch])
					break;
			}
			if (match) {
				selected = id;
				goto out;
			}
		}
	}
out:
	soft_partition_reclassify(taskc, selected, scx_bpf_now());
}

/* Called under the picker's RCU lock; temp_mask is private scratch. */
__hidden s32 soft_partition_pick_cpu(struct pick_ctx *ctx, bool *is_idle)
{
	struct bpf_cpumask *preferred, *scratch = ctx->cpuc_cur->temp_mask;
	const struct cpumask *online;
	u32 id = ctx->taskc->partition_id;
	s32 cpu = -ENOENT, busy = -ENOENT;

	if (!nr_partitions || id >= nr_partitions || !scratch)
		return -ENOENT;
	preferred = MEMBER_VPTR(partition_cpumask, [id]);
	if (!preferred)
		return -ENOENT;

	online = scx_bpf_get_online_cpumask();
	bpf_cpumask_and(scratch, cast_mask(preferred), ctx->p->cpus_ptr);
	bpf_cpumask_and(scratch, cast_mask(scratch), online);
	scx_bpf_put_cpumask(online);

	/* Keep cache locality when the previous preferred CPU is idle. */
	if (bpf_cpumask_test_cpu(ctx->prev_cpu, cast_mask(scratch)) &&
	    scx_bpf_test_and_clear_cpu_idle(ctx->prev_cpu))
		cpu = ctx->prev_cpu;
	if (cpu < 0)
		cpu = scx_bpf_pick_idle_cpu(cast_mask(scratch), SCX_PICK_IDLE_CORE);
	if (cpu < 0)
		cpu = scx_bpf_pick_idle_cpu(cast_mask(scratch), 0);
	if (cpu >= 0)
		goto idle;

	busy = bpf_cpumask_any_distribute(cast_mask(scratch));
	/* Borrow idle capacity without turning the preference into affinity. */
	cpu = scx_bpf_pick_idle_cpu(ctx->p->cpus_ptr, SCX_PICK_IDLE_CORE);
	if (cpu < 0)
		cpu = scx_bpf_pick_idle_cpu(ctx->p->cpus_ptr, 0);
	if (cpu >= 0)
		goto idle;
	return busy < nr_cpu_ids ? busy : ctx->prev_cpu;
idle:
	*is_idle = true;
	return cpu;
}

__hidden bool soft_partition_pending(u32 cpu)
{
	u32 id = cpu_owner(cpu);

	return id < nr_partitions &&
	       scx_bpf_dsq_nr_queued(partition_to_dsq(id));
}

__hidden void soft_partition_running(task_ctx *taskc)
{
	u64 *last;

	if (!nr_partitions || taskc->partition_id >= nr_partitions)
		return;
	last = MEMBER_VPTR(partition_last_service, [taskc->partition_id]);
	if (last)
		WRITE_ONCE(*last, scx_bpf_now());
}

static bool consume_partition(u32 id, u64 now)
{
	u64 *last;

	if (id >= nr_partitions)
		return false;
	last = MEMBER_VPTR(partition_last_service, [id]);
	if (!last || !scx_bpf_dsq_move_to_local(partition_to_dsq(id), 0))
		return false;
	WRITE_ONCE(*last, now);
	return true;
}

__hidden bool soft_partition_can_refill(struct cpu_ctx *cpuc, task_ctx *prev)
{
	return prev && (prev->partition_id >= nr_partitions ||
		       prev->partition_id == cpu_owner(cpuc->cpu_id));
}

/*
 * Prefer the owner's queue. Native affinity work competes by LAVD deadline.
 * Rescue overdue queues before that comparison, including partitions with
 * no CPUs after hotplug or when there are more partitions than cores.
 * This is a service threshold, not a strict per-task latency guarantee.
 */
__hidden bool soft_partition_consume(struct cpu_ctx *cpuc, task_ctx *prev)
{
	u32 owner = cpu_owner(cpuc->cpu_id), rescue = LAVD_PARTITION_MAX;
	u64 now = scx_bpf_now(), oldest = now, threshold = 8 * slice_max_ns;
	u64 native_dsq = cpu_to_dsq(cpuc->cpu_id), native_vtime, owner_vtime;
	u64 *native_last, *last;
	int id;

	native_last = MEMBER_VPTR(partition_native_service, [cpuc->cpu_id]);
	if (!native_last)
		return false;
	native_vtime = peek_dsq_vtime(native_dsq);
	if (native_vtime != U64_MAX &&
	    time_delta(now, READ_ONCE(*native_last)) >= threshold &&
	    scx_bpf_dsq_move_to_local(native_dsq, 0)) {
		WRITE_ONCE(*native_last, now);
		return true;
	}
	bpf_for(id, 0, LAVD_PARTITION_MAX) {
		if (id >= nr_partitions)
			break;
		last = MEMBER_VPTR(partition_last_service, [id]);
		if (last && READ_ONCE(*last) < oldest &&
		    time_delta(now, READ_ONCE(*last)) >= threshold &&
		    scx_bpf_dsq_nr_queued(partition_to_dsq(id))) {
			oldest = READ_ONCE(*last);
			rescue = id;
		}
	}
	if (consume_partition(rescue, now))
		return true;

	owner_vtime = owner < nr_partitions ?
		peek_dsq_vtime(partition_to_dsq(owner)) : U64_MAX;
	if (native_vtime != U64_MAX && native_vtime <= owner_vtime &&
	    scx_bpf_dsq_move_to_local(native_dsq, 0)) {
		WRITE_ONCE(*native_last, now);
		return true;
	}
	if (consume_partition(owner, now))
		return true;
	if (native_vtime != U64_MAX && scx_bpf_dsq_move_to_local(native_dsq, 0)) {
		WRITE_ONCE(*native_last, now);
		return true;
	}

	/* An owned task still running is demand, even when its DSQ is empty. */
	if (prev && prev->partition_id == owner)
		return false;
	bpf_for(id, 0, LAVD_PARTITION_MAX) {
		if (id >= nr_partitions)
			break;
		if (consume_partition(id, now))
			return true;
	}
	return false;
}
