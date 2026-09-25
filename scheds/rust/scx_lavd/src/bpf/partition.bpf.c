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
extern const volatile u16 lat_load_target_pct;
extern const volatile bool is_smt_active;

const volatile struct partition_rule partition_rules[LAVD_PARTITION_MAX];
/* Queue affiliation is fixed for the lifetime of this scheduler instance. */
const volatile u32 partition_cpdom_owner[LAVD_CPDOM_MAX_NR];
const volatile bool partition_cpu_granularity;
u32 partition_next_owner[LAVD_CPU_ID_MAX];
u32 partition_cpu_owner[LAVD_CPU_ID_MAX];
/* Successful service from owner home queues, including native exceptions. */
u64 partition_owner_dispatches[LAVD_PARTITION_MAX];
u64 partition_remote_dispatches[LAVD_PARTITION_MAX];
u64 partition_native_rescues;
private(LAVD_PARTITION) struct bpf_cpumask partition_cpumask[LAVD_PARTITION_MAX];
static u64 partition_last_service[LAVD_PARTITION_MAX];
static u64 partition_native_service[LAVD_CPU_ID_MAX];
static bool partition_initialized;

struct partition_service {
	u64 util_steady, util_turb;
	u64 cap_steady, cap_turb;
	u32 nr_steady;
	u32 vuln_thresh;
};
static struct partition_service partition_services[LAVD_PARTITION_MAX];

static u32 cpu_owner(u32 cpu)
{
	u32 *owner;

	/* Both SMT siblings consult the same word in core-grant mode. */
	if (!partition_cpu_granularity)
		cpu = get_primary_cpu(cpu);
	owner = MEMBER_VPTR(partition_cpu_owner, [cpu]);
	return owner ? READ_ONCE(*owner) : LAVD_PARTITION_MAX;
}

static u32 home_owner(u32 cpdom)
{
	const volatile u32 *owner = MEMBER_VPTR(partition_cpdom_owner, [cpdom]);

	return owner ? *owner : LAVD_PARTITION_MAX;
}

/* Masks are placement hints. Service and preemption consult cpu_owner(). */
static int apply_owners(void)
{
	const struct cpumask *online;
	struct bpf_cpumask *mask;
	u32 *next, *owner, old;
	int id, cpu;

	if (!nr_partitions)
		return 0;
	if (nr_partitions > LAVD_PARTITION_MAX)
		return -EINVAL;

	bpf_for(cpu, 0, LAVD_CPU_ID_MAX) {
		if (cpu >= nr_cpu_ids)
			break;
		next = MEMBER_VPTR(partition_next_owner, [cpu]);
		owner = MEMBER_VPTR(partition_cpu_owner, [cpu]);
		if (!next || !owner)
			continue;
		old = READ_ONCE(*owner);
		WRITE_ONCE(*owner, READ_ONCE(*next));
		if (partition_initialized && old != READ_ONCE(*owner) &&
		    (partition_cpu_granularity || cpu == get_primary_cpu(cpu))) {
			struct cpu_ctx *cpuc = get_cpu_ctx_id(cpu);
			const volatile u32 *sibling;

			if (cpuc && cpuc->is_online)
				scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
			sibling = MEMBER_VPTR(cpu_sibling, [cpu]);
			if (!partition_cpu_granularity && is_smt_active && sibling &&
			    *sibling != cpu) {
				cpuc = get_cpu_ctx_id(*sibling);
				if (cpuc && cpuc->is_online)
					scx_bpf_kick_cpu(*sibling, SCX_KICK_PREEMPT);
			}
		}
	}

	bpf_rcu_read_lock();
	online = scx_bpf_get_online_cpumask();
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
		if (!bpf_cpumask_test_cpu(cpu, online))
			continue;
		id = cpu_owner(cpu);
		if (id < 0 || id >= nr_partitions)
			continue;
		mask = MEMBER_VPTR(partition_cpumask, [id]);
		if (mask)
			bpf_cpumask_set_cpu(cpu, mask);
	}
	scx_bpf_put_cpumask(online);
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
	u32 homes = 0;
	int id, err;

	if (!nr_partitions)
		return 0;
	if (nr_partitions > LAVD_PARTITION_MAX || per_cpu_dsq || warm_cpu_ns)
		return -EINVAL;
	bpf_for(id, 0, LAVD_CPDOM_MAX_NR) {
		struct cpdom_ctx *cpdomc = MEMBER_VPTR(cpdom_ctxs, [id]);
		u32 owner = home_owner(id);

		if (cpdomc && cpdomc->is_valid && owner < nr_partitions)
			homes |= 1U << owner;
	}
	if (homes != ((1U << nr_partitions) - 1))
		return -EINVAL;
	bpf_for(id, 0, LAVD_PARTITION_MAX) {
		struct partition_service *service = MEMBER_VPTR(partition_services, [id]);

		if (service)
			service->vuln_thresh = LAVD_VULN_THRESH_INIT;
	}
	err = apply_owners();
	if (!err)
		partition_initialized = true;
	return err;
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

	/* Constrained work retains native placement; no extra DSQs are created. */
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
	if (selected != taskc->partition_id)
		WRITE_ONCE(taskc->partition_home_cpdom, LAVD_CPDOM_MAX_NR);
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

