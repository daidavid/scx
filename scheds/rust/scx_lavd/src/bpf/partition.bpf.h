/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LAVD_PARTITION_H
#define __LAVD_PARTITION_H

extern const volatile u32 nr_partitions;

void soft_partition_refresh(struct task_struct *p, task_ctx *taskc);
s32 soft_partition_pick_cpu(struct pick_ctx *ctx, bool *is_idle);
bool soft_partition_consume(struct cpu_ctx *cpuc, task_ctx *prev);
bool soft_partition_can_refill(struct cpu_ctx *cpuc, task_ctx *prev);
bool soft_partition_pending(u32 cpu);
bool soft_partition_group_pending(u32 id);
u32 soft_partition_home(task_ctx *taskc, u32 physical_cpdom);
bool soft_partition_cpu_owned(task_ctx *taskc, u32 cpu);
bool soft_partition_kick_guest(task_ctx *taskc, s32 cpu);
void soft_partition_preempt_mask(struct task_struct *p, task_ctx *taskc,
				 struct bpf_cpumask *mask);
void soft_partition_update_service(void);
void soft_partition_running(task_ctx *taskc);
int soft_partition_init(void);

#endif
