/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Standalone arithmetic tests; no Linux kernel or BPF loader is required.
 * cc -std=gnu11 -Wall -Wextra -Werror -O2 tests/partition_demand.c -o /tmp/lavd-demand
 * /tmp/lavd-demand
 * Repeat with -DTEST_NR_PARTITIONS=0 to exercise the disabled fast path.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif
#define LAVD_PARTITION_MAX 16
#define LAVD_SHIFT 10
#define LAVD_SCALE (1U << LAVD_SHIFT)
#define WRITE_ONCE(x, v) ((x) = (v))
#define MEMBER_VPTR(array, index) (&(array index))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define MS 1000000ULL

typedef struct {
	u64 partition_window_at;
	u64 partition_run_ns;
	u64 partition_sleep_ns;
	u64 partition_sleep_at;
	u32 partition_id;
	u32 partition_account_id;
	u32 partition_demand_est;
} task_ctx;

#ifndef TEST_NR_PARTITIONS
#define TEST_NR_PARTITIONS 2
#endif
const volatile u32 nr_partitions = TEST_NR_PARTITIONS;
u64 partition_demand[LAVD_PARTITION_MAX];

static inline u64 time_delta(u64 now, u64 then)
{
	return now > then ? now - then : 0;
}

#include "../src/bpf/partition_demand.bpf.h"

static task_ctx new_task(void)
{
	task_ctx task;

	/* Simulate init_task inheriting another task's entire context. */
	memset(&task, 0xff, sizeof(task));
	soft_partition_reset(&task);
	memset(partition_demand, 0, sizeof(partition_demand));
	assert(task.partition_run_ns == 0);
	assert(task.partition_sleep_ns == 0);
	assert(task.partition_sleep_at == 0);
	assert(task.partition_account_id == 0);
	assert(task.partition_demand_est == 0);
	return task;
}

static void test_continuous_run(void)
{
	task_ctx task = new_task();
	u64 now = 1;

	soft_partition_runnable(&task, now);
	for (int i = 0; i < 30; i++) {
		now += 10 * MS;
		soft_partition_account(&task, 10 * MS, now);
	}
	/* Three 100 ms windows blend 0 -> 512 -> 768 -> 896. */
	assert(task.partition_demand_est == 896);
	assert(partition_demand[0] == (512 + 768 + 896) * 100 * MS);
	soft_partition_stopping(&task, now);
	assert(partition_demand[0] == (512 + 768 + 896) * 100 * MS);
}

static void test_queue_wait_is_excluded(void)
{
	task_ctx task = new_task();

	soft_partition_runnable(&task, 1);
	/* Wait 80 ms, then execute 20 ms: the sample is 1024, not 204. */
	soft_partition_account(&task, 20 * MS, 100 * MS + 1);
	soft_partition_stopping(&task, 100 * MS + 1);
	assert(task.partition_demand_est == 512);
	assert(partition_demand[0] == 512 * 100 * MS);
}

static void test_sleep_cycle_is_not_split_at_first_tick(void)
{
	task_ctx task = new_task();

	soft_partition_quiescent(&task, 1);
	soft_partition_runnable(&task, 200 * MS + 1);
	soft_partition_account(&task, 5 * MS, 205 * MS + 1);
	assert(partition_demand[0] == 0);
	soft_partition_account(&task, 15 * MS, 220 * MS + 1);
	soft_partition_stopping(&task, 220 * MS + 1);
	/* sample = floor(1024 * 20 / 220) = 93; blend = floor(93 * 220 / 320). */
	assert(task.partition_demand_est == 63);
	assert(partition_demand[0] == 63 * 220 * MS);
	soft_partition_quiescent(&task, 220 * MS + 1);
	soft_partition_stopping(&task, 1000 * MS + 1);
	assert(partition_demand[0] == 63 * 220 * MS);
}

static void test_reclassification_and_exempt_tasks(void)
{
	task_ctx task = new_task();
	u64 old_sum, new_sum;

	soft_partition_runnable(&task, 1);
	soft_partition_account(&task, 20 * MS, 20 * MS + 1);
	soft_partition_reclassify(&task, 1, 21 * MS + 1);
	assert(partition_demand[0] > 0);
	assert(partition_demand[1] == 0);
	old_sum = partition_demand[0];
	soft_partition_account(&task, 10 * MS, 31 * MS + 1);
	soft_partition_stopping(&task, 31 * MS + 1);
	assert(partition_demand[0] == old_sum);
	assert(partition_demand[1] > 0);
	new_sum = partition_demand[1];
	soft_partition_reclassify(&task, UINT32_MAX, 32 * MS + 1);
	soft_partition_account(&task, 50 * MS, 82 * MS + 1);
	soft_partition_stopping(&task, 82 * MS + 1);
	assert(partition_demand[0] == old_sum);
	assert(partition_demand[1] == new_sum);
}

static void test_large_intervals_are_bounded(void)
{
	task_ctx task = new_task();
	u64 now = 1ULL << 60;

	soft_partition_quiescent(&task, 1);
	soft_partition_runnable(&task, now);
	assert(task.partition_sleep_ns == LAVD_PARTITION_DEMAND_MAX_NS);
	soft_partition_account(&task, now, now + 1);
	assert(task.partition_demand_est <= LAVD_SCALE);
	assert(partition_demand[0] <= LAVD_SCALE * LAVD_PARTITION_DEMAND_MAX_NS);
}

int main(void)
{
	if (!nr_partitions) {
		task_ctx task = new_task();

		soft_partition_runnable(&task, 1);
		soft_partition_account(&task, 100 * MS, 100 * MS + 1);
		soft_partition_stopping(&task, 100 * MS + 1);
		soft_partition_quiescent(&task, 100 * MS + 1);
		assert(partition_demand[0] == 0);
		assert(task.partition_run_ns == 0);
	} else {
		test_continuous_run();
		test_queue_wait_is_excluded();
		test_sleep_cycle_is_not_split_at_first_tick();
		test_reclassification_and_exempt_tasks();
		test_large_intervals_are_bounded();
	}
	puts("partition demand tests passed");
	return 0;
}