/* Choose queue residence independently of the execution CPU. */
__hidden u32 soft_partition_home(task_ctx *taskc, u32 physical_cpdom)
{
	struct cpdom_ctx *physical = MEMBER_VPTR(cpdom_ctxs, [physical_cpdom]);
	u32 id = taskc->partition_id, home = taskc->partition_home_cpdom;
	u32 best = LAVD_CPDOM_MAX_NR, best_score = U32_MAX;
	u64 best_load = U64_MAX;
	int domain;

	if (!nr_partitions || id >= nr_partitions)
		return physical_cpdom;
	if (home_owner(physical_cpdom) == id) {
		home = physical_cpdom;
		goto out;
	}
	if (home < LAVD_CPDOM_MAX_NR && home_owner(home) == id)
		return home;
	bpf_for(domain, 0, LAVD_CPDOM_MAX_NR) {
		struct cpdom_ctx *cpdomc;
		u32 score = 0;
		u64 load;

		if (domain >= nr_cpdoms)
			break;
		if (home_owner(domain) != id)
			continue;
		cpdomc = MEMBER_VPTR(cpdom_ctxs, [domain]);
		if (!cpdomc || !cpdomc->is_valid)
			continue;
		if (physical)
			score = 2 * (physical->numa_id != cpdomc->numa_id) +
				(physical->is_big != cpdomc->is_big);
		load = READ_ONCE(cpdomc->qload_invr);
		if (score < best_score || (score == best_score && load < best_load)) {
			best = domain;
			best_score = score;
			best_load = load;
		}
	}
	home = best;
out:
	WRITE_ONCE(taskc->partition_home_cpdom, home);
	return home;
}

__hidden bool soft_partition_group_pending(u32 id)
{
	int domain;

	if (id >= nr_partitions)
		return false;
	bpf_for(domain, 0, LAVD_CPDOM_MAX_NR) {
		if (domain >= nr_cpdoms)
			break;
		if (home_owner(domain) == id &&
		    (scx_bpf_dsq_nr_queued(cpdom_to_dsq(domain)) ||
		     scx_bpf_dsq_nr_queued(cpdom_to_turb_dsq(domain))))
			return true;
	}
	return false;
}

__hidden bool soft_partition_pending(u32 cpu)
{
	return soft_partition_group_pending(cpu_owner(cpu));
}

__hidden bool soft_partition_cpu_owned(task_ctx *taskc, u32 cpu)
{
	return taskc && (taskc->partition_id >= nr_partitions ||
		       taskc->partition_id == cpu_owner(cpu));
}

