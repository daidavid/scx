#define _GNU_SOURCE
/*
 * TL;DR: Minimal pinned spin worker used to prove that sharing an SMT sibling
 * measurably slows a latency-sensitive thread on the selected CPU pair.
 */

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

struct opts {
	const char *label;
	int cpu;
	uint64_t duration_ms;
};

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static uint64_t now_ns(clockid_t clk_id)
{
	struct timespec ts;

	clock_gettime(clk_id, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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

static int parse_i32(const char *arg, int *out)
{
	char *end = NULL;
	long val;

	errno = 0;
	val = strtol(arg, &end, 10);
	if (errno || !end || *end != '\0' || val < 0 || val > 4096)
		return -EINVAL;

	*out = (int)val;
	return 0;
}

static int parse_args(int argc, char **argv, struct opts *opts)
{
	int i;

	memset(opts, 0, sizeof(*opts));
	opts->label = "worker";

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--label") && i + 1 < argc) {
			opts->label = argv[++i];
		} else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) {
			if (parse_i32(argv[++i], &opts->cpu))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--duration-ms") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opts->duration_ms))
				return -EINVAL;
		} else {
			fprintf(stderr, "unknown argument: %s\n", argv[i]);
			return -EINVAL;
		}
	}

	if (opts->duration_ms == 0)
		return -EINVAL;

	return 0;
}

static int pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0)
		return -errno;

	return 0;
}

int main(int argc, char **argv)
{
	const uint64_t batch_iters = 1000000ULL;
	struct opts opts;
	volatile double x = 1.0000001;
	volatile double y = 1.0000003;
	volatile double z = 0.0000001;
	uint64_t wall_start_ns, wall_end_ns, cpu_start_ns, cpu_end_ns;
	uint64_t deadline_ns, batches = 0;
	int ret;

	ret = parse_args(argc, argv, &opts);
	if (ret) {
		fprintf(stderr,
			"usage: %s --cpu N --duration-ms N [--label NAME]\n",
			argv[0]);
		return 2;
	}

	ret = pin_to_cpu(opts.cpu);
	if (ret) {
		fprintf(stderr, "%s: failed to pin to cpu %d: %s\n",
			opts.label, opts.cpu, strerror(-ret));
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	wall_start_ns = now_ns(CLOCK_MONOTONIC_RAW);
	cpu_start_ns = now_ns(CLOCK_THREAD_CPUTIME_ID);
	deadline_ns = wall_start_ns + opts.duration_ms * 1000000ULL;

	while (!g_stop && now_ns(CLOCK_MONOTONIC_RAW) < deadline_ns) {
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
		batches++;
	}

	wall_end_ns = now_ns(CLOCK_MONOTONIC_RAW);
	cpu_end_ns = now_ns(CLOCK_THREAD_CPUTIME_ID);

	printf("label=%s cpu=%d batches=%llu ops=%llu wall_ms=%llu cpu_ms=%llu final=%0.6f\n",
	       opts.label,
	       sched_getcpu(),
	       (unsigned long long)batches,
	       (unsigned long long)(batches * batch_iters),
	       (unsigned long long)((wall_end_ns - wall_start_ns) / 1000000ULL),
	       (unsigned long long)((cpu_end_ns - cpu_start_ns) / 1000000ULL),
	       (double)x);

	return 0;
}
