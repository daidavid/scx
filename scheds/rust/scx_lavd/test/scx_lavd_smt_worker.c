#define _GNU_SOURCE

#include <errno.h>
#include <linux/sched.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <bpf/bpf.h>

#define strscpy(dst, src) strncpy(dst, src, sizeof(dst))
#include "task_local_data_user.h"

TLD_DEFINE_KEY(g_slice_ms_key, "slice_ms", sizeof(uint64_t));
TLD_DEFINE_KEY(g_lat_cri_key, "lat_cri", sizeof(uint64_t));
TLD_DEFINE_KEY(g_smt_exclusive_key, "smt_exclusive", sizeof(uint64_t));

static volatile sig_atomic_t g_stop;

struct opts {
	const char *label;
	const char *allowed_cpus;
	const char *map_path;
	int sched_ext;
	int yield_each_batch;
	uint64_t slice_ms;
	uint64_t phase1_lat_cri;
	uint64_t phase2_lat_cri;
	uint64_t phase1_ms;
	uint64_t phase1_loops;
	uint64_t phase2_ms;
	uint64_t phase2_loops;
	uint64_t phase1_smt;
	uint64_t phase2_smt;
	uint64_t batch_iters;
	uint64_t sleep_us_each_batch;
};

struct burn_result {
	uint64_t elapsed_ms;
	uint64_t completed_loops;
};

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static uint64_t now_mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int parse_cpu_list(const char *cpu_list, cpu_set_t *set)
{
	char *buf, *cursor, *saveptr = NULL;

	CPU_ZERO(set);
	buf = strdup(cpu_list);
	if (!buf)
		return -ENOMEM;

	for (cursor = strtok_r(buf, ",", &saveptr); cursor;
	     cursor = strtok_r(NULL, ",", &saveptr)) {
		char *dash = strchr(cursor, '-');
		long start, end, cpu;

		if (dash) {
			*dash = '\0';
			start = strtol(cursor, NULL, 10);
			end = strtol(dash + 1, NULL, 10);
			if (start < 0 || end < start) {
				free(buf);
				return -EINVAL;
			}
			for (cpu = start; cpu <= end; cpu++)
				CPU_SET((int)cpu, set);
		} else {
			cpu = strtol(cursor, NULL, 10);
			if (cpu < 0) {
				free(buf);
				return -EINVAL;
			}
			CPU_SET((int)cpu, set);
		}
	}

	free(buf);
	return 0;
}

static int set_sched_ext(void)
{
	struct sched_param param = {
		.sched_priority = 0,
	};

	if (sched_setscheduler(0, SCHED_EXT, &param) != 0)
		return -errno;

	return 0;
}

static int write_hints(int map_fd, uint64_t slice_ms, uint64_t lat_cri, uint64_t smt_exclusive)
{
	uint64_t *slice_ms_ptr;
	uint64_t *lat_cri_ptr;
	uint64_t *smt_exclusive_ptr;

	slice_ms_ptr = tld_get_data(map_fd, g_slice_ms_key);
	lat_cri_ptr = tld_get_data(map_fd, g_lat_cri_key);
	smt_exclusive_ptr = tld_get_data(map_fd, g_smt_exclusive_key);
	if (!slice_ms_ptr || !lat_cri_ptr || !smt_exclusive_ptr)
		return -ENOENT;

	*slice_ms_ptr = slice_ms;
	*lat_cri_ptr = lat_cri;
	*smt_exclusive_ptr = smt_exclusive;
	return 0;
}