__hidden bool soft_partition_can_refill(struct cpu_ctx *cpuc, task_ctx *prev)
{
	return soft_partition_cpu_owned(prev, cpuc->cpu_id);
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

/* Reclaim a granted CPU from a guest without depending on LAVD urgency. */
__hidden bool soft_partition_kick_guest(task_ctx *taskc, s32 cpu)
{
	struct task_struct *curr;
	task_ctx *running;
	bool kick = false;

	if (!nr_partitions || taskc->partition_id >= nr_partitions || cpu < 0 ||
	    cpu_owner(cpu) != taskc->partition_id)
		return false;
	bpf_rcu_read_lock();
	curr = __COMPAT_scx_bpf_cpu_curr(cpu);
	if (curr && !rt_or_dl_task(curr)) {
		running = find_task_ctx(curr);
		/* Constrained native work is not a revocable participating guest. */
		kick = running && running->partition_id < nr_partitions &&
		       running->partition_id != taskc->partition_id;
	}
	bpf_rcu_read_unlock();
	if (kick)
		scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
	return kick;
}

__hidden void soft_partition_preempt_mask(struct task_struct *p, task_ctx *taskc,
					 struct bpf_cpumask *mask)
{
	struct bpf_cpumask *preferred;

	/* The non-sleepable enqueue callback supplies the RCU read-side scope. */
	preferred = MEMBER_VPTR(partition_cpumask, [taskc->partition_id]);
	if (preferred)
		bpf_cpumask_and(mask, cast_mask(preferred), p->cpus_ptr);
	else
		bpf_cpumask_clear(mask);
}

/* Physical-domain load ratios become invalid when its CPUs serve other homes. */
__hidden void soft_partition_update_service(void)
{
	int id, cpu, domain;

	if (!nr_partitions)
		return;
	bpf_for(id, 0, LAVD_PARTITION_MAX) {
		struct partition_service *s = MEMBER_VPTR(partition_services, [id]);

		if (!s)
			continue;
		s->util_steady = s->util_turb = 0;
		s->cap_steady = s->cap_turb = 0;
		s->nr_steady = 0;
	}
	bpf_for(cpu, 0, LAVD_CPU_ID_MAX) {
		struct cpu_ctx *cpuc;
		struct partition_service *s;

		if (cpu >= nr_cpu_ids)
			break;
		cpuc = get_cpu_ctx_id(cpu);
		if (!cpuc || !cpuc->is_online)
			continue;
		s = MEMBER_VPTR(partition_services, [cpu_owner(cpu)]);
		if (!s)
			continue;
		if (is_steady_cpu(cpuc)) {
			s->nr_steady++;
			s->util_steady += cpuc->util_est;
			s->cap_steady += cpuc->max_capacity;
		} else {
			s->util_turb += cpuc->util_est;
			s->cap_turb += cpuc->max_capacity;
		}
	}
	bpf_for(id, 0, LAVD_PARTITION_MAX) {
		struct partition_service *s = MEMBER_VPTR(partition_services, [id]);

		if (!s)
			continue;
		if (!s->cap_turb) {
			s->vuln_thresh = 0;
		} else if (s->cap_steady) {
			u64 high = (s->util_steady << LAVD_SHIFT) / s->cap_steady;
			u64 low = (s->util_turb << LAVD_SHIFT) / s->cap_turb;
			u64 target = high * lat_load_target_pct / 100;

			if (low > target && s->vuln_thresh)
				s->vuln_thresh--;
			else if (low < target && s->vuln_thresh < LAVD_VULN_THRESH_MAX)
				s->vuln_thresh++;
		}
	}
	bpf_for(domain, 0, LAVD_CPDOM_MAX_NR) {
		struct cpdom_ctx *cpdomc;
		struct partition_service *s;

		if (domain >= nr_cpdoms)
			break;
		cpdomc = MEMBER_VPTR(cpdom_ctxs, [domain]);
		s = MEMBER_VPTR(partition_services, [home_owner(domain)]);
		if (cpdomc && s)
			cpdomc->vuln_thresh = s->vuln_thresh;
	}
}

static bool can_serve_steady(struct cpu_ctx *cpuc, u32 domain, bool rescue)
{
	struct partition_service *s = MEMBER_VPTR(partition_services, [home_owner(domain)]);

	return rescue || !is_turbulent_cpu(cpuc) || !s || !READ_ONCE(s->nr_steady) ||
	       scx_bpf_dsq_nr_queued(cpdom_to_dsq(domain)) >
		       scx_bpf_dsq_nr_queued(cpdom_to_turb_dsq(domain));
}

static u64 domain_vtime(struct cpu_ctx *cpuc, u32 domain, bool rescue, u64 *dsq)
{
	u64 steady = can_serve_steady(cpuc, domain, rescue) ?
		peek_dsq_vtime(cpdom_to_dsq(domain)) : U64_MAX;
	u64 turbulent = peek_dsq_vtime(cpdom_to_turb_dsq(domain));

	*dsq = steady <= turbulent ? cpdom_to_dsq(domain) : cpdom_to_turb_dsq(domain);
	return min(steady, turbulent);
}

static u64 home_vtime(struct cpu_ctx *cpuc, u32 owner, bool rescue, u64 *dsq)
{
	u64 best = U64_MAX;
	int domain;

	*dsq = SCX_DSQ_INVALID;
	if (owner >= nr_partitions)
		return best;
	bpf_for(domain, 0, LAVD_CPDOM_MAX_NR) {
		u64 candidate, vtime;

		if (domain >= nr_cpdoms)
			break;
		if (home_owner(domain) != owner)
			continue;
		vtime = domain_vtime(cpuc, domain, rescue, &candidate);
		if (vtime < best) {
			best = vtime;
			*dsq = candidate;
		}
	}
	return best;
}

static bool move_home(struct cpu_ctx *cpuc, u32 owner, u64 dsq, u64 now)
{
	u64 *last, *count;

	if (dsq == SCX_DSQ_INVALID || !scx_bpf_dsq_move_to_local(dsq, 0))
		return false;
	last = MEMBER_VPTR(partition_last_service, [owner]);
	if (last)
		WRITE_ONCE(*last, now);
	if (owner == cpu_owner(cpuc->cpu_id)) {
		count = MEMBER_VPTR(partition_owner_dispatches, [owner]);
		if (count)
			__sync_fetch_and_add(count, 1);
		if (dsq_to_cpdom(dsq) != cpuc->cpdom_id) {
			count = MEMBER_VPTR(partition_remote_dispatches, [owner]);
			if (count)
				__sync_fetch_and_add(count, 1);
		}
	}
	return true;
}

static bool consume_home(struct cpu_ctx *cpuc, u32 owner, u64 now, bool rescue)
{
	u64 dsq, ignored;
	int domain;

	if (home_vtime(cpuc, owner, rescue, &dsq) == U64_MAX)
		return false;
	if (move_home(cpuc, owner, dsq, now))
		return true;
	/* A native affinity exception can make a head ineligible on this CPU. */
	bpf_for(domain, 0, LAVD_CPDOM_MAX_NR) {
		if (domain >= nr_cpdoms)
			break;
		if (home_owner(domain) != owner)
			continue;
		domain_vtime(cpuc, domain, rescue, &ignored);
		if (move_home(cpuc, owner, ignored, now))
			return true;
		if (can_serve_steady(cpuc, domain, rescue) &&
		    move_home(cpuc, owner, cpdom_to_dsq(domain), now))
			return true;
		if (move_home(cpuc, owner, cpdom_to_turb_dsq(domain), now))
			return true;
	}
	return false;
}

/* A deadline hint only: the eventual move can race another consumer. */
static u64 native_head_vtime(u64 dsq)
{
	struct task_struct *p;
	task_ctx *taskc;
	u64 vtime = U64_MAX;

	bpf_rcu_read_lock();
	p = __COMPAT_scx_bpf_dsq_peek(dsq);
	if (p) {
		taskc = find_task_ctx(p);
		if (!taskc || taskc->partition_id >= nr_partitions)
			vtime = p->scx.dsq_vtime;
	}
	bpf_rcu_read_unlock();
	return vtime;
}

static u64 native_vtime(struct cpu_ctx *cpuc, bool rescue, u64 *dsq)
{
	u64 best = U64_MAX, candidate, vtime;
	int tier;

	*dsq = SCX_DSQ_INVALID;
	if (use_per_cpu_dsq()) {
		candidate = cpu_to_dsq(cpuc->cpu_id);
		best = peek_dsq_vtime(candidate);
		if (best != U64_MAX)
			*dsq = candidate;
	}
	bpf_for(tier, 0, 2) {
		candidate = tier ? cpdom_to_turb_dsq(cpuc->cpdom_id) :
			cpdom_to_dsq(cpuc->cpdom_id);
		vtime = rescue ? peek_dsq_vtime(candidate) : native_head_vtime(candidate);
		if (vtime < best) {
			best = vtime;
			*dsq = candidate;
		}
	}
	return best;
}

static bool consume_native(struct cpu_ctx *cpuc, u64 dsq, u64 now, bool rescue)
{
	u64 *last;
	bool moved;

	moved = dsq != SCX_DSQ_INVALID && scx_bpf_dsq_move_to_local(dsq, 0);
	if (!moved && rescue) {
		struct dsq_entry dsqs[3] = {
			{ cpdom_to_dsq(cpuc->cpdom_id),
			  peek_dsq_vtime(cpdom_to_dsq(cpuc->cpdom_id)) },
			{ cpdom_to_turb_dsq(cpuc->cpdom_id),
			  peek_dsq_vtime(cpdom_to_turb_dsq(cpuc->cpdom_id)) },
			{ cpu_to_dsq(cpuc->cpu_id), use_per_cpu_dsq() ?
			  peek_dsq_vtime(cpu_to_dsq(cpuc->cpu_id)) : U64_MAX },
		};
		int i;

		/* The earliest queue may contain no task eligible on this CPU. */
		sort_dsqs(&dsqs[0], &dsqs[1], &dsqs[2]);
		for (i = 0; i < 3; i++) {
			if (dsqs[i].dsq_id == dsq || dsqs[i].vtime == U64_MAX)
				continue;
			if (scx_bpf_dsq_move_to_local(dsqs[i].dsq_id, 0)) {
				moved = true;
				break;
			}
		}
	}
	if (!moved)
		return false;
	last = MEMBER_VPTR(partition_native_service, [cpuc->cpu_id]);
	if (last)
		WRITE_ONCE(*last, now);
	if (rescue)
		__sync_fetch_and_add(&partition_native_rescues, 1);
	return true;
}

/*
 * Owner home queues get priority. Native exception heads compete by deadline;
 * every eight maximum slices, physical queues may also run foreign participants
 * to uncover constrained tasks behind them. This is bounded periodic service,
 * not a per-task latency guarantee or hard isolation. No BPF task-list scans or
 * extra queues are needed. Ordinary physical-domain balancing is bypassed.
 */
__hidden bool soft_partition_consume(struct cpu_ctx *cpuc, task_ctx *prev)
{
	u32 owner = cpu_owner(cpuc->cpu_id), rescue = LAVD_PARTITION_MAX;
	u64 now = scx_bpf_now(), oldest = now, threshold = 8 * slice_max_ns;
	u64 native_dsq, owned_dsq, native_time, owned_time;
	u64 *native_last, *last;
	int id;

	native_last = MEMBER_VPTR(partition_native_service, [cpuc->cpu_id]);
	if (native_last && time_delta(now, READ_ONCE(*native_last)) >= threshold) {
		native_vtime(cpuc, true, &native_dsq);
		if (consume_native(cpuc, native_dsq, now, true))
			return true;
	}
	/* Rescue a home with no usable CPU grants, including after hotplug. */
	bpf_for(id, 0, LAVD_PARTITION_MAX) {
		if (id >= nr_partitions)
			break;
		last = MEMBER_VPTR(partition_last_service, [id]);
		if (last && READ_ONCE(*last) < oldest &&
		    time_delta(now, READ_ONCE(*last)) >= threshold &&
		    soft_partition_group_pending(id)) {
			oldest = READ_ONCE(*last);
			rescue = id;
		}
	}
	if (consume_home(cpuc, rescue, now, true))
		return true;

	native_time = native_vtime(cpuc, false, &native_dsq);
	owned_time = home_vtime(cpuc, owner, false, &owned_dsq);
	if (native_time != U64_MAX && native_time <= owned_time &&
	    consume_native(cpuc, native_dsq, now, false))
		return true;
	if (consume_home(cpuc, owner, now, false))
		return true;
	if (consume_native(cpuc, native_dsq, now, false))
		return true;

	/* An owned runnable previous task is demand even with empty DSQs. */
	if (prev && prev->partition_id < nr_partitions && prev->partition_id == owner)
		return false;
	native_vtime(cpuc, true, &native_dsq);
	if (consume_native(cpuc, native_dsq, now, false))
		return true;
	bpf_for(id, 0, LAVD_PARTITION_MAX) {
		if (id >= nr_partitions)
			break;
		if (consume_home(cpuc, id, now, false))
			return true;
	}
	return false;
}
