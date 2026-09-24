/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LAVD_PARTITION_H
#define __LAVD_PARTITION_H

extern const volatile u32 nr_partitions;

#define partition_to_dsq(id) ((u64)(id) | ((u64)LAVD_DSQ_TYPE_PARTITION << LAVD_DSQ_TYPE_SHFT))

void soft_partition_refresh(struct task_struct *p, task_ctx *taskc);
s32 soft_partition_pick_cpu(struct pick_ctx *ctx, bool *is_idle);
bool soft_partition_consume(struct cpu_ctx *cpuc, task_ctx *prev);
bool soft_partition_can_refill(struct cpu_ctx *cpuc, task_ctx *prev);
bool soft_partition_pending(u32 cpu);
void soft_partition_running(task_ctx *taskc);
int soft_partition_init(void);

#endif