static struct burn_result burn_cpu(uint64_t duration_ms, uint64_t target_loops,
				   uint64_t batch_iters, int yield_each_batch,
				   uint64_t sleep_us_each_batch)
{
	uint64_t start_ms = now_mono_ms();
	uint64_t end_ms = duration_ms ? start_ms + duration_ms : 0;
	uint64_t completed_loops = 0;
	volatile double x = 1.0000001;
	volatile double y = 1.0000003;
	volatile double z = 0.0000001;
	struct burn_result result;

	while (!g_stop) {
		if (duration_ms && now_mono_ms() >= end_ms)
			break;
		if (target_loops && completed_loops >= target_loops)
			break;
		for (uint64_t i = 0; i < batch_iters; i++) {
			x = x * y + z;
			y = y + 0.00000001;
			z = z + 0.000000001;
			if (x > 8.0)
				x = 1.0000001;
			if (y > 4.0)
				y = 1.0000003;
			if (z > 2.0)
				z = 0.0000001;
		}
		completed_loops++;
		if (yield_each_batch)
			sched_yield();
		if (sleep_us_each_batch)
			usleep((useconds_t)sleep_us_each_batch);
	}

	if (x == 0.0)
		fprintf(stderr, "impossible\n");

	result.elapsed_ms = now_mono_ms() - start_ms;
	result.completed_loops = completed_loops;
	return result;
}

static int parse_u64(const char *arg, uint64_t *out)
{
	char *end = NULL;
	unsigned long long val;

	errno = 0;
	val = strtoull(arg, &end, 10);
	if (errno || !end || *end != '\0')
		return -EINVAL;

	*out = (uint64_t)val;
	return 0;
}

static int parse_args(int argc, char **argv, struct opts *opts)
{
	int phase1_lat_cri_set = 0;
	int phase2_lat_cri_set = 0;
	int i;

	memset(opts, 0, sizeof(*opts));
	opts->label = "worker";
	opts->slice_ms = 20;
	opts->phase1_lat_cri = 60000;
	opts->phase2_lat_cri = 60000;
	opts->batch_iters = 1000000;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--label") && i + 1 < argc) {
			opts->label = argv[++i];
		} else if (!strcmp(argv[i], "--allowed-cpus") && i + 1 < argc) {
			opts->allowed_cpus = argv[++i];
		} else if (!strcmp(argv[i], "--map-path") && i + 1 < argc) {
			opts->map_path = argv[++i];
		} else if (!strcmp(argv[i], "--slice-ms") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->slice_ms))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--lat-cri") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->phase1_lat_cri))
				return -EINVAL;
			opts->phase2_lat_cri = opts->phase1_lat_cri;
			phase1_lat_cri_set = 1;
			phase2_lat_cri_set = 1;
		} else if (!strcmp(argv[i], "--phase1-lat-cri") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->phase1_lat_cri))
				return -EINVAL;
			phase1_lat_cri_set = 1;
		} else if (!strcmp(argv[i], "--phase2-lat-cri") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->phase2_lat_cri))
				return -EINVAL;
			phase2_lat_cri_set = 1;
		} else if (!strcmp(argv[i], "--phase1-ms") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->phase1_ms))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--phase1-loops") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->phase1_loops))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--phase2-ms") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->phase2_ms))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--phase2-loops") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->phase2_loops))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--phase1-smt") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->phase1_smt))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--phase2-smt") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->phase2_smt))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--batch-iters") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->batch_iters))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--sleep-us-each-batch") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->sleep_us_each_batch))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--yield-each-batch")) {
			opts->yield_each_batch = 1;
		} else if (!strcmp(argv[i], "--sched-ext")) {
			opts->sched_ext = 1;
		} else {
			fprintf(stderr, "unknown argument: %s\n", argv[i]);
			return -EINVAL;
		}
	}

	if (phase1_lat_cri_set && !phase2_lat_cri_set)
		opts->phase2_lat_cri = opts->phase1_lat_cri;

	if (!opts->allowed_cpus)
		return -EINVAL;

	if ((opts->phase1_ms == 0) == (opts->phase1_loops == 0))
		return -EINVAL;

	if (opts->phase2_ms && opts->phase2_loops)
		return -EINVAL;

	if (!opts->batch_iters)
		return -EINVAL;

	return 0;
}

int main(int argc, char **argv)
{
	cpu_set_t cpus;
	struct burn_result phase1_res, phase2_res;
	struct opts opts;
	int map_fd = -1;
	int ret;

	ret = parse_args(argc, argv, &opts);
	if (ret) {
		fprintf(stderr,
			"usage: %s --allowed-cpus CPU0,CPU1 "
			"(--phase1-ms N | --phase1-loops N) "
			"[--phase2-ms N | --phase2-loops N] "
			"[--map-path PATH] [--phase1-smt 0|1] "
			"[--phase2-smt 0|1] [--slice-ms N] [--lat-cri N] "
			"[--sched-ext] [--label NAME]\n",
			argv[0]);
		return 2;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	ret = parse_cpu_list(opts.allowed_cpus, &cpus);
	if (ret) {
		fprintf(stderr, "%s: failed to parse cpu list '%s': %s\n",
			opts.label, opts.allowed_cpus, strerror(-ret));
		return 1;
	}

	if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0) {
		perror("sched_setaffinity");
		return 1;
	}

	if (opts.sched_ext) {
		ret = set_sched_ext();
		if (ret) {
			fprintf(stderr, "%s: failed to enter SCHED_EXT: %s\n",
				opts.label, strerror(-ret));
			return 1;
		}
	}

	if (opts.map_path) {
		map_fd = bpf_obj_get(opts.map_path);
		if (map_fd < 0) {
			perror("bpf_obj_get");
			return 1;
		}
		ret = write_hints(map_fd, opts.slice_ms, opts.phase1_lat_cri,
				  opts.phase1_smt);
		if (ret) {
			fprintf(stderr, "%s: failed to write phase1 hints: %s\n",
				opts.label, strerror(-ret));
			return 1;
		}
	}

	printf("%s pid=%d phase=1 smt=%llu lat_cri=%llu cpu=%d\n",
	       opts.label,
	       getpid(),
	       (unsigned long long)opts.phase1_smt,
	       (unsigned long long)opts.phase1_lat_cri,
	       sched_getcpu());
	fflush(stdout);
	phase1_res = burn_cpu(opts.phase1_ms, opts.phase1_loops,
			      opts.batch_iters, opts.yield_each_batch,
			      opts.sleep_us_each_batch);
	printf("%s pid=%d phase=1_done elapsed_ms=%llu loops=%llu cpu=%d\n",
	       opts.label,
	       getpid(),
	       (unsigned long long)phase1_res.elapsed_ms,
	       (unsigned long long)phase1_res.completed_loops,
	       sched_getcpu());
	fflush(stdout);

	if (!g_stop && (opts.phase2_ms || opts.phase2_loops)) {
		if (opts.map_path) {
			ret = write_hints(map_fd, opts.slice_ms, opts.phase2_lat_cri,
					  opts.phase2_smt);
			if (ret) {
				fprintf(stderr,
					"%s: failed to write phase2 hints: %s\n",
					opts.label, strerror(-ret));
				return 1;
			}
		}
		printf("%s pid=%d phase=2 smt=%llu lat_cri=%llu cpu=%d\n",
		       opts.label,
		       getpid(),
		       (unsigned long long)opts.phase2_smt,
		       (unsigned long long)opts.phase2_lat_cri,
		       sched_getcpu());
		fflush(stdout);
		phase2_res = burn_cpu(opts.phase2_ms, opts.phase2_loops,
				      opts.batch_iters, opts.yield_each_batch,
				      opts.sleep_us_each_batch);
		printf("%s pid=%d phase=2_done elapsed_ms=%llu loops=%llu cpu=%d\n",
		       opts.label,
		       getpid(),
		       (unsigned long long)phase2_res.elapsed_ms,
		       (unsigned long long)phase2_res.completed_loops,
		       sched_getcpu());
		fflush(stdout);
	}

	printf("%s pid=%d done cpu=%d\n", opts.label, getpid(), sched_getcpu());
	fflush(stdout);

	if (map_fd >= 0)
		close(map_fd);
	tld_free();
	return 0;
}
