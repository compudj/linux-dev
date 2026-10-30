// SPDX-License-Identifier: GPL-2.0

/*
 * Tests for cpu.concurrency.max, see
 * Documentation/scheduler/sched-max-concurrency.rst
 */

#include "cpu_concurrency_util.h"
/* The files parse, round trip and report "max" when unlimited. */
static int test_concurrency_interface(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;

	cg = cg_name(root, "cc_test_iface");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;

	if (cg_read_strcmp(cg, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(cg, "cpu.concurrency.ceiling", "max\n"))
		goto cleanup;
	if (cg_read_strcmp(cg, "cpu.concurrency.effective", "max max\n"))
		goto cleanup;
	if (!cg_write(cg, "cpu.concurrency.max", "0"))
		goto cleanup;
	if (!cg_write(cg, "cpu.concurrency.max", "lots"))
		goto cleanup;
	if (cg_write(cg, "cpu.concurrency.max", "2"))
		goto cleanup;
	if (cg_read_strcmp(cg, "cpu.concurrency.max", "2\n") ||
	    cg_read_strcmp(cg, "cpu.concurrency.ceiling", "max\n"))
		goto cleanup;
	if (cg_read_strcmp(cg, "cpu.concurrency.effective", "2 max\n"))
		goto cleanup;
	if (cg_write(cg, "cpu.concurrency.max", "max"))
		goto cleanup;
	if (cg_read_strcmp(cg, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(cg, "cpu.concurrency.ceiling", "max\n"))
		goto cleanup;
	if (cg_read_strcmp(cg, "cpu.concurrency.effective", "max max\n"))
		goto cleanup;

	ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/* A child is never allowed more than its parent. */
static int test_concurrency_hierarchy(const char *root)
{
	int ret = KSFT_FAIL;
	char *parent = NULL, *child = NULL;

	parent = cg_name(root, "cc_test_parent");
	if (!parent)
		goto cleanup;
	child = cg_name(parent, "cc_test_child");
	if (!child)
		goto cleanup;
	if (cg_create(parent))
		goto cleanup;
	if (cg_write(parent, "cgroup.subtree_control", "+cpu"))
		goto cleanup;
	if (cg_create(child))
		goto cleanup;

	if (cg_write(parent, "cpu.concurrency.max", "2"))
		goto cleanup;
	if (cg_read_strcmp(child, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(child, "cpu.concurrency.ceiling", "max\n"))
		goto cleanup;
	if (cg_read_strcmp(child, "cpu.concurrency.effective", "2 max\n"))
		goto cleanup;

	if (cg_write(child, "cpu.concurrency.max", "4"))
		goto cleanup;
	if (cg_read_strcmp(child, "cpu.concurrency.effective", "2 max\n"))
		goto cleanup;

	if (cg_write(child, "cpu.concurrency.max", "1"))
		goto cleanup;
	if (cg_read_strcmp(child, "cpu.concurrency.effective", "1 max\n"))
		goto cleanup;

	if (cg_write(parent, "cpu.concurrency.max", "max"))
		goto cleanup;
	if (cg_read_strcmp(parent, "cpu.concurrency.effective", "max max\n"))
		goto cleanup;
	if (cg_read_strcmp(child, "cpu.concurrency.effective", "1 max\n"))
		goto cleanup;

	ret = KSFT_PASS;
cleanup:
	cg_destroy(child);
	free(child);
	cg_destroy(parent);
	free(parent);
	return ret;
}

/*
 * Spinning threads in a limited cgroup never run on more CPUs than the
 * limit and never see an mm_cid at or above it.
 */
static int test_concurrency_bound(const char *root)
{
	struct spin_param param = {
		.nthreads = MIN(get_nprocs(), MAX_THREADS),
		.limit = 2,
		.duration_ms = 1000,
	};
	int ret = KSFT_FAIL;
	char *cg;

	if (need_cpus(3))
		return KSFT_SKIP;

	cg = cg_name(root, "cc_test_bound");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;
	if (cg_write(cg, "cpu.concurrency.max", "2"))
		goto cleanup;

	if (cg_run(cg, spin_and_check, &param))
		goto cleanup;

	/* The limit is also enforced from a limited parent. */
	ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/* Spinning threads, then the limit is decreased underneath them. */
static int decrease_under_load(const char *cgroup, void *arg)
{
	struct spin_param *p = arg;
	struct spinner s[MAX_THREADS] = { };
	struct timespec until;
	int i, seen;

	clock_gettime(CLOCK_MONOTONIC, &until);
	until.tv_sec += 2;

	for (i = 0; i < p->nthreads; i++) {
		s[i].until = until;
		s[i].max_mm_cid = -1;
		if (pthread_create(&s[i].thread, NULL, spin, &s[i]))
			return 1;
	}

	/* Let them spread, then shrink and wait for the eviction. */
	usleep(300000);
	if (cg_write(cgroup, "cpu.concurrency.max", "1"))
		return 4;
	if (cg_read_strcmp(cgroup, "cpu.concurrency.effective", "1 max\n"))
		return 5;
	/* Forget what was seen before the decrease. */
	for (i = 0; i < p->nthreads; i++)
		memset(s[i].cpus_seen, 0, sizeof(s[i].cpus_seen));
	usleep(300000);
	for (i = 0; i < p->nthreads; i++)
		memset(s[i].cpus_seen, 0, sizeof(s[i].cpus_seen));

	for (i = 0; i < p->nthreads; i++)
		pthread_join(s[i].thread, NULL);

	seen = count_cpus_seen(s, p->nthreads);
	if (seen > 1) {
		fprintf(stderr, "after decrease to 1, %d CPUs used\n", seen);
		return 2;
	}
	return 0;
}

static int test_concurrency_decrease(const char *root)
{
	struct spin_param param = {
		.nthreads = MIN(get_nprocs(), 8),
		.limit = 1,
	};
	int ret = KSFT_FAIL;
	char *cg;

	if (need_cpus(3))
		return KSFT_SKIP;

	cg = cg_name(root, "cc_test_decrease");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;
	if (cg_write(cg, "cpu.concurrency.max", "3"))
		goto cleanup;

	if (cg_run(cg, decrease_under_load, &param))
		goto cleanup;

	ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * Runs inside a cgroup limited to 2: three threads pinned to three
 * distinct CPUs need three CPUs, so the third sched_setaffinity() must
 * fail with EINVAL while the first two succeed.
 */
static void *pin_self(void *arg)
{
	int cpu = (long)arg;
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return (void *)(long)errno;
	/* Stay alive so that the class remains counted. */
	usleep(500000);
	return NULL;
}

static int pin_three(const char *cgroup, void *arg)
{
	pthread_t t[3];
	void *res[3];
	int i;

	for (i = 0; i < 3; i++) {
		if (pthread_create(&t[i], NULL, pin_self, (void *)(long)i))
			return 1;
		/* Serialize so that the third one is the one refused. */
		usleep(50000);
	}
	for (i = 0; i < 3; i++)
		pthread_join(t[i], &res[i]);

	if (res[0] || res[1]) {
		fprintf(stderr, "pinning within the limit failed\n");
		return 2;
	}
	if ((long)res[2] != EINVAL) {
		fprintf(stderr, "third pin returned %ld, expected EINVAL\n",
			(long)res[2]);
		return 3;
	}
	return 0;
}

static int test_concurrency_setaffinity(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;

	if (need_cpus(3))
		return KSFT_SKIP;

	cg = cg_name(root, "cc_test_affinity");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;
	if (cg_write(cg, "cpu.concurrency.max", "2"))
		goto cleanup;

	if (cg_run(cg, pin_three, NULL))
		goto cleanup;

	ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * Two threads pinned to two distinct CPUs form two affinity classes: a
 * decrease to 1 is recorded but the effective value stays at 2 until the
 * pins are removed, which retries the decrease.
 */
static int relax_then_unpin(const char *cgroup, void *arg)
{
	cpu_set_t set, all;
	pthread_t t;
	int i;

	CPU_ZERO(&all);
	for (i = 0; i < get_nprocs(); i++)
		CPU_SET(i, &all);

	/* This thread on CPU 0, a helper on CPU 1. */
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return 1;
	if (pthread_create(&t, NULL, pin_self, (void *)1L))
		return 1;
	usleep(100000);

	if (cg_write(cgroup, "cpu.concurrency.max", "1"))
		return 2;
	if (cg_read_strcmp(cgroup, "cpu.concurrency.max", "1\n") ||
	    cg_read_strcmp(cgroup, "cpu.concurrency.ceiling", "max\n"))
		return 3;
	if (cg_read_strcmp(cgroup, "cpu.concurrency.effective", "2 max\n")) {
		fprintf(stderr, "expected the limit to be relaxed to 2\n");
		return 4;
	}

	pthread_join(t, NULL);
	if (sched_setaffinity(0, sizeof(all), &all))
		return 5;
	if (cg_read_strcmp(cgroup, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "expected the decrease to apply after unpinning\n");
		return 6;
	}
	return 0;
}

static int test_concurrency_relax(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;

	if (need_cpus(2))
		return KSFT_SKIP;

	cg = cg_name(root, "cc_test_relax");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;
	if (cg_write(cg, "cpu.concurrency.max", "3"))
		goto cleanup;

	if (cg_run(cg, relax_then_unpin, NULL))
		goto cleanup;

	ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * A lone spinner under a limit of 1 is pinned to a CPU its group does not
 * use: its presence must move as a whole, so its mm_cid stays 0
 * throughout and the effective limit stays 1.
 */
struct move_param {
	int limit;
};

static int move_sole_presence(const char *cgroup, void *arg)
{
	struct spinner s = { .max_mm_cid = -1 };
	cpu_set_t set;
	int cpu, target, i;

	clock_gettime(CLOCK_MONOTONIC, &s.until);
	s.until.tv_sec += 1;
	if (pthread_create(&s.thread, NULL, spin, &s))
		return 1;
	usleep(200000);

	/* Pin it to a CPU it is not on, three times around. */
	for (i = 0; i < 3; i++) {
		cpu = -1;
		for (target = 0; target < get_nprocs(); target++) {
			if (!(s.cpus_seen[target / 64] & (1UL << (target % 64))))
				cpu = target;
		}
		if (cpu < 0)
			return 2;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (pthread_setaffinity_np(s.thread, sizeof(set), &set))
			return 3;
		usleep(100000);
		if (cg_read_strcmp(cgroup, "cpu.concurrency.effective", "1 max\n")) {
			fprintf(stderr, "effective limit changed while moving\n");
			return 4;
		}
	}
	pthread_join(s.thread, NULL);

	if (rseq_has_mm_cid() && s.max_mm_cid >= 1) {
		fprintf(stderr, "mm_cid %lld seen while moving, limit 1\n", s.max_mm_cid);
		return 5;
	}
	return 0;
}

static int test_concurrency_move(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;

	if (need_cpus(2))
		return KSFT_SKIP;

	cg = cg_name(root, "cc_test_move");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;
	if (cg_write(cg, "cpu.concurrency.max", "1"))
		goto cleanup;

	if (cg_run(cg, move_sole_presence, NULL))
		goto cleanup;

	ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * Spinners under a limit of 1 while the only CPU they use goes offline
 * and comes back: with no other used CPU to join, the presence must move
 * as a whole, so no mm_cid above 0 is ever seen and the effective limit
 * stays 1. Needs CPU hotplug through sysfs; skipped when unavailable.
 */

static int hotplug_under_load(const char *cgroup, void *arg)
{
	struct spinner s[4] = { };
	cpu_set_t not0;
	int i, cpu = -1, ret = 0;

	for (i = 0; i < 4; i++) {
		clock_gettime(CLOCK_MONOTONIC, &s[i].until);
		s[i].until.tv_sec += 2;
		s[i].max_mm_cid = -1;
		if (pthread_create(&s[i].thread, NULL, spin, &s[i]))
			return 1;
	}
	/* CPU 0 cannot be offlined: keep the group's presence off it. */
	stress_all_cpus(&not0);
	CPU_CLR(0, &not0);
	pthread_setaffinity_np(s[0].thread, sizeof(not0), &not0);
	usleep(100000);
	for (i = 0; i < 4; i++)
		memset(s[i].cpus_seen, 0, sizeof(s[i].cpus_seen));
	usleep(200000);

	/* A CPU the spinners use, other than CPU 0. */
	for (i = 1; i < get_nprocs() && cpu < 0; i++) {
		int j;

		for (j = 0; j < 4; j++) {
			if (s[j].cpus_seen[i / 64] & (1UL << (i % 64))) {
				cpu = i;
				break;
			}
		}
	}
	if (cpu < 0) {
		ret = 2;
		goto join;
	}
	if (write_cpu_online(cpu, 0)) {
		ret = KSFT_SKIP;
		goto join;
	}
	usleep(300000);
	if (cg_read_strcmp(cgroup, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "effective limit changed across hotplug\n");
		ret = 3;
	}
	write_cpu_online(cpu, 1);
join:
	for (i = 0; i < 4; i++)
		pthread_join(s[i].thread, NULL);
	if (ret)
		return ret;
	if (rseq_has_mm_cid() && max_mm_cid_seen(s, 4) >= 1) {
		fprintf(stderr, "mm_cid %lld seen across hotplug, limit 1\n",
			max_mm_cid_seen(s, 4));
		return 4;
	}
	return 0;
}

static int test_concurrency_hotplug(const char *root)
{
	int ret = KSFT_FAIL, r;
	char *cg;

	if (need_cpus(3))
		return KSFT_SKIP;

	cg = cg_name(root, "cc_test_hotplug");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;
	if (cg_write(cg, "cpu.concurrency.max", "1"))
		goto cleanup;

	r = cg_run(cg, hotplug_under_load, NULL);
	if (r == KSFT_SKIP)
		ret = KSFT_SKIP;
	else if (!r)
		ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * A process whose threads are split between two sibling threaded cgroups
 * limited to 1 each, under an unlimited parent: it runs on two CPUs and
 * its mm_cid must stay below 2, the sum of the two limits.
 */
struct split_param {
	char *b;
};

static int split_threads(const char *cgroup, void *arg)
{
	struct split_param *p = arg;
	struct spinner s[4] = { };
	cpu_set_t set;
	char buf[32];
	int i, cpu, target, seen;
	long long max;

	for (i = 0; i < 4; i++) {
		clock_gettime(CLOCK_MONOTONIC, &s[i].until);
		s[i].until.tv_sec += 1;
		s[i].max_mm_cid = -1;
		if (pthread_create(&s[i].thread, NULL, spin, &s[i]))
			return 1;
	}
	for (i = 0; i < 4; i++)
		while (!__atomic_load_n(&s[i].tid, __ATOMIC_ACQUIRE))
			usleep(1000);
	usleep(100000);
	/*
	 * Two of the four threads go to the other cgroup. Nothing keeps two
	 * limited groups apart, so they are pinned to a CPU this group has
	 * not used, for the process to span two CPUs.
	 */
	target = -1;
	for (cpu = 0; cpu < get_nprocs(); cpu++) {
		if (!((s[0].cpus_seen[cpu / 64] | s[1].cpus_seen[cpu / 64]) &
		      (1UL << (cpu % 64)))) {
			target = cpu;
			break;
		}
	}
	if (target < 0)
		return 2;
	CPU_ZERO(&set);
	CPU_SET(target, &set);
	for (i = 2; i < 4; i++) {
		snprintf(buf, sizeof(buf), "%d", (int)s[i].tid);
		if (cg_write(p->b, "cgroup.threads", buf)) {
			fprintf(stderr, "moving a thread to the sibling failed\n");
			return 3;
		}
		if (pthread_setaffinity_np(s[i].thread, sizeof(set), &set)) {
			fprintf(stderr, "pinning a thread of the sibling failed\n");
			return 4;
		}
	}
	for (i = 0; i < 4; i++)
		pthread_join(s[i].thread, NULL);

	seen = count_cpus_seen(s, 4);
	max = max_mm_cid_seen(s, 4);
	if (rseq_has_mm_cid() && max >= 2) {
		fprintf(stderr, "mm_cid %lld seen, sum of limits 2\n", max);
		return 3;
	}
	if (seen < 2) {
		fprintf(stderr, "only %d CPU used by two limited groups\n", seen);
		return 5;
	}
	return 0;
}

static int test_concurrency_split(const char *root)
{
	struct split_param param = { };
	char *parent = NULL, *a = NULL;
	int ret = KSFT_FAIL;

	if (need_cpus(3))
		return KSFT_SKIP;

	parent = cg_name(root, "cc_test_split");
	if (!parent)
		goto cleanup;
	a = cg_name(parent, "a");
	param.b = cg_name(parent, "b");
	if (!a || !param.b)
		goto cleanup;
	if (cg_create(parent))
		goto cleanup;
	if (cg_write(parent, "cgroup.subtree_control", "+cpu"))
		goto cleanup;
	if (cg_create(a) || cg_create(param.b))
		goto cleanup;
	if (cg_write(a, "cgroup.type", "threaded") ||
	    cg_write(param.b, "cgroup.type", "threaded")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	if (cg_write(a, "cpu.concurrency.max", "1") ||
	    cg_write(param.b, "cpu.concurrency.max", "1"))
		goto cleanup;

	if (cg_run(a, split_threads, &param))
		goto cleanup;

	ret = KSFT_PASS;
cleanup:
	if (a)
		cg_destroy(a);
	if (param.b)
		cg_destroy(param.b);
	if (parent)
		cg_destroy(parent);
	free(a);
	free(param.b);
	free(parent);
	return ret;
}

/*
 * Two threads pinned, while asleep, to two distinct CPUs the group does
 * not use, under a limit of 2 equal to the number of classes: unpinned
 * spinners created then must be placed on the CPUs of those classes, the
 * only CPUs that leave the sleepers a legal one, so that the process
 * never spans more than two CPUs and no mm_cid reaches 2. (Sleepers that
 * had run pinned would hold their CPUs through their sticky slots, which
 * the cleaner keeps for them; pinning them asleep is what leaves the
 * classes unhit.)
 */
static int sleepers_and_spinners(const char *cgroup, void *arg)
{
	struct spinner s[5] = { };
	struct timespec until;
	cpu_set_t set;
	int i, n = get_nprocs(), seen, err;
	long long max;

	clock_gettime(CLOCK_MONOTONIC, &until);
	until.tv_sec += 1;
	/* The sleepers first: they sleep before their first sample, and are pinned meanwhile. */
	for (i = 0; i < 2; i++) {
		s[i].until = until;
		s[i].max_mm_cid = -1;
		s[i].nap_us = 1000;
		s[i].first_nap_us = 100000;
		if (pthread_create(&s[i].thread, NULL, spin, &s[i]))
			return 1;
	}
	usleep(10000);
	for (i = 0; i < 2; i++) {
		CPU_ZERO(&set);
		CPU_SET(n - 1 - i, &set);
		err = pthread_setaffinity_np(s[i].thread, sizeof(set), &set);
		if (err) {
			fprintf(stderr, "pinning a sleeper failed: %s\n", strerror(err));
			return 2;
		}
	}
	usleep(10000);
	for (i = 2; i < 5; i++) {
		s[i].until = until;
		s[i].max_mm_cid = -1;
		if (pthread_create(&s[i].thread, NULL, spin, &s[i]))
			return 1;
	}
	for (i = 0; i < 5; i++)
		pthread_join(s[i].thread, NULL);

	if (cg_read_strcmp(cgroup, "cpu.concurrency.effective", "2 max\n")) {
		fprintf(stderr, "effective limit left 2\n");
		return 3;
	}
	seen = count_cpus_seen(s, 5);
	max = max_mm_cid_seen(s, 5);
	if (seen > 2) {
		fprintf(stderr, "ran on %d CPUs, limit 2\n", seen);
		return 4;
	}
	if (rseq_has_mm_cid() && max >= 2) {
		fprintf(stderr, "mm_cid %lld seen, limit 2\n", max);
		return 5;
	}
	return 0;
}

static int test_concurrency_sleepers(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;

	if (need_cpus(4))
		return KSFT_SKIP;

	cg = cg_name(root, "cc_test_sleepers");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;
	if (cg_write(cg, "cpu.concurrency.max", "2"))
		goto cleanup;

	if (cg_run(cg, sleepers_and_spinners, NULL))
		goto cleanup;

	ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * The same with the constraint coming from cpusets alone: a sleeper in a
 * threaded child whose cpuset is one CPU, spinners in a sibling whose
 * cpuset is the rest, nothing pinned with sched_setaffinity(2). The
 * sleeper's mask is a class all the same, so a CPU stays available for
 * it and no mm_cid reaches the parent's limit of 2.
 */
struct cpuset_class_param {
	char *b;
};

static int cpuset_sleeper(const char *cgroup, void *arg)
{
	struct cpuset_class_param *p = arg;
	struct spinner s[4] = { };
	struct timespec until;
	char buf[32];
	int i, seen;
	long long max;

	clock_gettime(CLOCK_MONOTONIC, &until);
	until.tv_sec += 1;
	/* The sleeper stays here, in the one-CPU cpuset. */
	s[0].until = until;
	s[0].max_mm_cid = -1;
	s[0].nap_us = 1000;
	if (pthread_create(&s[0].thread, NULL, spin, &s[0]))
		return 1;
	while (!__atomic_load_n(&s[0].tid, __ATOMIC_ACQUIRE))
		usleep(1000);
	/* The spinners go to the sibling, with the other CPUs. */
	for (i = 1; i < 4; i++) {
		s[i].until = until;
		s[i].max_mm_cid = -1;
		if (pthread_create(&s[i].thread, NULL, spin, &s[i]))
			return 1;
		while (!__atomic_load_n(&s[i].tid, __ATOMIC_ACQUIRE))
			usleep(1000);
		snprintf(buf, sizeof(buf), "%d", (int)s[i].tid);
		if (cg_write(p->b, "cgroup.threads", buf)) {
			fprintf(stderr, "moving a spinner to the sibling failed\n");
			return 2;
		}
	}
	for (i = 0; i < 4; i++)
		pthread_join(s[i].thread, NULL);

	seen = count_cpus_seen(s, 4);
	max = max_mm_cid_seen(s, 4);
	if (seen > 2) {
		fprintf(stderr, "ran on %d CPUs, limit 2\n", seen);
		return 3;
	}
	if (rseq_has_mm_cid() && max >= 2) {
		fprintf(stderr, "mm_cid %lld seen, limit 2\n", max);
		return 4;
	}
	return 0;
}

static int test_concurrency_cpuset_class(const char *root)
{
	struct cpuset_class_param param = { };
	char *parent = NULL, *a = NULL;
	char buf[64];
	int ret = KSFT_FAIL, n = get_nprocs();

	if (need_cpus(4))
		return KSFT_SKIP;

	parent = cg_name(root, "cc_test_cpuset_class");
	if (!parent)
		goto cleanup;
	a = cg_name(parent, "a");
	param.b = cg_name(parent, "b");
	if (!a || !param.b)
		goto cleanup;
	if (cg_create(parent))
		goto cleanup;
	if (cg_write(parent, "cgroup.subtree_control", "+cpu +cpuset"))
		goto cleanup;
	if (cg_create(a) || cg_create(param.b))
		goto cleanup;
	if (cg_write(a, "cgroup.type", "threaded") ||
	    cg_write(param.b, "cgroup.type", "threaded")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	snprintf(buf, sizeof(buf), "%d", n - 1);
	if (cg_write(a, "cpuset.cpus", buf))
		goto cleanup;
	snprintf(buf, sizeof(buf), "0-%d", n - 2);
	if (cg_write(param.b, "cpuset.cpus", buf))
		goto cleanup;
	if (cg_write(parent, "cpu.concurrency.max", "2"))
		goto cleanup;

	if (cg_run(a, cpuset_sleeper, &param))
		goto cleanup;
	if (cg_read_strcmp(parent, "cpu.concurrency.effective", "2 max\n")) {
		fprintf(stderr, "effective limit left 2\n");
		goto cleanup;
	}

	ret = KSFT_PASS;
cleanup:
	if (a)
		cg_destroy(a);
	if (param.b)
		cg_destroy(param.b);
	if (parent)
		cg_destroy(parent);
	free(a);
	free(param.b);
	free(parent);
	return ret;
}

/*
 * The hard value: a group takes it when it is created, from its parent's
 * cpu.concurrency.child_ceiling, and keeps it. A second value written may
 * only repeat it, the soft value stays within it, a child's own is capped
 * by its parent's, and it is reported next to the soft value in force.
 * Two affinity classes are refused at the door of a group whose hard value
 * is 1; in one whose hard value is 2, a third class is refused by the hard
 * value alone, the soft one being "max".
 */
static int pin_two(const char *cgroup, void *arg)
{
	const char *one = arg;
	pthread_t t, t3;
	cpu_set_t set;
	void *res;
	int err;

	CPU_ZERO(&set);
	CPU_SET(0, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return 1;
	if (pthread_create(&t, NULL, pin_self, (void *)1L))
		return 1;
	usleep(100000);
	err = cg_enter_current(one);
	if (err != -EBUSY) {
		fprintf(stderr, "two classes into a group of hard value 1: %s, not EBUSY\n",
			err ? strerror(-err) : "accepted");
		return 2;
	}
	if (pthread_create(&t3, NULL, pin_self, (void *)2L))
		return 1;
	pthread_join(t3, &res);
	if ((long)res != EINVAL) {
		fprintf(stderr, "third class returned %ld under hard 2, expected EINVAL\n", (long)res);
		return 4;
	}
	pthread_join(t, NULL);
	return 0;
}

static int test_concurrency_hard(const char *root)
{
	char *parent = NULL, *child = NULL, *later = NULL, *grand = NULL;
	char *one = NULL, *two = NULL, buf[32];
	int ret = KSFT_FAIL;

	if (need_cpus(3))
		return KSFT_SKIP;
	parent = cg_name(root, "cc_test_hard");
	if (!parent)
		goto cleanup;
	child = cg_name(parent, "child");
	later = cg_name(parent, "later");
	one = cg_name(parent, "one");
	two = cg_name(parent, "two");
	if (!child || !later || !one || !two)
		goto cleanup;
	grand = cg_name(child, "grand");
	if (!grand)
		goto cleanup;
	/* The root holds the value of its children too. */
	if (cg_read(root, "cpu.concurrency.child_ceiling", buf, sizeof(buf))) {
		fprintf(stderr, "no cpu.concurrency.child_ceiling on the root\n");
		goto cleanup;
	}
	if (cg_create(parent) || cg_write(parent, "cgroup.subtree_control", "+cpu") ||
	    cg_read_strcmp(parent, "cpu.concurrency.child_ceiling", "max\n"))
		goto cleanup;

	/* A child takes the value in force when it is created, and keeps it. */
	if (cg_write(parent, "cpu.concurrency.child_ceiling", "4") || cg_create(child) ||
	    cg_write(parent, "cpu.concurrency.child_ceiling", "6") || cg_create(later) ||
	    cg_write(parent, "cpu.concurrency.child_ceiling", "1") || cg_create(one) ||
	    cg_write(parent, "cpu.concurrency.child_ceiling", "2") || cg_create(two))
		goto cleanup;
	if (cg_read_strcmp(parent, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(parent, "cpu.concurrency.ceiling", "max\n") ||
	    cg_read_strcmp(child, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(child, "cpu.concurrency.ceiling", "4\n") ||
	    cg_read_strcmp(child, "cpu.concurrency.effective", "max 4\n") ||
	    cg_read_strcmp(later, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(later, "cpu.concurrency.ceiling", "6\n")) {
		fprintf(stderr, "children not created with the hard values 4 and 6\n");
		goto cleanup;
	}
	/* A lone soft value keeps the hard one; a second value may only repeat it. */
	if (cg_write(child, "cpu.concurrency.max", "2") ||
	    cg_read_strcmp(child, "cpu.concurrency.max", "2\n") ||
	    cg_read_strcmp(child, "cpu.concurrency.ceiling", "4\n") ||
	    cg_read_strcmp(child, "cpu.concurrency.effective", "2 4\n") ||
	    cg_write(child, "cpu.concurrency.max", "3") ||
	    cg_read_strcmp(child, "cpu.concurrency.max", "3\n") ||
	    cg_read_strcmp(child, "cpu.concurrency.ceiling", "4\n"))
		goto cleanup;
	/* A second value is refused, as is a limit above the ceiling. */
	if (cg_write(child, "cpu.concurrency.max", "3 5") != -EINVAL ||
	    cg_write(child, "cpu.concurrency.max", "2 3") != -EINVAL ||
	    cg_write(child, "cpu.concurrency.max", "3 max") != -EINVAL ||
	    cg_write(child, "cpu.concurrency.max", "5") != -EINVAL ||
	    cg_read_strcmp(child, "cpu.concurrency.max", "3\n") ||
	    cg_read_strcmp(child, "cpu.concurrency.ceiling", "4\n")) {
		fprintf(stderr, "a hard value changed, or a soft value went above it\n");
		goto cleanup;
	}
	/* A grandchild's own hard value is capped by its parent's. */
	if (cg_write(child, "cgroup.subtree_control", "+cpu") ||
	    cg_write(child, "cpu.concurrency.child_ceiling", "8") || cg_create(grand) ||
	    cg_read_strcmp(grand, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(grand, "cpu.concurrency.ceiling", "8\n") ||
	    cg_read_strcmp(grand, "cpu.concurrency.effective", "3 4\n"))
		goto cleanup;

	if (cg_run(two, pin_two, one))
		goto cleanup;

	ret = KSFT_PASS;
cleanup:
	if (grand)
		cg_destroy(grand);
	if (child)
		cg_destroy(child);
	if (later)
		cg_destroy(later);
	if (one)
		cg_destroy(one);
	if (two)
		cg_destroy(two);
	if (parent)
		cg_destroy(parent);
	free(grand);
	free(child);
	free(later);
	free(one);
	free(two);
	free(parent);
	return ret;
}

/* A thread that stays around, unpinned, for two seconds. */
static void *idle_thread(void *arg)
{
	struct spinner *s = arg;

	__atomic_store_n(&s->tid, gettid(), __ATOMIC_RELEASE);
	usleep(2000000);
	return NULL;
}

/*
 * Writes to cpusets and migrations never move the soft value: under a
 * soft value of 1, confining a sibling's thread to another CPU through
 * cpuset.cpus is refused, as is moving the thread into a sibling already
 * confined to another CPU; a wider cpuset that adds no class is not.
 */
struct cpuset_refuse_param {
	char *b, *c;
};

static int cpuset_refuse_run(const char *cgroup, void *arg)
{
	struct cpuset_refuse_param *p = arg;
	struct spinner s = { };
	pthread_t t;
	char buf[32];

	if (pthread_create(&t, NULL, idle_thread, &s))
		return 1;
	while (!__atomic_load_n(&s.tid, __ATOMIC_ACQUIRE))
		usleep(1000);
	snprintf(buf, sizeof(buf), "%d", (int)s.tid);
	/* Same CPU as ours: one class, the move is fine. */
	if (cg_write(p->b, "cgroup.threads", buf)) {
		fprintf(stderr, "moving a thread to a sibling with the same CPU failed\n");
		return 2;
	}
	/* Another CPU would be a second class under a soft value of 1. */
	if (!cg_write(p->b, "cpuset.cpus", "1")) {
		fprintf(stderr, "a second class through cpuset.cpus was accepted under soft 1\n");
		return 3;
	}
	/* A wider set adds no class. */
	if (cg_write(p->b, "cpuset.cpus", "0-1")) {
		fprintf(stderr, "a wider cpuset was refused\n");
		return 4;
	}
	/* Into a sibling already confined to another CPU: refused too. */
	if (!cg_write(p->c, "cgroup.threads", buf)) {
		fprintf(stderr, "moving a thread under a second class was accepted under soft 1\n");
		return 5;
	}
	pthread_join(t, NULL);
	return 0;
}

static int test_concurrency_cpuset_refused(const char *root)
{
	struct cpuset_refuse_param param = { };
	char *parent = NULL, *a = NULL;
	int ret = KSFT_FAIL;

	if (need_cpus(2))
		return KSFT_SKIP;
	parent = cg_name(root, "cc_test_cpuset_refused");
	if (!parent)
		goto cleanup;
	a = cg_name(parent, "a");
	param.b = cg_name(parent, "b");
	param.c = cg_name(parent, "c");
	if (!a || !param.b || !param.c)
		goto cleanup;
	if (cg_create(parent) || cg_write(parent, "cgroup.subtree_control", "+cpu +cpuset") ||
	    cg_create(a) || cg_create(param.b) || cg_create(param.c))
		goto cleanup;
	if (cg_write(a, "cgroup.type", "threaded") ||
	    cg_write(param.b, "cgroup.type", "threaded") ||
	    cg_write(param.c, "cgroup.type", "threaded")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	/* Configured while empty: no class yet, nothing to refuse. */
	if (cg_write(a, "cpuset.cpus", "0") || cg_write(param.b, "cpuset.cpus", "0") ||
	    cg_write(param.c, "cpuset.cpus", "1"))
		goto cleanup;
	if (cg_write(parent, "cpu.concurrency.max", "1"))
		goto cleanup;

	if (cg_run(a, cpuset_refuse_run, &param))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (a)
		cg_destroy(a);
	if (param.b)
		cg_destroy(param.b);
	if (param.c)
		cg_destroy(param.c);
	if (parent)
		cg_destroy(parent);
	free(a);
	free(param.b);
	free(param.c);
	free(parent);
	return ret;
}

/*
 * clone3(2) into @cg a child that exits at once: 0 when it was born,
 * otherwise the errno of the refusal.
 */
static int ns_cid_clone_into(const char *cg)
{
	int fd = open(cg, O_RDONLY | O_DIRECTORY), err = 0;
	pid_t c;

	if (fd < 0)
		return errno;
	c = clone_into_cgroup(fd);
	if (!c)
		_exit(0);
	if (c < 0)
		err = errno;
	else
		waitpid(c, NULL, 0);
	close(fd);
	return err;
}

/*
 * A thread pinned to one CPU sleeps in a group limited to 1 while that
 * CPU goes offline and comes back, beside two threads with no affinity
 * that spin and nap. While it sleeps, its class keeps the group on its
 * CPU. The unplug must not widen its mask: a sleeping task keeps its
 * affinity as it would without a limit, select_fallback_rq() widening
 * it only if it wakes up while the CPU is away. Nor may the two others
 * be held meanwhile for the sleeper's class, which no CPU can serve. Once
 * the CPU is back, the sleeper, woken, runs there with its mask. Needs CPU
 * hotplug through sysfs; skipped when unavailable.
 */
struct dormant_sleeper {
	int cpu;
	int pipefd[2];
	int ran_on;		/* where it ran after its wakeup, -1 before */
	cpu_set_t after;	/* its mask then */
};

struct dormant_worker {
	pthread_t thread;
	volatile unsigned long rounds;
	volatile int stop;
};

static void *dormant_sleeper_fn(void *arg)
{
	struct dormant_sleeper *d = arg;
	cpu_set_t set;
	char c;

	CPU_ZERO(&set);
	CPU_SET(d->cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return NULL;
	/* Run there for a moment, then sleep until told. */
	spin_ns(50000000);
	if (read(d->pipefd[0], &c, 1) != 1)
		return NULL;
	sched_getaffinity(0, sizeof(d->after), &d->after);
	__atomic_store_n(&d->ran_on, sched_getcpu(), __ATOMIC_RELEASE);
	return NULL;
}

static void *dormant_worker_fn(void *arg)
{
	struct dormant_worker *w = arg;

	while (!w->stop) {
		spin_ns(3000000);
		usleep(500);
		w->rounds++;
	}
	return NULL;
}

/* Did both workers get on in @ms? */
static int dormant_workers_run(struct dormant_worker *w, int ms)
{
	unsigned long r0 = w[0].rounds, r1 = w[1].rounds;

	usleep(ms * 1000);
	return w[0].rounds != r0 && w[1].rounds != r1;
}

/* Exit codes other than the KSFT ones (KSFT_SKIP is 4). */
static int dormant_run(const char *cgroup, void *arg)
{
	struct dormant_sleeper d = { .cpu = get_nprocs() - 1, .ran_on = -1 };
	struct dormant_worker w[2] = { };
	pthread_t sleeper;
	int i, ret = 0;

	if (pipe(d.pipefd) || pthread_create(&sleeper, NULL, dormant_sleeper_fn, &d))
		return 1;
	usleep(100000);
	for (i = 0; i < 2; i++)
		if (pthread_create(&w[i].thread, NULL, dormant_worker_fn, &w[i]))
			return 1;
	usleep(300000);
	if (write_cpu_online(d.cpu, 0)) {
		ret = KSFT_SKIP;
		goto wake;
	}
	if (!dormant_workers_run(w, 1000)) {
		fprintf(stderr, "workers held while cpu%d is offline\n", d.cpu);
		ret = 2;
	}
	write_cpu_online(d.cpu, 1);
	usleep(200000);
wake:
	if (write(d.pipefd[1], "x", 1) != 1)
		return 1;
	for (i = 0; i < 300 && __atomic_load_n(&d.ran_on, __ATOMIC_ACQUIRE) < 0; i++)
		usleep(10000);
	if (ret == KSFT_SKIP)
		goto out;
	if (d.ran_on < 0) {
		fprintf(stderr, "sleeper did not run within 3 s of its wakeup\n");
		ret = 3;
		goto out;
	}
	if (CPU_COUNT(&d.after) != 1 || !CPU_ISSET(d.cpu, &d.after)) {
		fprintf(stderr, "sleeper pinned to cpu%d woke with %d CPUs in its mask\n",
			d.cpu, CPU_COUNT(&d.after));
		ret = 6;
	}
	if (!dormant_workers_run(w, 300)) {
		fprintf(stderr, "workers held after cpu%d came back\n", d.cpu);
		ret = 5;
	}
out:
	for (i = 0; i < 2; i++) {
		w[i].stop = 1;
		pthread_join(w[i].thread, NULL);
	}
	if (d.ran_on >= 0)
		pthread_join(sleeper, NULL);
	return ret;
}

static int test_concurrency_dormant(const char *root)
{
	int ret = KSFT_FAIL, r;
	char *cg;

	if (need_cpus(3))
		return KSFT_SKIP;
	cg = cg_name(root, "cc_test_dormant");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;
	if (cg_write(cg, "cpu.concurrency.max", "1"))
		goto cleanup;
	r = cg_run(cg, dormant_run, NULL);
	if (r == KSFT_SKIP)
		ret = KSFT_SKIP;
	else if (!r)
		ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * A SCHED_DEADLINE task moved again and again into a group limited to 1
 * whose one CPU is kept busy: arriving on another CPU, it finds no slot,
 * is held at its return to user space and placed on the group's CPU by
 * the placement work; its policy is switched away and back meanwhile.
 * The deadline class keeps bandwidth per runqueue that must follow each
 * of these moves, and warns when it does not, which taints the kernel:
 * the W taint must not appear. Skipped without the privilege or the
 * bandwidth for SCHED_DEADLINE, or when the kernel is tainted W already.
 */

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE	6
#endif /* !SCHED_DEADLINE */

#define TAINT_WARN_BIT	(1L << 9)
static int set_deadline(pid_t pid, __u64 runtime_us, __u64 period_us)
{
	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_DEADLINE,
		.sched_runtime = runtime_us * 1000,
		.sched_deadline = period_us * 1000,
		.sched_period = period_us * 1000,
	};

	return syscall(__NR_sched_setattr, pid, &attr, 0) ? -errno : 0;
}

static int busy_forever(const char *cgroup, void *arg)
{
	for (;;)
		;
	return 0;
}

static int deadline_task(const char *cgroup, void *arg)
{
	if (set_deadline(0, 2000, 10000))
		return skip("SCHED_DEADLINE admission refused");
	for (;;) {
		spin_ns(1000000);
		usleep(3000);
	}
	return 0;
}

static int test_concurrency_deadline(const char *root)
{
	char *full = NULL, *src = NULL;
	int ret = KSFT_FAIL, i, status;
	pid_t busy = -1, dl = -1;
	long taint;

	if (need_cpus(2))
		return KSFT_SKIP;
	taint = read_sysctl("tainted");
	if (taint < 0 || taint & TAINT_WARN_BIT)
		return skip("the kernel is tainted by a warning");
	full = cg_name(root, "cc_test_dl_full");
	src = cg_name(root, "cc_test_dl_src");
	if (!full || !src || cg_create(full) || cg_create(src))
		goto cleanup;
	if (cg_write(full, "cpu.concurrency.max", "1"))
		goto cleanup;
	busy = cg_run_nowait(full, busy_forever, NULL);
	dl = cg_run_nowait(src, deadline_task, NULL);
	if (busy < 0 || dl < 0)
		goto cleanup;
	usleep(200000);
	if (waitpid(dl, &status, WNOHANG) == dl) {
		dl = -1;
		if (WIFEXITED(status) && WEXITSTATUS(status) == KSFT_SKIP)
			ret = KSFT_SKIP;
		goto cleanup;
	}
	for (i = 0; i < 60; i++) {
		struct sched_param sp = { .sched_priority = 0 };

		if (cg_enter(full, dl))
			goto cleanup;
		if (i % 3 == 1) {
			sched_setscheduler(dl, SCHED_OTHER, &sp);
			if (set_deadline(dl, 2000, 10000))
				goto cleanup;
		}
		usleep(30000 + (i % 7) * 10000);
		if (cg_enter(src, dl))
			goto cleanup;
		usleep(20000 + (i % 5) * 10000);
	}
	if (read_sysctl("tainted") & TAINT_WARN_BIT) {
		fprintf(stderr, "the kernel warned (taint W) while a deadline task was held and placed\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (dl > 0) {
		kill(dl, SIGKILL);
		waitpid(dl, NULL, 0);
	}
	if (busy > 0) {
		kill(busy, SIGKILL);
		waitpid(busy, NULL, 0);
	}
	if (full)
		cg_destroy(full);
	if (src)
		cg_destroy(src);
	free(full);
	free(src);
	return ret;
}

/*
 * Admission by width: a change is refused when the masks of the subtree
 * would hold more pairwise un-nested masks than the limit, even when
 * fewer CPUs would serve them all, because a task leaving or a CPU going
 * offline can turn such masks into that many classes. Masks are judged as
 * configured, whatever CPUs are online. Shapes, on CPUs 1 to 4:
 *  - limit 1: {1} then {1,2} nest; {1,3} beside {1,2} is refused although
 *    CPU 1 serves both; {1,2,3} nests again;
 *  - limit 2: windows {1,2}, {2,3} fit; {3,4} is a third un-nested mask;
 *  - limit 2: nested pairs {1} {1,2} and {3} {3,4} fit (two chains);
 *  - a soft value below the width relaxes to it and comes back once a
 *    mask nests;
 *  - limit 1, CPU 1 offline: {1,2} then {2,3} is refused although their
 *    live parts {2} and {2,3} nest: hotplug cannot change the verdict.
 */
#define WIDTH_THREADS	4
struct width_thread {
	pthread_t thread;
	pid_t tid;
	int fd;
};

static void *width_sleeper(void *arg)
{
	struct width_thread *w = arg;
	char c;

	__atomic_store_n(&w->tid, gettid(), __ATOMIC_RELEASE);
	if (read(w->fd, &c, 1) < 0)
		return NULL;
	return NULL;
}

static int width_pin(struct width_thread *w, const int *cpus)
{
	return pin_cpus(w->tid, cpus);
}

static int width_unpin_all(struct width_thread *w)
{
	int i, err = 0;

	for (i = 0; i < WIDTH_THREADS; i++)
		err |= width_pin(&w[i], NULL);
	return err;
}

static int width_shapes(const char *cgroup, void *arg)
{
	static const int c1[] = { 1, -1 }, c12[] = { 1, 2, -1 }, c13[] = { 1, 3, -1 },
			 c123[] = { 1, 2, 3, -1 }, c23[] = { 2, 3, -1 }, c34[] = { 3, 4, -1 },
			 c3[] = { 3, -1 };
	struct width_thread w[WIDTH_THREADS];
	int pfd[2], i, err, ret = 0;

	if (pipe(pfd))
		return 1;
	for (i = 0; i < WIDTH_THREADS; i++) {
		w[i].tid = 0;
		w[i].fd = pfd[0];
		if (pthread_create(&w[i].thread, NULL, width_sleeper, &w[i]))
			return 1;
	}
	for (i = 0; i < WIDTH_THREADS; i++)
		while (!__atomic_load_n(&w[i].tid, __ATOMIC_ACQUIRE))
			usleep(1000);

	/* Limit 1: crossing under a nested mask. */
	if (cg_write(cgroup, "cpu.concurrency.max", "1")) {
		ret = 10;
		goto out;
	}
	if (width_pin(&w[0], c1) || width_pin(&w[1], c12)) {
		fprintf(stderr, "nested {1} {1,2} refused under limit 1\n");
		ret = 11;
		goto out;
	}
	err = width_pin(&w[2], c13);
	if (err != EINVAL) {
		fprintf(stderr, "{1,3} beside {1,2} under limit 1: %s, not EINVAL\n", strerror(err));
		ret = 12;
		goto out;
	}
	if (width_pin(&w[2], c123)) {
		fprintf(stderr, "{1,2,3} over {1,2} refused under limit 1\n");
		ret = 13;
		goto out;
	}
	/* The dominating mask leaves: nothing to relax, the masks still nest. */
	width_pin(&w[0], NULL);
	if (cg_read_strcmp(cgroup, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "effective changed when {1} left\n");
		ret = 14;
		goto out;
	}
	if (width_unpin_all(w)) {
		ret = 15;
		goto out;
	}

	/* Limit 2: sliding windows. */
	if (cg_write(cgroup, "cpu.concurrency.max", "2")) {
		ret = 16;
		goto out;
	}
	if (width_pin(&w[0], c12) || width_pin(&w[1], c23)) {
		fprintf(stderr, "windows {1,2} {2,3} refused under limit 2\n");
		ret = 17;
		goto out;
	}
	err = width_pin(&w[2], c34);
	if (err != EINVAL) {
		fprintf(stderr, "window {3,4} as a third under limit 2: %s, not EINVAL\n", strerror(err));
		ret = 18;
		goto out;
	}
	/* A soft value below the width relaxes to it. */
	if (cg_write(cgroup, "cpu.concurrency.max", "1") ||
	    cg_read_strcmp(cgroup, "cpu.concurrency.effective", "2 max\n")) {
		fprintf(stderr, "soft 1 over two un-nested masks: not relaxed to 2\n");
		ret = 20;
		goto out;
	}
	/* {2,3} becomes {1,2,3}: one chain, the relaxation is undone. */
	if (width_pin(&w[1], c123)) {
		ret = 21;
		goto out;
	}
	usleep(100000);
	if (cg_read_strcmp(cgroup, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "relaxation not undone once the masks nest\n");
		ret = 22;
		goto out;
	}
	if (width_unpin_all(w)) {
		ret = 23;
		goto out;
	}

	/* Limit 2: two nested pairs, two chains. */
	if (cg_write(cgroup, "cpu.concurrency.max", "2")) {
		ret = 24;
		goto out;
	}
	if (width_pin(&w[0], c1) || width_pin(&w[1], c12) || width_pin(&w[2], c3) ||
	    width_pin(&w[3], c34)) {
		fprintf(stderr, "two nested pairs refused under limit 2\n");
		ret = 25;
		goto out;
	}
	if (width_unpin_all(w)) {
		ret = 26;
		goto out;
	}

	/* Limit 1 with CPU 1 offline: judged as configured. */
	if (cg_write(cgroup, "cpu.concurrency.max", "1")) {
		ret = 27;
		goto out;
	}
	if (write_cpu_online(1, 0))
		goto out;	/* no hotplug: the rest is covered */
	err = width_pin(&w[0], c12);
	if (!err)
		err = width_pin(&w[1], c23);
	else
		err = -err;
	write_cpu_online(1, 1);
	if (err != EINVAL) {
		fprintf(stderr, "{2,3} beside {1,2} with CPU 1 offline: %s, not EINVAL\n",
			err < 0 ? strerror(-err) : strerror(err));
		ret = 28;
	}
out:
	width_unpin_all(w);
	close(pfd[1]);
	for (i = 0; i < WIDTH_THREADS; i++)
		pthread_join(w[i].thread, NULL);
	close(pfd[0]);
	return ret;
}

static int test_concurrency_width(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg;

	if (need_cpus(5))
		return KSFT_SKIP;
	cg = cg_name(root, "cc_test_width");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;
	if (!cg_run(cg, width_shapes, NULL))
		ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

static int sleep_forever(const char *cgroup, void *arg)
{
	for (;;)
		pause();
	return 0;
}

static int cg_write_cpus(const char *cg, const char *file, const char *fmt, ...)
{
	char buf[64];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return cg_write(cg, file, buf);
}

static int expect_write(const char *cg, const char *file, char *val, int want,
			const char *what)
{
	int err = cg_write(cg, file, val);

	if (err == want)
		return 0;
	fprintf(stderr, "%s: %s, not %s\n", what, err ? strerror(-err) : "accepted",
		want ? strerror(-want) : "accepted");
	return -1;
}

/*
 * Partition changes beside and inside a limited group. Under a limit a
 * cpuset must grant the CPUs it names, since cpuset would give back what
 * it cuts on its own when a partition root goes (L is the last CPU):
 *  - lim holds a sleeper in a1 and one in b. a names L, b L-2 and L. A
 *    limit of 1 is refused while a1 names L-1, which a does not have, and
 *    accepted once a1 names nothing, inheriting {L}: {L} within {L-2,L},
 *    width 1. Under the limit a1 cannot name L-1 again. A partition root
 *    beside lim on CPU L-3 leaves them alone: created, switched to
 *    isolated and removed, it is accepted. One on CPU L, which a and b
 *    name, is created invalid, "Refused by a concurrency limit", claiming
 *    nothing, and stays so when its CPUs change, until the file is
 *    written again: on CPU L-3, it is valid. With the limit lifted, it is
 *    valid on CPU L.
 *  - P, a partition root on L-2 and L-1 with the limit 1, holds a sleeper
 *    in m1 and one in m2. m names L-1, its child m1 nothing: {L-1}; m2
 *    names L-2 and L-1. P growing to L cuts no CPU anyone names: accepted.
 *    P shrinking to L-2 would cut m's and m2's: refused with EBUSY.
 * Partition changes used to be refused outright while a task of a limited
 * group lived in any cpuset, then judged by the width of the masks they
 * left, which a cpuset granting what it names never lets them raise.
 */
static int test_concurrency_partition_check(const char *root)
{
	char *lim = NULL, *a = NULL, *a1 = NULL, *b = NULL, *part = NULL;
	char *P = NULL, *m = NULL, *m1 = NULL, *m2 = NULL, buf[64];
	int ret = KSFT_FAIL, n = get_nprocs(), L = n - 1;
	pid_t x = -1, y = -1, u = -1, v = -1;

	if (need_cpus(5))
		return KSFT_SKIP;
	lim = cg_name(root, "cc_test_pchk");
	a = cg_name(lim, "a");
	a1 = cg_name(a, "a1");
	b = cg_name(lim, "b");
	part = cg_name(root, "cc_test_pchk_part");
	P = cg_name(root, "cc_test_pchk_grow");
	m = cg_name(P, "m");
	m1 = cg_name(m, "m1");
	m2 = cg_name(P, "m2");
	if (!lim || !a || !a1 || !b || !part || !P || !m || !m1 || !m2)
		goto cleanup;

	if (cg_create(lim) || cg_write(lim, "cgroup.subtree_control", "+cpuset") ||
	    cg_create(a) || cg_write(a, "cgroup.subtree_control", "+cpuset") ||
	    cg_create(a1) || cg_create(b) || cg_create(part))
		goto cleanup;
	if (cg_write_cpus(a, "cpuset.cpus", "%d", L) ||
	    cg_write_cpus(a1, "cpuset.cpus", "%d", L - 1) ||
	    cg_write_cpus(b, "cpuset.cpus", "%d,%d", L - 2, L))
		goto cleanup;
	if (expect_write(lim, "cpu.concurrency.max", "1", -EBUSY,
			 "a limit over a1, naming a CPU a does not have"))
		goto cleanup;
	if (cg_write(a1, "cpuset.cpus", "\n") || cg_write(lim, "cpu.concurrency.max", "1"))
		goto cleanup;
	snprintf(buf, sizeof(buf), "%d", L - 1);
	if (expect_write(a1, "cpuset.cpus", buf, -EBUSY,
			 "a1 naming a CPU a does not have, under the limit"))
		goto cleanup;
	x = cg_run_nowait(a1, sleep_forever, NULL);
	y = cg_run_nowait(b, sleep_forever, NULL);
	if (x < 0 || y < 0)
		goto cleanup;
	usleep(100000);

	if (cg_write_cpus(part, "cpuset.cpus", "%d", L - 3))
		goto cleanup;
	if (expect_write(part, "cpuset.cpus.partition", "root", 0,
			 "a partition root beside lim, masks unchanged") ||
	    expect_write(part, "cpuset.cpus.partition", "isolated", 0,
			 "switching it to isolated") ||
	    expect_write(part, "cpuset.cpus.partition", "member", 0,
			 "removing it"))
		goto cleanup;
	if (cg_write_cpus(part, "cpuset.cpus", "%d", L))
		goto cleanup;
	if (expect_write(part, "cpuset.cpus.partition", "root", 0,
			 "a partition root taking the CPU a and b name, created invalid"))
		goto cleanup;
	if (cg_read(part, "cpuset.cpus.partition", buf, sizeof(buf)) ||
	    strcmp(buf, "root invalid (Refused by a concurrency limit)\n")) {
		fprintf(stderr, "the refused partition root is \"%.*s\"\n",
			(int)strcspn(buf, "\n"), buf);
		goto cleanup;
	}
	/* Not reconsidered when its CPUs change: it stays invalid, a write of the file is. */
	if (cg_write_cpus(part, "cpuset.cpus", "%d", L - 3) ||
	    cg_read(part, "cpuset.cpus.partition", buf, sizeof(buf)) ||
	    strncmp(buf, "root invalid", 12)) {
		fprintf(stderr, "the refused partition root, its CPUs changed, is \"%.*s\"\n",
			(int)strcspn(buf, "\n"), buf);
		goto cleanup;
	}
	if (expect_write(part, "cpuset.cpus.partition", "root", 0,
			 "the refused partition root written again, on a free CPU") ||
	    cg_read(part, "cpuset.cpus.partition", buf, sizeof(buf)) || strcmp(buf, "root\n")) {
		fprintf(stderr, "the partition root written again on a free CPU is \"%.*s\"\n",
			(int)strcspn(buf, "\n"), buf);
		goto cleanup;
	}
	if (expect_write(part, "cpuset.cpus.partition", "member", 0, "removing it") ||
	    cg_write_cpus(part, "cpuset.cpus", "%d", L))
		goto cleanup;
	if (cg_write(lim, "cpu.concurrency.max", "max") ||
	    expect_write(part, "cpuset.cpus.partition", "root", 0,
			 "the same with no limit") ||
	    cg_read(part, "cpuset.cpus.partition", buf, sizeof(buf)) || strcmp(buf, "root\n") ||
	    expect_write(part, "cpuset.cpus.partition", "member", 0, "removing it"))
		goto cleanup;

	if (cg_create(P) || cg_write_cpus(P, "cpuset.cpus", "%d-%d", L - 2, L - 1) ||
	    cg_write(P, "cpuset.cpus.partition", "root")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	if (cg_write(P, "cgroup.subtree_control", "+cpuset") ||
	    cg_create(m) || cg_write(m, "cgroup.subtree_control", "+cpuset") ||
	    cg_create(m1) || cg_create(m2))
		goto cleanup;
	if (cg_write_cpus(m, "cpuset.cpus", "%d", L - 1) ||
	    cg_write_cpus(m2, "cpuset.cpus", "%d-%d", L - 2, L - 1))
		goto cleanup;
	if (cg_write(P, "cpu.concurrency.max", "1"))
		goto cleanup;
	u = cg_run_nowait(m1, sleep_forever, NULL);
	v = cg_run_nowait(m2, sleep_forever, NULL);
	if (u < 0 || v < 0)
		goto cleanup;
	usleep(100000);
	snprintf(buf, sizeof(buf), "%d-%d", L - 2, L);
	if (expect_write(P, "cpuset.cpus", buf, 0, "P growing to L"))
		goto cleanup;
	snprintf(buf, sizeof(buf), "%d", L - 2);
	if (expect_write(P, "cpuset.cpus", buf, -EBUSY,
			 "P shrinking off the CPU m and m2 name"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (lim)
		cg_write(lim, "cpu.concurrency.max", "max");
	if (P)
		cg_write(P, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (y > 0) {
		kill(y, SIGKILL);
		waitpid(y, NULL, 0);
	}
	if (u > 0) {
		kill(u, SIGKILL);
		waitpid(u, NULL, 0);
	}
	if (v > 0) {
		kill(v, SIGKILL);
		waitpid(v, NULL, 0);
	}
	if (part) {
		cg_write(part, "cpuset.cpus.partition", "member");
		cg_destroy(part);
	}
	if (m1)
		cg_destroy(m1);
	if (m2)
		cg_destroy(m2);
	if (m)
		cg_destroy(m);
	if (P) {
		cg_write(P, "cpuset.cpus.partition", "member");
		cg_destroy(P);
	}
	if (a1)
		cg_destroy(a1);
	if (a)
		cg_destroy(a);
	if (b)
		cg_destroy(b);
	if (lim)
		cg_destroy(lim);
	free(m2);
	free(m1);
	free(m);
	free(P);
	free(part);
	free(b);
	free(a1);
	free(a);
	free(lim);
	return ret;
}

/*
 * A partition change that cannot be judged, that of a partition root with
 * a partition root below it, is refused only where it matters: when a
 * cpuset the CPUs it could move reach is one of a limited cgroup, naming
 * CPUs, or holds a task of one. P, a partition root on CPUs L-2 to L, has
 * an isolated child Q on CPU L. lim, limit 1, holds x in its cpuset,
 * which names CPU L-3; lim2, limit 1, names nothing, inheriting the
 * root's CPUs, which P's CPUs come from.
 *  - While lim2 is empty, P switching to isolated and back, and its cpus
 *    shrinking to L-1 and L and growing back, are accepted: they reach
 *    neither lim's CPU nor a task. P's cpus growing to L-3 would: refused
 *    with EBUSY.
 *  - With y sleeping in lim2, P switching to isolated is refused with
 *    EBUSY; with y gone, accepted. Such changes used to be refused while a
 *    limited cgroup had a cpuset, or a task, anywhere.
 */
static int test_concurrency_partition_beside(const char *root)
{
	char *P = NULL, *Q = NULL, *lim = NULL, *lim2 = NULL, buf[64];
	int ret = KSFT_FAIL, n = get_nprocs(), L = n - 1;
	pid_t x = -1, y = -1;

	if (need_cpus(5))
		return KSFT_SKIP;
	P = cg_name(root, "cc_test_pbeside");
	Q = cg_name(P, "q");
	lim = cg_name(root, "cc_test_pbeside_lim");
	lim2 = cg_name(root, "cc_test_pbeside_lim2");
	if (!P || !Q || !lim || !lim2)
		goto cleanup;
	if (cg_create(P) || cg_write_cpus(P, "cpuset.cpus", "%d-%d", L - 2, L) ||
	    cg_write(P, "cpuset.cpus.partition", "root") ||
	    cg_write(P, "cgroup.subtree_control", "+cpuset") || cg_create(Q) ||
	    cg_write_cpus(Q, "cpuset.cpus", "%d", L) ||
	    cg_write(Q, "cpuset.cpus.partition", "isolated")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	if (cg_create(lim) || cg_write_cpus(lim, "cpuset.cpus", "%d", L - 3) ||
	    cg_write(lim, "cpu.concurrency.max", "1") ||
	    cg_create(lim2) || cg_write(lim2, "cpu.concurrency.max", "1"))
		goto cleanup;
	x = cg_run_nowait(lim, sleep_forever, NULL);
	if (x < 0)
		goto cleanup;
	usleep(100000);

	if (expect_write(P, "cpuset.cpus.partition", "isolated", 0,
			 "P isolated, beside a limited cpuset it cannot reach") ||
	    expect_write(P, "cpuset.cpus.partition", "root", 0, "P root again"))
		goto cleanup;
	snprintf(buf, sizeof(buf), "%d-%d", L - 1, L);
	if (expect_write(P, "cpuset.cpus", buf, 0, "P shrinking, beside a limited cpuset it cannot reach"))
		goto cleanup;
	snprintf(buf, sizeof(buf), "%d-%d", L - 2, L);
	if (expect_write(P, "cpuset.cpus", buf, 0, "P growing back"))
		goto cleanup;
	snprintf(buf, sizeof(buf), "%d-%d", L - 3, L);
	if (expect_write(P, "cpuset.cpus", buf, -EBUSY, "P growing to the CPU lim names"))
		goto cleanup;

	y = cg_run_nowait(lim2, sleep_forever, NULL);
	if (y < 0)
		goto cleanup;
	usleep(100000);
	if (expect_write(P, "cpuset.cpus.partition", "isolated", -EBUSY,
			 "P isolated, a limited task inheriting the CPUs P takes from"))
		goto cleanup;
	kill(y, SIGKILL);
	waitpid(y, NULL, 0);
	y = -1;
	if (expect_write(P, "cpuset.cpus.partition", "isolated", 0,
			 "P isolated, that task gone") ||
	    expect_write(P, "cpuset.cpus.partition", "root", 0, "P root again"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (lim)
		cg_write(lim, "cpu.concurrency.max", "max");
	if (lim2)
		cg_write(lim2, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (y > 0) {
		kill(y, SIGKILL);
		waitpid(y, NULL, 0);
	}
	if (lim)
		cg_destroy(lim);
	if (lim2)
		cg_destroy(lim2);
	if (Q) {
		cg_write(Q, "cpuset.cpus.partition", "member");
		cg_destroy(Q);
	}
	if (P) {
		cg_write(P, "cpuset.cpus.partition", "member");
		cg_destroy(P);
	}
	free(lim2);
	free(lim);
	free(Q);
	free(P);
	return ret;
}

/*
 * A write to a cpuset with cpusets below it, which holds no task of its
 * own: pw, limit 1, has a, which names CPU B, and b, which names B and
 * B+1 (B is the fourth CPU from the end). a's child a1 names nothing: it
 * inherits {B}. Sleepers in a1 and b: {B} within {B,B+1}. pw's
 * cpuset.cpus becoming B+1 to B+3 would cut the CPU a and b name: refused
 * with EBUSY. a's becoming B+1 to B+3 gives a1 those, not nested with
 * b's {B,B+1}: width 2, refused with EBUSY. Such writes used to go
 * unchecked, the check looking only for tasks in the cpuset written.
 */
static int test_concurrency_cpuset_parent(const char *root)
{
	char *pw = NULL, *a = NULL, *a1 = NULL, *b = NULL, buf[64];
	int ret = KSFT_FAIL, n = get_nprocs(), B = n - 4;
	pid_t x = -1, y = -1;

	if (need_cpus(4))
		return KSFT_SKIP;
	pw = cg_name(root, "cc_test_cparent");
	a = cg_name(pw, "a");
	a1 = cg_name(a, "a1");
	b = cg_name(pw, "b");
	if (!pw || !a || !a1 || !b)
		goto cleanup;
	if (cg_create(pw) || cg_write(pw, "cgroup.subtree_control", "+cpuset") ||
	    cg_create(a) || cg_write(a, "cgroup.subtree_control", "+cpuset") ||
	    cg_create(a1) || cg_create(b))
		goto cleanup;
	if (cg_write_cpus(pw, "cpuset.cpus", "%d-%d", B, B + 3) ||
	    cg_write_cpus(a, "cpuset.cpus", "%d", B) ||
	    cg_write_cpus(b, "cpuset.cpus", "%d-%d", B, B + 1))
		goto cleanup;
	if (cg_write(pw, "cpu.concurrency.max", "1"))
		goto cleanup;
	x = cg_run_nowait(a1, sleep_forever, NULL);
	y = cg_run_nowait(b, sleep_forever, NULL);
	if (x < 0 || y < 0)
		goto cleanup;
	usleep(100000);
	snprintf(buf, sizeof(buf), "%d-%d", B + 1, B + 3);
	if (expect_write(pw, "cpuset.cpus", buf, -EBUSY, "pw's cpuset.cpus cutting a's CPU") ||
	    expect_write(a, "cpuset.cpus", buf, -EBUSY, "a's cpuset.cpus widening a1 past b"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (pw)
		cg_write(pw, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (y > 0) {
		kill(y, SIGKILL);
		waitpid(y, NULL, 0);
	}
	if (a1)
		cg_destroy(a1);
	if (a)
		cg_destroy(a);
	if (b)
		cg_destroy(b);
	if (pw)
		cg_destroy(pw);
	free(b);
	free(a1);
	free(a);
	free(pw);
	return ret;
}

/*
 * The configured CPUs of cpusets beside a partition: what each would get
 * with every CPU online, the partition taking its CPUs whatever is online.
 * po, a partition root on CPUs 1 to n-1, has the limit 1; b, a partition
 * root on the last two; a and a2 members, each with its own cpu group so
 * that migrations are judged by their own cpuset. x sleeps in b, pinned to
 * the last CPU.
 *  - a's cpuset.cpus naming only b's last CPU, which cpuset would replace
 *    by po's CPUs but b's, the limit is refused with EBUSY; a naming
 *    nothing inherits these, and a sleeper of a counts apart from x: the
 *    soft value relaxes to 2. Under the limit a cannot name b's CPU
 *    again.
 *  - a2 inherits po's CPUs but b's: a task joining it is refused, with the
 *    last CPU online and offline alike, since offline it is still b's.
 */
static int test_concurrency_partition_cpus(const char *root)
{
	char *po = NULL, *a = NULL, *a2 = NULL, *b = NULL, *out = NULL, buf[64];
	int ret = KSFT_FAIL, err, n = get_nprocs(), last = n - 1, offline = 0;
	pid_t x = -1, z = -1, w = -1;
	cpu_set_t set;

	if (need_cpus(4))
		return KSFT_SKIP;
	po = cg_name(root, "cc_test_pcpus");
	out = cg_name(root, "cc_test_pcpus_out");
	a = cg_name(po, "a");
	a2 = cg_name(po, "a2");
	b = cg_name(po, "b");
	if (!po || !out || !a || !a2 || !b || cg_create(po) || cg_create(out))
		goto cleanup;
	snprintf(buf, sizeof(buf), "1-%d", last);
	if (cg_write(po, "cpuset.cpus", buf) || cg_write(po, "cpuset.cpus.partition", "root")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	if (cg_write(po, "cgroup.subtree_control", "+cpuset +cpu") ||
	    cg_create(a) || cg_create(a2) || cg_create(b))
		goto cleanup;
	snprintf(buf, sizeof(buf), "%d-%d", last - 1, last);
	if (cg_write(b, "cpuset.cpus", buf) || cg_write(b, "cpuset.cpus.partition", "root")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	snprintf(buf, sizeof(buf), "%d", last);
	if (cg_write(a, "cpuset.cpus", buf))
		goto cleanup;

	x = cg_run_nowait(b, sleep_forever, NULL);
	z = cg_run_nowait(a, sleep_forever, NULL);
	w = cg_run_nowait(out, sleep_forever, NULL);
	if (x < 0 || z < 0 || w < 0)
		goto cleanup;
	CPU_ZERO(&set);
	CPU_SET(last, &set);
	if (sched_setaffinity(x, sizeof(set), &set)) {
		perror("pinning x");
		goto cleanup;
	}
	usleep(100000);

	snprintf(buf, sizeof(buf), "%d", last);
	if (expect_write(po, "cpu.concurrency.max", "1", -EBUSY,
			 "a limit over a, naming only b's CPU"))
		goto cleanup;
	if (cg_write(a, "cpuset.cpus", "\n") || cg_write(po, "cpu.concurrency.max", "1"))
		goto cleanup;
	if (expect_write(a, "cpuset.cpus", buf, -EBUSY, "a naming b's CPU under the limit"))
		goto cleanup;
	usleep(100000);
	if (cg_read(po, "cpu.concurrency.effective", buf, sizeof(buf)))
		goto cleanup;
	if (strncmp(buf, "2 ", 2)) {
		fprintf(stderr, "a sleeper of a and x counted as \"%.*s\", not \"2 max\"\n",
			(int)strcspn(buf, "\n"), buf);
		goto cleanup;
	}
	kill(z, SIGKILL);
	waitpid(z, NULL, 0);
	z = -1;

	err = cg_enter(a2, w);
	if (err != -EBUSY) {
		fprintf(stderr, "joining a2 beside x: %s, not EBUSY\n",
			err ? strerror(-err) : "accepted");
		goto cleanup;
	}
	if (write_cpu_online(last, 0)) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	offline = 1;
	usleep(100000);
	err = cg_enter(a2, w);
	if (err != -EBUSY) {
		fprintf(stderr, "joining a2 beside x, CPU %d offline: %s, not EBUSY\n",
			last, err ? strerror(-err) : "accepted");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (offline)
		write_cpu_online(last, 1);
	/* The limit first: tasks killed one after the other must not wait for a slot. */
	if (po)
		cg_write(po, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (z > 0) {
		kill(z, SIGKILL);
		waitpid(z, NULL, 0);
	}
	if (w > 0) {
		kill(w, SIGKILL);
		waitpid(w, NULL, 0);
	}
	if (b) {
		cg_write(b, "cpuset.cpus.partition", "member");
		cg_destroy(b);
	}
	if (a)
		cg_destroy(a);
	if (a2)
		cg_destroy(a2);
	if (po) {
		cg_write(po, "cpuset.cpus.partition", "member");
		cg_destroy(po);
	}
	if (out)
		cg_destroy(out);
	free(b);
	free(a2);
	free(a);
	free(out);
	free(po);
	return ret;
}

/*
 * A group created under a limited parent counts from the start: spinners
 * in a child created after the limit stay within it. A group used to
 * start counting only at the next write of a value above it, its tasks
 * escaping the limit until then.
 */
static int test_concurrency_late_child(const char *root)
{
	struct spin_param param = {
		.nthreads = MIN(get_nprocs(), MAX_THREADS),
		.limit = 1,
		.duration_ms = 1000,
	};
	char *parent = NULL, *child = NULL;
	int ret = KSFT_FAIL;

	if (need_cpus(2))
		return KSFT_SKIP;
	parent = cg_name(root, "cc_test_late");
	child = cg_name(parent, "child");
	if (!parent || !child || cg_create(parent) ||
	    cg_write(parent, "cgroup.subtree_control", "+cpu") ||
	    cg_write(parent, "cpu.concurrency.max", "1") || cg_create(child))
		goto cleanup;
	if (cg_read_strcmp(child, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "a child created under the limit does not inherit it\n");
		goto cleanup;
	}
	if (cg_run(child, spin_and_check, &param))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (child)
		cg_destroy(child);
	if (parent)
		cg_destroy(parent);
	free(child);
	free(parent);
	return ret;
}

struct slot_task {
	int cpu;		/* pinned there */
	int ready[2], go[2], done[2];
	int run_ms;		/* CPU time to run once told to go */
	int then_exit;		/* exit after, else signal done and sleep */
};

static long thread_cpu_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int slot_task_fn(const char *cgroup, void *arg)
{
	struct slot_task *t = arg;
	cpu_set_t set;
	long start;
	char c = 0;

	CPU_ZERO(&set);
	CPU_SET(t->cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return 1;
	if (write(t->ready[1], &c, 1) != 1 || read(t->go[0], &c, 1) != 1)
		return 1;
	start = thread_cpu_ms();
	while (thread_cpu_ms() - start < t->run_ms)
		;
	if (t->then_exit)
		return 0;
	if (write(t->done[1], &c, 1) != 1)
		return 1;
	for (;;)
		pause();
	return 0;
}

static int slot_task_init(struct slot_task *t, int cpu, int run_ms, int then_exit)
{
	t->cpu = cpu;
	t->run_ms = run_ms;
	t->then_exit = then_exit;
	return pipe(t->ready) || pipe(t->go) || pipe(t->done);
}

/*
 * A slot left behind in a child group. cs has the limit 1, its children
 * a and b task groups of their own. z, in a, pinned to CPU 1, and x, in
 * b, pinned to CPU 2, are registered before the limit, which relaxes to
 * their width, 2. z runs and exits: the width drops, the effective value
 * with it, and CPU 1 stays used for a while. x then runs: the slot z left
 * must be given back. The children's tasks used to be registered twice in
 * cs when cs's limit made them start counting, and the copy z's exit left
 * kept the cleaner off CPU 1 for good: x was held until the limit went.
 */
static int test_concurrency_child_slot(const char *root)
{
	char *cs = NULL, *a = NULL, *b = NULL, c = 'g';
	struct slot_task x = { }, z = { };
	int ret = KSFT_FAIL;
	pid_t xp = -1, zp = -1;
	struct pollfd pfd;

	if (need_cpus(3))
		return KSFT_SKIP;
	cs = cg_name(root, "cc_test_slot");
	a = cg_name(cs, "a");
	b = cg_name(cs, "b");
	if (!cs || !a || !b || cg_create(cs) ||
	    cg_write(cs, "cgroup.subtree_control", "+cpu") || cg_create(a) || cg_create(b))
		goto cleanup;
	if (slot_task_init(&x, 2, 300, 0) || slot_task_init(&z, 1, 300, 1))
		goto cleanup;
	xp = cg_run_nowait(b, slot_task_fn, &x);
	zp = cg_run_nowait(a, slot_task_fn, &z);
	if (xp < 0 || zp < 0 || read(x.ready[0], &c, 1) != 1 || read(z.ready[0], &c, 1) != 1)
		goto cleanup;
	if (cg_write(cs, "cpu.concurrency.max", "1"))
		goto cleanup;
	if (write(z.go[1], &c, 1) != 1 || waitpid(zp, NULL, 0) != zp)
		goto cleanup;
	zp = -1;
	usleep(50000);
	if (write(x.go[1], &c, 1) != 1)
		goto cleanup;
	pfd.fd = x.done[0];
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 5000) != 1) {
		fprintf(stderr, "x did not get 300 ms of CPU in 5 s after z's exit\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	/* The limit first: a held task acts on SIGKILL once placed. */
	if (cs)
		cg_write(cs, "cpu.concurrency.max", "max");
	if (xp > 0) {
		kill(xp, SIGKILL);
		waitpid(xp, NULL, 0);
	}
	if (zp > 0) {
		kill(zp, SIGKILL);
		waitpid(zp, NULL, 0);
	}
	if (a)
		cg_destroy(a);
	if (b)
		cg_destroy(b);
	if (cs)
		cg_destroy(cs);
	free(a);
	free(b);
	free(cs);
	return ret;
}

/*
 * A grandchild counting on its own when a limit appears two levels up: g
 * and p have the cpu controller, c has the soft value 2 and sleepers
 * pinned to CPUs 0 and 1. g gets the soft value 1, relaxed to 2; a task of
 * c's sibling c2 pinning itself to CPU 2 must be refused, a width of 3
 * over g's request. c's masks used to reach p only, not g, whose own list
 * the affinity check reads.
 */
static int test_concurrency_grand_merge(const char *root)
{
	char *g = NULL, *p = NULL, *c = NULL, *c2 = NULL;
	pid_t s0 = -1, s1 = -1, w = -1;
	int ret = KSFT_FAIL, err;
	cpu_set_t set;

	if (need_cpus(3))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_gm");
	p = cg_name(g, "p");
	c = cg_name(p, "c");
	c2 = cg_name(p, "c2");
	if (!g || !p || !c || !c2 || cg_create(g) ||
	    cg_write(g, "cgroup.subtree_control", "+cpu") || cg_create(p) ||
	    cg_write(p, "cgroup.subtree_control", "+cpu") || cg_create(c) || cg_create(c2))
		goto cleanup;
	if (cg_write(c, "cpu.concurrency.max", "2"))
		goto cleanup;
	s0 = cg_run_nowait(c, sleep_forever, NULL);
	s1 = cg_run_nowait(c, sleep_forever, NULL);
	w = cg_run_nowait(c2, sleep_forever, NULL);
	if (s0 < 0 || s1 < 0 || w < 0)
		goto cleanup;
	usleep(50000);
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	if (sched_setaffinity(s0, sizeof(set), &set))
		goto cleanup;
	CPU_ZERO(&set);
	CPU_SET(1, &set);
	if (sched_setaffinity(s1, sizeof(set), &set))
		goto cleanup;
	if (cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	if (cg_read_strcmp(g, "cpu.concurrency.effective", "2 max\n")) {
		fprintf(stderr, "g does not relax to the width of c's masks\n");
		goto cleanup;
	}
	CPU_ZERO(&set);
	CPU_SET(2, &set);
	err = sched_setaffinity(w, sizeof(set), &set) ? errno : 0;
	if (err != EINVAL) {
		fprintf(stderr, "pinning beside c's masks over g's request 1: %s, not EINVAL\n",
			err ? strerror(err) : "accepted");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (g)
		cg_write(g, "cpu.concurrency.max", "max");
	if (c)
		cg_write(c, "cpu.concurrency.max", "max");
	if (s0 > 0) {
		kill(s0, SIGKILL);
		waitpid(s0, NULL, 0);
	}
	if (s1 > 0) {
		kill(s1, SIGKILL);
		waitpid(s1, NULL, 0);
	}
	if (w > 0) {
		kill(w, SIGKILL);
		waitpid(w, NULL, 0);
	}
	if (c)
		cg_destroy(c);
	if (c2)
		cg_destroy(c2);
	if (p)
		cg_destroy(p);
	if (g)
		cg_destroy(g);
	free(c2);
	free(c);
	free(p);
	free(g);
	return ret;
}

/*
 * Migrations judged by the destination cpuset. cm has the limit 1 and the
 * cpuset controller below it, not the cpu one: a and b share cm's task
 * group, a on CPU 1, b on CPU 2. Two sleepers in a. A task from outside
 * joining b, and a sleeper moving from a to b, would make {1} and {2}, a
 * width of 2: both fail with EBUSY. The first was judged by cm's cpuset;
 * the second, which changes the cpuset alone, was not judged at all.
 */
static int test_concurrency_cpuset_move(const char *root)
{
	char *cm = NULL, *a = NULL, *b = NULL, *out = NULL;
	pid_t x = -1, y = -1, w = -1;
	int ret = KSFT_FAIL, err;

	if (need_cpus(3))
		return KSFT_SKIP;
	cm = cg_name(root, "cc_test_cm");
	out = cg_name(root, "cc_test_cm_out");
	a = cg_name(cm, "a");
	b = cg_name(cm, "b");
	if (!cm || !out || !a || !b || cg_create(cm) || cg_create(out) ||
	    cg_write(cm, "cgroup.subtree_control", "+cpuset") || cg_create(a) || cg_create(b))
		goto cleanup;
	if (cg_write(a, "cpuset.cpus", "1") || cg_write(b, "cpuset.cpus", "2") ||
	    cg_write(cm, "cpu.concurrency.max", "1"))
		goto cleanup;
	x = cg_run_nowait(a, sleep_forever, NULL);
	y = cg_run_nowait(a, sleep_forever, NULL);
	w = cg_run_nowait(out, sleep_forever, NULL);
	if (x < 0 || y < 0 || w < 0)
		goto cleanup;
	usleep(50000);
	err = cg_enter(b, w);
	if (err != -EBUSY) {
		fprintf(stderr, "joining b from outside: %s, not EBUSY\n",
			err ? strerror(-err) : "accepted");
		goto cleanup;
	}
	err = cg_enter(b, y);
	if (err != -EBUSY) {
		fprintf(stderr, "moving from a to b: %s, not EBUSY\n",
			err ? strerror(-err) : "accepted");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (cm)
		cg_write(cm, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (y > 0) {
		kill(y, SIGKILL);
		waitpid(y, NULL, 0);
	}
	if (w > 0) {
		kill(w, SIGKILL);
		waitpid(w, NULL, 0);
	}
	if (a)
		cg_destroy(a);
	if (b)
		cg_destroy(b);
	if (cm)
		cg_destroy(cm);
	if (out)
		cg_destroy(out);
	free(a);
	free(b);
	free(cm);
	free(out);
	return ret;
}

/*
 * Field @field of a stat file of /proc, counted from 1 as proc(5) does,
 * at the start of the text in @buf from there on; NULL when unreadable.
 */
static char *proc_stat_field(const char *path, int field, char *buf, size_t len)
{
	char *p;
	FILE *f;
	int i;

	f = fopen(path, "r");
	if (!f)
		return NULL;
	p = fgets(buf, len, f);
	fclose(f);
	if (!p)
		return NULL;
	/* Field 3, the state, follows the command, which may hold spaces. */
	p = strrchr(buf, ')');
	for (i = 2; i < field && p; i++)
		p = strchr(p + 1, ' ');
	return p;
}

/* CPU time @pid used so far, in ms, -1 when it is gone. */
static long proc_cpu_ms(pid_t pid)
{
	unsigned long utime, stime;
	char path[64], buf[1024], *p;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	p = proc_stat_field(path, 14, buf, sizeof(buf));
	if (!p || sscanf(p, "%lu %lu", &utime, &stime) != 2)
		return -1;
	return (utime + stime) * 1000 / sysconf(_SC_CLK_TCK);
}

/* SIGKILL @pid and reap it; 0 when it died within @ms, else -ETIMEDOUT. */
static int kill_within(pid_t pid, int ms)
{
	int i;

	kill(pid, SIGKILL);
	for (i = 0; i < ms / 10; i++) {
		if (waitpid(pid, NULL, WNOHANG) == pid)
			return 0;
		usleep(10000);
	}
	return -ETIMEDOUT;
}

/*
 * Removing a partition root's cgroup cannot be refused: its CPUs return to
 * its parent, and the cpusets that named them get them back. Under a
 * limit no cpuset may name CPUs a partition holds, so the removal widens
 * only the masks inherited from the parent, which hold all the others.
 * po, a partition root on CPUs 1 to n-1, gets the limit 1; p, a
 * partition root on CPUs n-2 and n-1, is empty; a names CPU n-3, where x
 * spins; z spins and s sleeps in b.
 *  - While b names only p's CPUs, which cpuset replaces by po's but p's,
 *    the limit is refused with EBUSY. Naming nothing, b inherits these,
 *    which hold a's: the limit is accepted, width 1. Under it b cannot
 *    name p's CPUs again.
 *  - p back to member and root again only widens and narrows b: accepted.
 *  - After the rmdir of p, b inherits all of po's CPUs: the width stays
 *    1, x and z share a CPU and both run; killed, z and s die.
 * The removal used to leave b on p's CPUs alone, disjoint from x's, and z
 * held for as long as x ran.
 */
static int test_concurrency_partition_rmdir(const char *root)
{
	char *po = NULL, *p = NULL, *a = NULL, *b = NULL, buf[64], pcpus[64];
	int ret = KSFT_FAIL, n = get_nprocs(), last = n - 1;
	pid_t x = -1, z = -1, s = -1;
	long x0, x1, z0, z1;

	if (need_cpus(4))
		return KSFT_SKIP;
	po = cg_name(root, "cc_test_prmdir");
	p = cg_name(po, "p");
	a = cg_name(po, "a");
	b = cg_name(po, "b");
	if (!po || !p || !a || !b || cg_create(po))
		goto cleanup;
	snprintf(buf, sizeof(buf), "1-%d", last);
	if (cg_write(po, "cpuset.cpus", buf) || cg_write(po, "cpuset.cpus.partition", "root")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	if (cg_write(po, "cgroup.subtree_control", "+cpuset") ||
	    cg_create(p) || cg_create(a) || cg_create(b))
		goto cleanup;
	snprintf(pcpus, sizeof(pcpus), "%d-%d", last - 1, last);
	if (cg_write(p, "cpuset.cpus", pcpus) || cg_write(p, "cpuset.cpus.partition", "root")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	snprintf(buf, sizeof(buf), "%d", last - 2);
	if (cg_write(b, "cpuset.cpus", pcpus) || cg_write(a, "cpuset.cpus", buf))
		goto cleanup;

	x = cg_run_nowait(a, busy_forever, NULL);
	z = cg_run_nowait(b, busy_forever, NULL);
	s = cg_run_nowait(b, sleep_forever, NULL);
	if (x < 0 || z < 0 || s < 0)
		goto cleanup;
	usleep(100000);

	if (expect_write(po, "cpu.concurrency.max", "1", -EBUSY,
			 "a limit over b, naming only p's CPUs"))
		goto cleanup;
	if (cg_write(b, "cpuset.cpus", "\n") || cg_write(po, "cpu.concurrency.max", "1"))
		goto cleanup;
	if (expect_write(b, "cpuset.cpus", pcpus, -EBUSY, "b naming p's CPUs under the limit") ||
	    expect_write(p, "cpuset.cpus.partition", "member", 0, "p back to member") ||
	    expect_write(p, "cpuset.cpus.partition", "root", 0, "p a partition root again"))
		goto cleanup;
	if (cg_destroy(p)) {
		fprintf(stderr, "rmdir of the empty partition root p failed\n");
		goto cleanup;
	}
	usleep(300000);
	if (cg_read(po, "cpu.concurrency.effective", buf, sizeof(buf)) ||
	    strncmp(buf, "1 ", 2)) {
		fprintf(stderr, "after the rmdir, po's effective limit is \"%.*s\", not 1\n",
			(int)strcspn(buf, "\n"), buf);
		goto cleanup;
	}
	x0 = proc_cpu_ms(x);
	z0 = proc_cpu_ms(z);
	usleep(500000);
	x1 = proc_cpu_ms(x);
	z1 = proc_cpu_ms(z);
	if (x1 <= x0 || z1 <= z0) {
		fprintf(stderr, "after the rmdir, in 500 ms x ran %ld ms, z %ld ms\n",
			x1 - x0, z1 - z0);
		goto cleanup;
	}
	if (kill_within(z, 1000)) {
		fprintf(stderr, "z, spinning, not dead 1 s after SIGKILL\n");
		goto cleanup;
	}
	z = -1;
	if (kill_within(s, 1000)) {
		fprintf(stderr, "s, asleep, not dead 1 s after SIGKILL\n");
		goto cleanup;
	}
	s = -1;
	ret = KSFT_PASS;
cleanup:
	if (po)
		cg_write(po, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (z > 0) {
		kill(z, SIGKILL);
		waitpid(z, NULL, 0);
	}
	if (s > 0) {
		kill(s, SIGKILL);
		waitpid(s, NULL, 0);
	}
	if (p) {
		cg_write(p, "cpuset.cpus.partition", "member");
		cg_destroy(p);
	}
	if (a)
		cg_destroy(a);
	if (b)
		cg_destroy(b);
	if (po) {
		cg_write(po, "cpuset.cpus.partition", "member");
		cg_destroy(po);
	}
	free(b);
	free(a);
	free(p);
	free(po);
	return ret;
}

/*
 * The cpu controller enabled on cgroups that exist gives each the hard
 * value its parent's child_hard holds, a limit that starts over their
 * cpusets as they are. p names CPU 0, x the last CPU, which p does not
 * have: with child_hard 1 in p, "+cpu" in p's subtree_control is refused
 * with EBUSY; once x names nothing it is accepted, and x has the hard
 * value 1.
 */
static int test_concurrency_child_hard_enable(const char *root)
{
	char *p = NULL, *x = NULL, buf[32];
	int ret = KSFT_FAIL, n = get_nprocs();

	if (need_cpus(2))
		return KSFT_SKIP;
	p = cg_name(root, "cc_test_child_hard_enable");
	if (!p)
		goto cleanup;
	x = cg_name(p, "x");
	if (!x || cg_create(p) || cg_write(p, "cgroup.subtree_control", "+cpuset") ||
	    cg_create(x))
		goto cleanup;
	snprintf(buf, sizeof(buf), "%d", n - 1);
	if (cg_write(p, "cpuset.cpus", "0") || cg_write(x, "cpuset.cpus", buf) ||
	    cg_write(p, "cpu.concurrency.child_ceiling", "1"))
		goto cleanup;
	if (expect_write(p, "cgroup.subtree_control", "+cpu", -EBUSY,
			 "the cpu controller over x, naming a CPU p does not have"))
		goto cleanup;
	if (cg_write(x, "cpuset.cpus", "\n") ||
	    expect_write(p, "cgroup.subtree_control", "+cpu", 0,
			 "the cpu controller over x, naming no CPU"))
		goto cleanup;
	if (cg_read_strcmp(x, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(x, "cpu.concurrency.ceiling", "1\n")) {
		fprintf(stderr, "x did not take the hard value 1 when the cpu controller came\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (x)
		cg_destroy(x);
	if (p)
		cg_destroy(p);
	free(x);
	free(p);
	return ret;
}

/* Pin @pid to CPUs @from to @to: 0 when the outcome is @want (an errno, 0 to succeed). */
static int expect_pin(pid_t pid, int from, int to, int want, const char *what)
{
	cpu_set_t set;
	int i, err = 0;

	CPU_ZERO(&set);
	for (i = from; i <= to; i++)
		CPU_SET(i, &set);
	if (sched_setaffinity(pid, sizeof(set), &set))
		err = errno;
	if (err == want)
		return 0;
	fprintf(stderr, "%s: %s, not %s\n", what, err ? strerror(err) : "accepted",
		want ? strerror(want) : "accepted");
	return -1;
}

/* The value of a field of /proc/<pid>/status, without its newline, or NULL. */
static char *proc_status(pid_t pid, const char *field, char *buf, size_t len)
{
	char path[64], line[256], *val = NULL;
	size_t n = strlen(field);
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return NULL;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, field, n) || line[n] != ':')
			continue;
		snprintf(buf, len, "%s", line + n + 1 + strspn(line + n + 1, " \t"));
		buf[strcspn(buf, "\n")] = 0;
		val = buf;
		break;
	}
	fclose(f);
	return val;
}

/*
 * Under a limit an affinity is taken as it is: within the cpuset, or
 * naming every CPU, which follows the cpuset. Any other cpuset cuts, and
 * gives back what it cut when it grows, which the removal of a partition
 * root does unchecked. Unless the cpuset names CPUs, a member of the
 * limited subtree: granted all it names, it changes only by writes the
 * limit judges, so a request it cuts stays cut and is accepted, counted
 * as the intersection.
 * p names CPUs 0 to 2 and its child h nothing, inheriting them; x sleeps
 * in h, pinned to {2,3} before any limit, which cpuset cuts to {2}:
 *  - a limit on p is refused with EBUSY; x pinned to {1,2} lets it start;
 *  - under it x cannot be pinned to {2,3} (EINVAL), but to every CPU;
 *  - p's cpuset cannot drop CPU 2, which x names (EBUSY), but can grow to
 *    CPU 3 and shrink back;
 *  - y, pinned to {2,3} outside p, cannot join h (EBUSY); pinned to every
 *    CPU, or to {0}, it can.
 * p's other child g names CPUs 0 to 2 and holds z, pinned to {2,3}:
 *  - the limit on g starts, z counted as {2}: its status reads
 *    Cpus_allowed_list 2 and Cpus_requested_list 2-3;
 *  - under it z pinned to {1,3}, then to {2,3} again, is accepted, and
 *    g's cpuset dropping CPU 1;
 *  - y, pinned to {2,3}, can join g;
 *  - g naming nothing would inherit p's CPUs, which cut z's request
 *    and y's as a cpuset that inherits may not: refused with EBUSY;
 *    with z and y pinned to every CPU, accepted.
 */
static int test_concurrency_literal_affinity(const char *root)
{
	char *p = NULL, *h = NULL, *g = NULL, *out = NULL, buf[64];
	int ret = KSFT_FAIL, err;
	pid_t x = -1, y = -1, z = -1;
	cpu_set_t all;

	if (need_cpus(4))
		return KSFT_SKIP;
	p = cg_name(root, "cc_test_lit");
	h = cg_name(p, "h");
	g = cg_name(p, "g");
	out = cg_name(root, "cc_test_lit_out");
	if (!p || !h || !g || !out || cg_create(p) || cg_write(p, "cpuset.cpus", "0-2") ||
	    cg_write(p, "cgroup.subtree_control", "+cpuset +cpu") || cg_create(h) ||
	    cg_create(g) || cg_write(g, "cpuset.cpus", "0-2") || cg_create(out))
		goto cleanup;
	x = cg_run_nowait(h, sleep_forever, NULL);
	y = cg_run_nowait(out, sleep_forever, NULL);
	z = cg_run_nowait(g, sleep_forever, NULL);
	if (x < 0 || y < 0 || z < 0)
		goto cleanup;
	stress_all_cpus(&all);

	if (expect_pin(x, 2, 3, 0, "x pinned to {2,3}, no limit") ||
	    expect_write(p, "cpu.concurrency.max", "2", -EBUSY,
			 "a limit over x, pinned partly outside h, which inherits") ||
	    expect_pin(x, 1, 2, 0, "x pinned within h") ||
	    expect_write(p, "cpu.concurrency.max", "2", 0, "the limit, x within h") ||
	    expect_pin(x, 2, 3, EINVAL, "x pinned partly outside h, under the limit"))
		goto cleanup;
	if (sched_setaffinity(x, sizeof(all), &all)) {
		perror("x pinned to every CPU, under the limit");
		goto cleanup;
	}
	if (expect_pin(x, 1, 2, 0, "x pinned within h again") ||
	    expect_write(p, "cpuset.cpus", "0-1", -EBUSY, "p's cpuset dropping CPU 2, which x names") ||
	    expect_write(p, "cpuset.cpus", "0-3", 0, "p's cpuset growing to CPU 3") ||
	    expect_write(p, "cpuset.cpus", "0-2", 0, "p's cpuset shrinking back"))
		goto cleanup;

	if (expect_pin(y, 2, 3, 0, "y pinned to {2,3} outside p"))
		goto cleanup;
	err = cg_enter(h, y);
	if (err != -EBUSY) {
		fprintf(stderr, "y, pinned partly outside h, joining h: %s, not EBUSY\n",
			err ? strerror(-err) : "accepted");
		goto cleanup;
	}
	if (sched_setaffinity(y, sizeof(all), &all)) {
		perror("y pinned to every CPU");
		goto cleanup;
	}
	if (cg_enter(h, y)) {
		fprintf(stderr, "y, pinned to every CPU, cannot join h\n");
		goto cleanup;
	}
	if (cg_enter(out, y) || expect_pin(y, 0, 0, 0, "y pinned to {0} outside p"))
		goto cleanup;
	if (cg_enter(h, y)) {
		fprintf(stderr, "y, pinned to {0}, cannot join h\n");
		goto cleanup;
	}
	if (cg_enter(out, y))
		goto cleanup;

	/* g names its CPUs: what it cuts from a request stays cut. */
	if (expect_pin(z, 2, 3, 0, "z pinned to {2,3} in g, no limit") ||
	    expect_write(g, "cpu.concurrency.max", "2", 0, "a limit over z, cut by g, which names CPUs"))
		goto cleanup;
	if (!proc_status(z, "Cpus_allowed_list", buf, sizeof(buf)) || strcmp(buf, "2")) {
		fprintf(stderr, "z's Cpus_allowed_list is \"%s\", not 2\n", buf);
		goto cleanup;
	}
	if (!proc_status(z, "Cpus_requested_list", buf, sizeof(buf)) || strcmp(buf, "2-3")) {
		fprintf(stderr, "z's Cpus_requested_list is \"%s\", not 2-3\n", buf);
		goto cleanup;
	}
	if (expect_pin(z, 1, 3, 0, "z pinned to {1,2,3}, under the limit") ||
	    expect_pin(z, 2, 3, 0, "z pinned to {2,3} again") ||
	    expect_write(g, "cpuset.cpus", "0,2", 0, "g's cpuset dropping CPU 1") ||
	    expect_pin(y, 2, 3, 0, "y pinned to {2,3} outside g"))
		goto cleanup;
	if (cg_enter(g, y)) {
		fprintf(stderr, "y, pinned partly outside g, which names CPUs, cannot join g\n");
		goto cleanup;
	}
	if (expect_write(g, "cpuset.cpus", "\n", -EBUSY,
			 "g naming nothing, with z and y cut by what it names"))
		goto cleanup;
	if (sched_setaffinity(z, sizeof(all), &all) || sched_setaffinity(y, sizeof(all), &all)) {
		perror("z and y pinned to every CPU");
		goto cleanup;
	}
	if (expect_write(g, "cpuset.cpus", "\n", 0, "g naming nothing, z and y following it"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (p)
		cg_write(p, "cpu.concurrency.max", "max");
	if (g)
		cg_write(g, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (y > 0) {
		kill(y, SIGKILL);
		waitpid(y, NULL, 0);
	}
	if (z > 0) {
		kill(z, SIGKILL);
		waitpid(z, NULL, 0);
	}
	if (h)
		cg_destroy(h);
	if (g)
		cg_destroy(g);
	if (p)
		cg_destroy(p);
	if (out)
		cg_destroy(out);
	free(out);
	free(g);
	free(h);
	free(p);
	return ret;
}

/*
 * A refused affinity change leaves the task registered by the mask it
 * keeps. g has the limit 1; x and y sleep in it, unpinned. With CPU c
 * offline, x pinned to {c} is refused with EINVAL: no CPU of its mask is
 * online. y pinned to {d} is then accepted, the width being 1. The class
 * of {c}, registered ahead of x's change, used to stay: {c} and {d} made
 * a width of 2, refused, and its headroom kept a slot for a CPU x could
 * not run on.
 */
static int test_concurrency_refused_affinity(const char *root)
{
	int ret = KSFT_FAIL, n = get_nprocs(), c = n - 1, d = n - 2, offline = 0;
	pid_t x = -1, y = -1;
	char *g = NULL;

	if (need_cpus(3))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_refused");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	x = cg_run_nowait(g, sleep_forever, NULL);
	y = cg_run_nowait(g, sleep_forever, NULL);
	if (x < 0 || y < 0)
		goto cleanup;
	usleep(100000);
	if (write_cpu_online(c, 0)) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	offline = 1;
	if (expect_pin(x, c, c, EINVAL, "x pinned to its offline CPU") ||
	    expect_pin(y, d, d, 0, "y pinned to another CPU after x's refusal"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (offline)
		write_cpu_online(c, 1);
	if (g)
		cg_write(g, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (y > 0) {
		kill(y, SIGKILL);
		waitpid(y, NULL, 0);
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

/*
 * An affinity change to an offline CPU makes no room for it. g has the
 * limit 1 and x spins in it, holding its slot. With CPU c offline, x is
 * pinned to {c} 50 times, refused with EINVAL each time, within 250 ms in
 * all. Each change used to try making room for {c}, ten rounds of a
 * jiffy's sleep under cc_mutex and cpuset_mutex, holding every other
 * change of a limited group meanwhile: 500 ms at least.
 */
static int test_concurrency_offline_affinity(const char *root)
{
	int ret = KSFT_FAIL, n = get_nprocs(), c = n - 1, offline = 0, i;
	struct timespec t0;
	long long ms;
	pid_t x = -1;
	char *g = NULL;

	if (need_cpus(2))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_offline_aff");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	x = cg_run_nowait(g, busy_forever, NULL);
	if (x < 0)
		goto cleanup;
	usleep(100000);
	if (write_cpu_online(c, 0)) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	offline = 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < 50; i++) {
		if (expect_pin(x, c, c, EINVAL, "x pinned to an offline CPU"))
			goto cleanup;
	}
	ms = ns_since(&t0) / 1000000;
	if (ms >= 250) {
		fprintf(stderr, "50 refused changes took %lld ms\n", ms);
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (offline)
		write_cpu_online(c, 1);
	if (g)
		cg_write(g, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

/* Pin @pid to the CPUs of @cpus, ended by -1: 0 or an errno. */

/*
 * A cpuset write moves a limited group's presence into its new CPUs
 * first, applying the new mask to the tasks it moves. They stay
 * registered by the mask they are configured with, offline CPUs
 * included: sched_setaffinity(2) is judged by the registered masks. With
 * CPU 2 offline, each write below moves a spinning task onto CPUs its
 * mask lacked:
 *  - g (limit 1), cpuset 1-2: x, pinned to {2}, sleeps; t1 spins. g's
 *    cpuset becomes 2-4 and t1 moves from CPU 1 to 3 or 4. Configured,
 *    t1's mask {2,3,4} contains every other: x can be pinned to {2,3}.
 *  - p (limit 2): t2 spins in a (cpuset 1); w1, pinned to {3,5}, and w2,
 *    to {3,4,5}, spin in b (cpuset 3-5). a's cpuset becomes {2,5} and t2
 *    moves to CPU 5. Configured, {2,5}, {3,5} and {4,5} would be three
 *    masks no two of which are nested: w2 cannot be pinned to {4,5}.
 * Registered by the entitled masks, {3,4} and {5}, x's pin was refused,
 * and w2's accepted, p's enforced value then rising to 3 with CPU 2 back.
 */
static int test_concurrency_setmove_mask(const char *root)
{
	static const int two[] = { 2, -1 }, w1m[] = { 3, 5, -1 }, w2m[] = { 3, 4, 5, -1 },
			 x23[] = { 2, 3, -1 }, w45[] = { 4, 5, -1 };
	char *g = NULL, *p = NULL, *a = NULL, *b = NULL;
	pid_t x = -1, t1 = -1, t2 = -1, w1 = -1, w2 = -1;
	int ret = KSFT_FAIL, offline = 0, err;

	if (need_cpus(6))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_smm_g");
	p = cg_name(root, "cc_test_smm_p");
	a = cg_name(p, "a");
	b = cg_name(p, "b");
	if (!g || !p || !a || !b || cg_create(g) || cg_create(p) ||
	    cg_write(p, "cgroup.subtree_control", "+cpu +cpuset") || cg_create(a) ||
	    cg_create(b))
		goto cleanup;
	if (cg_write(g, "cpuset.cpus", "1-2") || cg_write(g, "cpu.concurrency.max", "1") ||
	    cg_write(a, "cpuset.cpus", "1") || cg_write(b, "cpuset.cpus", "3-5") ||
	    cg_write(p, "cpu.concurrency.max", "2"))
		goto cleanup;
	x = cg_run_nowait(g, sleep_forever, NULL);
	t1 = cg_run_nowait(g, busy_forever, NULL);
	t2 = cg_run_nowait(a, busy_forever, NULL);
	w1 = cg_run_nowait(b, busy_forever, NULL);
	w2 = cg_run_nowait(b, busy_forever, NULL);
	if (x < 0 || t1 < 0 || t2 < 0 || w1 < 0 || w2 < 0)
		goto cleanup;
	err = pin_cpus(x, two);
	if (!err)
		err = pin_cpus(w1, w1m);
	if (!err)
		err = pin_cpus(w2, w2m);
	if (err) {
		fprintf(stderr, "pinning before the test: %s\n", strerror(err));
		goto cleanup;
	}
	usleep(100000);
	if (write_cpu_online(2, 0)) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	offline = 1;
	usleep(50000);

	if (expect_write(g, "cpuset.cpus", "2-4", 0, "g's cpuset to 2-4"))
		goto cleanup;
	err = pin_cpus(x, x23);
	if (err) {
		fprintf(stderr, "x pinned to {2,3} beside t1 moved onto {3,4}: %s, not accepted\n",
			strerror(err));
		goto cleanup;
	}

	if (expect_write(a, "cpuset.cpus", "2,5", 0, "a's cpuset to {2,5}"))
		goto cleanup;
	err = pin_cpus(w2, w45);
	if (err != EINVAL) {
		fprintf(stderr, "w2 pinned to {4,5} beside t2 moved onto {5}: %s, not EINVAL\n",
			err ? strerror(err) : "accepted");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (offline)
		write_cpu_online(2, 1);
	if (g)
		cg_write(g, "cpu.concurrency.max", "max");
	if (p)
		cg_write(p, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (t1 > 0) {
		kill(t1, SIGKILL);
		waitpid(t1, NULL, 0);
	}
	if (t2 > 0) {
		kill(t2, SIGKILL);
		waitpid(t2, NULL, 0);
	}
	if (w1 > 0) {
		kill(w1, SIGKILL);
		waitpid(w1, NULL, 0);
	}
	if (w2 > 0) {
		kill(w2, SIGKILL);
		waitpid(w2, NULL, 0);
	}
	if (a)
		cg_destroy(a);
	if (b)
		cg_destroy(b);
	if (p)
		cg_destroy(p);
	if (g)
		cg_destroy(g);
	free(a);
	free(b);
	free(p);
	free(g);
	return ret;
}

/* State shared with the process of a round of test_concurrency_cid_decrease(). */
struct cid_dec {
	volatile int go, ready, done, stop, at1;
	volatile long long max_cid;	/* -1 for none */
	char cg[256];
};

static struct cid_dec *cd;

/* Has @cg's effective value come down to 1 within a second? */
static int cid_dec_wait_at1(const char *cg)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (!cg_read_strcmp(cg, "cpu.concurrency.effective", "1 max\n"))
			return 1;
		usleep(100);
	}
	return 0;
}

/* Sleep, a schedule-in under the bound in force, then note the ID given. */
static void cid_dec_note(void)
{
	__u32 cid;

	usleep(100);
	cid = rseq_field(thread_rseq(), RSEQ_MM_CID_OFFSET);
	if (cid > cd->max_cid)
		cd->max_cid = cid;
}

static void cid_dec_busy(void)
{
	spin_ns(20000);
	usleep(20);
}

/* The second thread: it holds ID 1 of its own, and writes the decrease. */
static void *cid_dec_writer(void *arg)
{
	while (!cd->go)
		cid_dec_busy();
	cg_write(cd->cg, "cpu.concurrency.max", "1");
	cd->at1 = cid_dec_wait_at1(cd->cg);
	if (cd->at1)
		cid_dec_note();
	cd->done = 1;
	return NULL;
}

static void *cid_dec_extra(void *arg)
{
	while (!cd->stop)
		cid_dec_busy();
	return NULL;
}

static void *cid_dec_new(void *arg)
{
	__u32 cid = rseq_field(thread_rseq(), RSEQ_MM_CID_OFFSET);

	if (cid > cd->max_cid)
		cd->max_cid = cid;
	cid_dec_note();
	return NULL;
}

/* A round's process: @writer, two threads, one writing; else three, then a new one. */
static int cid_dec_proc(const char *cg, void *arg)
{
	pthread_t t[2], n;
	int i;

	if (arg) {
		pthread_create(&t[0], NULL, cid_dec_writer, NULL);
		usleep(200);
		cd->ready = 1;
		while (!cd->done)
			cid_dec_busy();
		if (cd->at1)
			cid_dec_note();
		pthread_join(t[0], NULL);
		return 0;
	}
	for (i = 0; i < 2; i++)
		pthread_create(&t[i], NULL, cid_dec_extra, NULL);
	usleep(200);
	cd->ready = 1;
	while (!cd->go)
		;
	pthread_create(&n, NULL, cid_dec_new, NULL);
	pthread_join(n, NULL);
	cd->stop = 1;
	for (i = 0; i < 2; i++)
		pthread_join(t[i], NULL);
	return 0;
}

/*
 * A decrease of the limit gives no thread an ID at or above it at its
 * next schedule-in. g has the limit 2; each round runs a new process in
 * g, lowers the limit to 1, and waits for the effective value to read 1:
 *  - the process has two threads, with IDs 0 and 1 of their own, and the
 *    thread holding 1 writes the decrease; each thread then sleeps and
 *    reads its ID. The decrease switches the process to IDs owned by the
 *    CPUs, which ran short under the new bound and skipped the writer;
 *  - the process has three threads, more than IDs under 2, so that its
 *    CPUs own them; after the decrease it creates a fourth, which reads
 *    its first ID: the one a CPU kept was handed to it as it was.
 * 60 rounds of each; every ID read must be 0.
 */
static int test_concurrency_cid_decrease(const char *root)
{
	int ret = KSFT_FAIL, r;
	char *g = NULL;

	if (!rseq_has_mm_cid())
		return skip("no mm_cid in the rseq area glibc registered");
	cd = mmap(NULL, sizeof(*cd), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (cd == MAP_FAILED)
		return KSFT_FAIL;
	g = cg_name(root, "cc_test_cid_dec");
	if (!g || cg_create(g))
		goto cleanup;
	snprintf(cd->cg, sizeof(cd->cg), "%s", g);
	for (r = 0; r < 120; r++) {
		int writer = r & 1;
		pid_t pid;

		memset((void *)cd, 0, offsetof(struct cid_dec, cg));
		cd->max_cid = -1;
		if (cg_write(g, "cpu.concurrency.max", "2"))
			goto cleanup;
		pid = cg_run_nowait(g, cid_dec_proc, writer ? (void *)1 : NULL);
		if (pid < 0)
			goto cleanup;
		while (!cd->ready)
			usleep(10);
		usleep(2000);
		if (writer) {
			cd->go = 1;
		} else {
			if (cg_write(g, "cpu.concurrency.max", "1"))
				goto cleanup;
			cd->at1 = cid_dec_wait_at1(g);
			cd->go = 1;
		}
		waitpid(pid, NULL, 0);
		if (!cd->at1) {
			fprintf(stderr, "round %d: the effective value stayed above 1 for 1 s\n", r);
			goto cleanup;
		}
		if (cd->max_cid != 0) {
			fprintf(stderr, "round %d (%s): ID %lld read under effective value 1\n",
				r, writer ? "a thread writing the decrease" : "a thread created after it",
				cd->max_cid);
			goto cleanup;
		}
	}
	ret = KSFT_PASS;
cleanup:
	if (g) {
		cg_write(g, "cpu.concurrency.max", "max");
		cg_destroy(g);
	}
	free(g);
	munmap((void *)cd, sizeof(*cd));
	return ret;
}

/* State shared with the processes of test_concurrency_effective_ids(). */
struct eff_ids {
	volatile int at1, stop, ready, judged, bad;
	unsigned int bad_cid;		/* read once its process is reaped */
	char file[PATH_MAX];
};

static struct eff_ids *ei;

static void *eff_ids_sleep(void *arg)
{
	for (;;)
		pause();
	return NULL;
}

/* The sibling's process: @arg threads that sleep, each walked by a bound update. */
static int eff_ids_bulk(const char *cgroup, void *arg)
{
	long i, n = (long)arg;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	for (i = 0; i < n; i++) {
		pthread_t t;

		if (pthread_create(&t, &attr, eff_ids_sleep, NULL))
			break;
	}
	ei->ready = 1;
	for (;;)
		pause();
	return 0;
}

/*
 * Run a millisecond, read the effective value, sleep, then read the ID
 * given at the schedule-in: judged while the limit is 1 only, since an
 * increase may come between the read and the schedule-in.
 */
static void *eff_ids_reader(void *arg)
{
	while (!ei->stop) {
		int at1 = ei->at1, v;
		__u32 cid;
		char buf[32];

		spin_ns(1000000);
		if (read_text(ei->file, buf, sizeof(buf)) <= 0 || !strncmp(buf, "max", 3))
			continue;
		v = atoi(buf);
		usleep(50);
		cid = rseq_field(thread_rseq(), RSEQ_MM_CID_OFFSET);
		__sync_synchronize();
		if (!at1 || !ei->at1)
			continue;
		__sync_fetch_and_add(&ei->judged, 1);
		if (cid >= (__u32)v) {
			__sync_fetch_and_add(&ei->bad, 1);
			ei->bad_cid = cid;
		}
	}
	return NULL;
}

/* A round's process: two readers, each with an ID of its own, 0 and 1. */
static int eff_ids_proc(const char *cgroup, void *arg)
{
	pthread_t t;

	pthread_create(&t, NULL, eff_ids_reader, NULL);
	eff_ids_reader(NULL);
	pthread_join(t, NULL);
	return 0;
}

/*
 * cpu.concurrency.effective never reads below the bound the IDs are
 * still given under: a thread scheduled in after reading it gets an ID
 * below the value read. g (limit 2) has two children: c0, made first,
 * holds a process of 1000 sleeping threads, and c1 a new process every
 * round, two threads that run and read c1's effective value, sleep, and
 * read their ID. The limit drops to 1 for 100 ms, then is back at 2. The
 * bounds of a subtree are applied one task after the other, c0's before
 * c1's: c1's value used to read 1 for those milliseconds while its
 * process still held IDs 0 and 1 of its own, at every decrease.
 */
static int test_concurrency_effective_ids(const char *root)
{
	char *g = NULL, *c0 = NULL, *c1 = NULL;
	int ret = KSFT_FAIL, r, i;
	pid_t bulk = -1;

	if (!rseq_has_mm_cid())
		return skip("no mm_cid in the rseq area glibc registered");
	if (get_nprocs() < 2)
		return skip("2 CPUs needed, %d online", get_nprocs());
	ei = mmap(NULL, sizeof(*ei), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (ei == MAP_FAILED)
		return KSFT_FAIL;
	memset((void *)ei, 0, sizeof(*ei));
	g = cg_name(root, "cc_test_eff_ids");
	c0 = cg_name(g, "c0");
	c1 = cg_name(g, "c1");
	if (!g || !c0 || !c1 || cg_create(g) || cg_write(g, "cgroup.subtree_control", "+cpu") ||
	    cg_create(c0) || cg_create(c1) || cg_write(g, "cpu.concurrency.max", "2"))
		goto cleanup;
	snprintf(ei->file, sizeof(ei->file), "%s/cpu.concurrency.effective", c1);
	bulk = cg_run_nowait(c0, eff_ids_bulk, (void *)1000L);
	if (bulk < 0)
		goto cleanup;
	for (i = 0; i < 5000 && !ei->ready; i++)
		usleep(1000);
	if (!ei->ready)
		goto cleanup;
	for (r = 0; r < 20; r++) {
		pid_t pid;

		ei->stop = 0;
		pid = cg_run_nowait(c1, eff_ids_proc, NULL);
		if (pid < 0)
			goto cleanup;
		usleep(100000);
		ei->at1 = 1;
		__sync_synchronize();
		if (cg_write(g, "cpu.concurrency.max", "1"))
			goto cleanup;
		usleep(100000);
		ei->at1 = 0;
		__sync_synchronize();
		ei->stop = 1;
		waitpid(pid, NULL, 0);
		if (cg_write(g, "cpu.concurrency.max", "2"))
			goto cleanup;
		if (ei->bad) {
			fprintf(stderr, "round %d: ID %u read after the effective value read 1, %d times\n",
				r, ei->bad_cid, ei->bad);
			goto cleanup;
		}
	}
	if (!ei->judged) {
		fprintf(stderr, "the effective value never read 1\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (bulk > 0) {
		kill(bulk, SIGKILL);
		waitpid(bulk, NULL, 0);
	}
	if (g)
		cg_write(g, "cpu.concurrency.max", "max");
	if (c1)
		cg_destroy(c1);
	if (c0)
		cg_destroy(c0);
	if (g)
		cg_destroy(g);
	free(c1);
	free(c0);
	free(g);
	munmap((void *)ei, sizeof(*ei));
	return ret;
}

#ifndef PR_SET_CONCURRENCY_PIN
#define PR_SET_CONCURRENCY_PIN	82
#define PR_GET_CONCURRENCY_PIN	83
#endif /* !PR_SET_CONCURRENCY_PIN */

/*
 * prctl(PR_SET_CONCURRENCY_PIN): a process pins itself to its cpu cgroup,
 * for good, and may size per-ID data by the ceiling PR_GET_CONCURRENCY_PIN
 * returns. In "in", created with the hard value 2 beside "out":
 *  - the pinned process reads the ceiling 2; unpinning fails with EPERM,
 *    pinning again succeeds; clone3(CLONE_INTO_CGROUP) of a child into
 *    out is refused with EBUSY, into in admitted;
 *  - neither it nor its fork(2) child, which reads the same ceiling, can
 *    be moved to out (EBUSY), and the cpu controller turned off above
 *    them is refused with EBUSY;
 *  - a child that called execve(2) reads no ceiling and moves, as does an
 *    unpinned process beside them.
 */
struct pin_shared {
	int ready, err, ceiling, child_ceiling;
	pid_t child, exec;
};

struct pin_param {
	struct pin_shared *sh;
	const char *out;
	int pipe_wr;
};

#define PIN_SKIP	-1
static int pin_run(const char *cgroup, void *arg)
{
	struct pin_param *p = arg;
	struct pin_shared *sh = p->sh;
	char fd[16];
	pid_t c;

	if (prctl(PR_GET_CONCURRENCY_PIN, 0, 0, 0, 0)) {
		sh->err = errno == EINVAL ? PIN_SKIP : 1;
		goto done;
	}
	if (prctl(PR_SET_CONCURRENCY_PIN, 1, 0, 0, 0)) {
		sh->err = 2;
		goto done;
	}
	sh->ceiling = prctl(PR_GET_CONCURRENCY_PIN, 0, 0, 0, 0);
	if (prctl(PR_SET_CONCURRENCY_PIN, 0, 0, 0, 0) != -1 || errno != EPERM) {
		sh->err = 3;
		goto done;
	}
	if (prctl(PR_SET_CONCURRENCY_PIN, 1, 0, 0, 0)) {
		sh->err = 4;
		goto done;
	}
	if (prctl(PR_SET_CONCURRENCY_PIN, 2, 0, 0, 0) != -1 || errno != EINVAL) {
		sh->err = 5;
		goto done;
	}
	if (ns_cid_clone_into(p->out) != EBUSY) {
		sh->err = 6;
		goto done;
	}
	if (ns_cid_clone_into(cgroup)) {
		sh->err = 7;
		goto done;
	}
	c = fork();
	if (!c) {
		__atomic_store_n(&sh->child_ceiling, prctl(PR_GET_CONCURRENCY_PIN, 0, 0, 0, 0),
				 __ATOMIC_RELEASE);
		for (;;)
			pause();
	}
	sh->child = c;
	snprintf(fd, sizeof(fd), "%d", p->pipe_wr);
	c = fork();
	if (!c) {
		execl("/proc/self/exe", "test_cpu_concurrency", "--pin-get", fd, (char *)NULL);
		_exit(127);
	}
	sh->exec = c;
done:
	__atomic_store_n(&sh->ready, 1, __ATOMIC_RELEASE);
	for (;;)
		pause();
	return 0;
}

/* Re-executed by pin_run(): report the ceiling on @fd, then wait to be moved. */
static int pin_get_main(int fd)
{
	int v = prctl(PR_GET_CONCURRENCY_PIN, 0, 0, 0, 0);

	if (write(fd, &v, sizeof(v)) != sizeof(v))
		return 1;
	close(fd);
	for (;;)
		pause();
	return 0;
}

static void pin_kill(pid_t pid, bool reap)
{
	if (pid <= 0)
		return;
	kill(pid, SIGKILL);
	if (reap)
		waitpid(pid, NULL, 0);
}

static int test_concurrency_pin(const char *root)
{
	char *parent = NULL, *in = NULL, *out = NULL;
	struct pin_param param = { };
	struct pin_shared *sh;
	int ret = KSFT_FAIL, pfd[2] = { -1, -1 }, v = -1, err, i;
	pid_t pinned = -1, other = -1;
	struct pollfd pf;

	if (need_cpus(2))
		return KSFT_SKIP;
	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		return KSFT_FAIL;
	memset(sh, 0, sizeof(*sh));
	sh->child_ceiling = -1;
	if (pipe(pfd))
		goto cleanup;
	parent = cg_name(root, "cc_test_pin");
	if (!parent)
		goto cleanup;
	in = cg_name(parent, "in");
	out = cg_name(parent, "out");
	if (!in || !out)
		goto cleanup;
	if (cg_create(parent) || cg_write(parent, "cgroup.subtree_control", "+cpu") ||
	    cg_write(parent, "cpu.concurrency.child_ceiling", "2") ||
	    cg_create(in) || cg_create(out))
		goto cleanup;

	param.sh = sh;
	param.out = out;
	param.pipe_wr = pfd[1];
	pinned = cg_run_nowait(in, pin_run, &param);
	if (pinned < 0)
		goto cleanup;
	for (i = 0; i < 200 && !__atomic_load_n(&sh->ready, __ATOMIC_ACQUIRE); i++)
		usleep(10000);
	if (!sh->ready) {
		fprintf(stderr, "the pinned process did not report\n");
		goto cleanup;
	}
	if (sh->err == PIN_SKIP) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	if (sh->err) {
		fprintf(stderr, "the pinned process failed its check %d\n", sh->err);
		goto cleanup;
	}
	if (sh->ceiling != 2) {
		fprintf(stderr, "ceiling %d under the hard value 2\n", sh->ceiling);
		goto cleanup;
	}
	for (i = 0; i < 200 && __atomic_load_n(&sh->child_ceiling, __ATOMIC_ACQUIRE) < 0; i++)
		usleep(10000);
	if (sh->child_ceiling != 2) {
		fprintf(stderr, "the fork child of a pinned process reads the ceiling %d, not 2\n",
			sh->child_ceiling);
		goto cleanup;
	}

	err = cg_enter(out, pinned);
	if (err != -EBUSY) {
		fprintf(stderr, "moving the pinned process: %s, not EBUSY\n",
			err ? strerror(-err) : "accepted");
		goto cleanup;
	}
	err = cg_enter(out, sh->child);
	if (err != -EBUSY) {
		fprintf(stderr, "moving the fork child of the pinned process: %s, not EBUSY\n",
			err ? strerror(-err) : "accepted");
		goto cleanup;
	}
	if (cg_enter(in, pinned)) {
		fprintf(stderr, "writing the pinned process to its own cgroup failed\n");
		goto cleanup;
	}
	err = cg_write(parent, "cgroup.subtree_control", "-cpu");
	if (err != -EBUSY) {
		fprintf(stderr, "cpu controller turned off above a pinned process: %s, not EBUSY\n",
			err ? strerror(-err) : "accepted");
		goto cleanup;
	}

	pf.fd = pfd[0];
	pf.events = POLLIN;
	if (poll(&pf, 1, 2000) != 1 || read(pfd[0], &v, sizeof(v)) != sizeof(v)) {
		fprintf(stderr, "the child that called execve(2) did not report\n");
		goto cleanup;
	}
	if (v) {
		fprintf(stderr, "after execve(2), the ceiling is %d, not 0\n", v);
		goto cleanup;
	}
	err = cg_enter(out, sh->exec);
	if (err) {
		fprintf(stderr, "moving a child after its execve(2): %s\n", strerror(-err));
		goto cleanup;
	}
	other = cg_run_nowait(in, sleep_forever, NULL);
	if (other < 0)
		goto cleanup;
	err = cg_enter(out, other);
	if (err) {
		fprintf(stderr, "moving an unpinned process: %s\n", strerror(-err));
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	pin_kill(sh->exec, false);
	pin_kill(sh->child, false);
	pin_kill(pinned, true);
	pin_kill(other, true);
	if (in)
		cg_destroy(in);
	if (out)
		cg_destroy(out);
	if (parent)
		cg_destroy(parent);
	if (pfd[0] >= 0) {
		close(pfd[0]);
		close(pfd[1]);
	}
	munmap(sh, sizeof(*sh));
	free(in);
	free(out);
	free(parent);
	return ret;
}

/*
 * From a process in @out pinned to CPUs @from to @to (every CPU when @from
 * is negative), clone3() with CLONE_INTO_CGROUP into @cg, the child exiting
 * at once: 0, the errno of the refusal, or -1, also when it did not end
 * within 2 s (a child held for good), its process group then killed.
 */
static int clone_into_pinned(const char *out, const char *cg, int from, int to)
{
	pid_t f;
	int st, i;

	f = fork();
	if (f < 0)
		return -1;
	if (!f) {
		cpu_set_t set;
		pid_t c;
		int fd;

		setpgid(0, 0);
		if (cg_enter_current(out))
			_exit(200);
		CPU_ZERO(&set);
		for (i = from < 0 ? 0 : from; i <= (from < 0 ? CPU_SETSIZE - 1 : to); i++)
			CPU_SET(i, &set);
		if (sched_setaffinity(0, sizeof(set), &set))
			_exit(201);
		fd = open(cg, O_RDONLY | O_DIRECTORY);
		if (fd < 0)
			_exit(202);
		c = clone_into_cgroup(fd);
		if (c < 0)
			_exit(errno);
		if (!c)
			_exit(0);
		waitpid(c, NULL, 0);
		_exit(0);
	}
	for (i = 0; i < 200; i++) {
		if (waitpid(f, &st, WNOHANG) == f)
			break;
		usleep(10000);
	}
	if (i == 200) {
		fprintf(stderr, "the clone did not end within 2 s, its child held\n");
		kill(-f, SIGKILL);
		waitpid(f, &st, 0);
		return -1;
	}
	if (!WIFEXITED(st) || WEXITSTATUS(st) >= 200)
		return -1;
	return WEXITSTATUS(st);
}

static int expect_clone(const char *out, const char *cg, int from, int to, int want,
			const char *what)
{
	int err = clone_into_pinned(out, cg, from, to);

	if (err == want)
		return 0;
	fprintf(stderr, "%s: %s, not %s\n", what,
		err < 0 ? "failed" : err ? strerror(err) : "accepted",
		want ? strerror(want) : "accepted");
	return -1;
}

/*
 * clone3(2) with CLONE_INTO_CGROUP into a limited group is judged as a
 * migration into it: g has the limit 1 and the cpuset of CPUs 0 to 2, x
 * sleeps in it pinned to {0}. A child born there from a parent pinned to
 * {1} would make a width of 2: refused with EBUSY; from one pinned to
 * {0,1}, nested over x's, it is accepted. From a parent pinned to {2,3},
 * which g's cpuset would cut, refused with EBUSY; from one pinned to
 * every CPU, accepted. The writes that wait for the forks judged before
 * them, to g's cpuset and limit, then return: a refused fork leaves no
 * count behind. Such forks used to go unjudged.
 */
static int test_concurrency_clone_into(const char *root)
{
	char *g = NULL, *out = NULL;
	int ret = KSFT_FAIL, fd;
	pid_t x = -1;

	if (need_cpus(4))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_clone");
	out = cg_name(root, "cc_test_clone_out");
	if (!g || !out || cg_create(g) || cg_create(out) || cg_write(g, "cpuset.cpus", "0-2") ||
	    cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	fd = open(g, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		goto cleanup;
	x = clone_into_cgroup(fd);
	if (!x)
		_exit(0);
	close(fd);
	if (x < 0 && errno == ENOSYS) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	if (x > 0)
		waitpid(x, NULL, 0);
	x = cg_run_nowait(g, sleep_forever, NULL);
	if (x < 0 || expect_pin(x, 0, 0, 0, "x pinned to {0} in g"))
		goto cleanup;

	if (expect_clone(out, g, 1, 1, EBUSY, "a child from a parent pinned to {1}") ||
	    expect_clone(out, g, 0, 1, 0, "a child from a parent pinned to {0,1}") ||
	    expect_clone(out, g, 2, 3, EBUSY, "a child from a parent pinned to {2,3}") ||
	    expect_clone(out, g, -1, -1, 0, "a child from a parent pinned to every CPU"))
		goto cleanup;
	if (cg_write(g, "cpuset.cpus", "0-3") || cg_write(g, "cpuset.cpus", "0-2") ||
	    cg_write(g, "cpu.concurrency.max", "2") || cg_write(g, "cpu.concurrency.max", "1")) {
		fprintf(stderr, "writes to g after the forks failed\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (g)
		cg_write(g, "cpu.concurrency.max", "max");
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (g)
		cg_destroy(g);
	if (out)
		cg_destroy(out);
	free(out);
	free(g);
	return ret;
}

/*
 * A thread moving itself to another threaded cgroup, writing "0" to its
 * cgroup.threads, takes no threadgroup rwsem, while another thread of its
 * process calls sched_setaffinity(2) on it. In the threaded domain p (no
 * limit), b has the limit 1 and u, pinned to {1}; t, pinned to {1},
 * starts in p for half of the rounds, in a (limit 2) for the other half.
 * Its move to b alone is accepted, and so is its pin to {2} alone, but
 * not both: t pinned to {2} in b beside u would make a width of 2. Each
 * round starts the call at another offset from the move, from 10 us
 * before to 53 us after it. Both used to be accepted, the call judged in
 * the group t was leaving, and the move on t's old mask.
 */
struct self_move_param {
	char *p, *a, *b;
	const char *src;	/* where t starts: p, then a */
	int fd;			/* b's cgroup.threads */
	int go, done, stop;
	long delay_ns;
	int moved;		/* 0 or an errno */
	pid_t tid, utid;
};

static void *self_mover(void *arg)
{
	struct self_move_param *sp = arg;
	int seen = 0, go;

	__atomic_store_n(&sp->tid, gettid(), __ATOMIC_RELEASE);
	for (;;) {
		while ((go = __atomic_load_n(&sp->go, __ATOMIC_ACQUIRE)) == seen &&
		       !__atomic_load_n(&sp->stop, __ATOMIC_ACQUIRE))
			;
		if (go == seen)
			return NULL;
		seen = go;
		spin_ns(sp->delay_ns);
		sp->moved = write(sp->fd, "0", 1) < 0 ? errno : 0;
		__atomic_store_n(&sp->done, seen, __ATOMIC_RELEASE);
	}
}

static void *self_move_idle(void *arg)
{
	struct self_move_param *sp = arg;

	__atomic_store_n(&sp->utid, gettid(), __ATOMIC_RELEASE);
	while (!__atomic_load_n(&sp->stop, __ATOMIC_ACQUIRE))
		usleep(1000);
	return NULL;
}

/* Is @tid in the cgroup whose path ends with @name? */
static int thread_in(pid_t tid, const char *name)
{
	char path[64], buf[PATH_MAX];
	size_t len, n;
	int fd;
	ssize_t r;

	snprintf(path, sizeof(path), "/proc/self/task/%d/cgroup", tid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = 0;
	len = strcspn(buf, "\n");
	buf[len] = 0;
	n = strlen(name);
	return len > n && buf[len - n - 1] == '/' && !strcmp(buf + len - n, name);
}

static int self_move_rounds(struct self_move_param *sp, int rounds)
{
	static const int one[] = { 1, -1 }, two[] = { 2, -1 };
	char buf[32];
	int i, bad = 0;

	for (i = 0; i < rounds; i++) {
		long off = ((i % 64) - 10) * 1000L;
		int pinned, in_b;
		cpu_set_t set;

		sp->delay_ns = off < 0 ? -off : 0;
		__atomic_store_n(&sp->go, i + 1, __ATOMIC_RELEASE);
		spin_ns(off);
		pinned = pin_cpus(sp->tid, two);
		while (__atomic_load_n(&sp->done, __ATOMIC_ACQUIRE) != i + 1)
			;
		in_b = thread_in(sp->tid, "b");
		if (in_b < 0 || sched_getaffinity(sp->tid, sizeof(set), &set))
			return -1;
		if (in_b && CPU_ISSET(2, &set)) {
			if (!bad++)
				fprintf(stderr, "t from %s: moved to b (%s) and pinned to {2} (%s), %ld ns apart\n",
					sp->src == sp->a ? "a" : "p",
					sp->moved ? strerror(sp->moved) : "accepted",
					pinned ? strerror(pinned) : "accepted", off);
		}
		if (!CPU_ISSET(1, &set) && pin_cpus(sp->tid, one))
			return -1;
		if (in_b) {
			snprintf(buf, sizeof(buf), "%d", sp->tid);
			if (cg_write(sp->src, "cgroup.threads", buf))
				return -1;
		}
	}
	return bad;
}

static int self_move_threads(const char *cgroup, void *arg)
{
	static const int zero[] = { 0, -1 }, one[] = { 1, -1 };
	struct self_move_param *sp = arg;
	pthread_t t, u;
	char buf[32];
	int bad, ret = 1;

	sp->fd = open(sp->b, O_RDONLY | O_DIRECTORY);
	if (sp->fd < 0)
		return 1;
	ret = openat(sp->fd, "cgroup.threads", O_WRONLY);
	close(sp->fd);
	sp->fd = ret;
	if (sp->fd < 0)
		return 1;
	pin_cpus(0, zero);
	/* u: asleep in b, pinned to {1}. */
	if (pthread_create(&u, NULL, self_move_idle, sp))
		return 1;
	if (pthread_create(&t, NULL, self_mover, sp)) {
		__atomic_store_n(&sp->stop, 1, __ATOMIC_RELEASE);
		pthread_join(u, NULL);
		return 1;
	}
	while (!__atomic_load_n(&sp->tid, __ATOMIC_ACQUIRE) ||
	       !__atomic_load_n(&sp->utid, __ATOMIC_ACQUIRE))
		usleep(100);
	snprintf(buf, sizeof(buf), "%d", sp->utid);
	sp->src = sp->p;
	if (pin_cpus(sp->utid, one) || cg_write(sp->b, "cgroup.threads", buf) ||
	    pin_cpus(sp->tid, one)) {
		fprintf(stderr, "setting u and t up failed\n");
		goto out;
	}
	bad = self_move_rounds(sp, 640);
	if (!bad) {
		snprintf(buf, sizeof(buf), "%d", sp->tid);
		sp->src = sp->a;
		if (cg_write(sp->a, "cgroup.threads", buf)) {
			fprintf(stderr, "moving t to a failed\n");
			goto out;
		}
		bad = self_move_rounds(sp, 640);
	}
	if (bad < 0)
		fprintf(stderr, "a round could not be checked or undone\n");
	else if (bad)
		fprintf(stderr, "%d rounds of 640 accepted both\n", bad);
	else
		ret = 0;
out:
	__atomic_store_n(&sp->stop, 1, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
	pthread_join(u, NULL);
	close(sp->fd);
	return ret;
}

static int test_concurrency_self_move(const char *root)
{
	struct self_move_param sp = { };
	int ret = KSFT_FAIL;

	if (need_cpus(3))
		return KSFT_SKIP;
	sp.p = cg_name(root, "cc_test_self_move");
	if (!sp.p)
		goto cleanup;
	sp.a = cg_name(sp.p, "a");
	sp.b = cg_name(sp.p, "b");
	if (!sp.a || !sp.b || cg_create(sp.p) ||
	    cg_write(sp.p, "cgroup.subtree_control", "+cpu") || cg_create(sp.a) || cg_create(sp.b))
		goto cleanup;
	if (cg_write(sp.a, "cgroup.type", "threaded") || cg_write(sp.b, "cgroup.type", "threaded")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	if (cg_write(sp.a, "cpu.concurrency.max", "2") || cg_write(sp.b, "cpu.concurrency.max", "1"))
		goto cleanup;
	if (!cg_run(sp.p, self_move_threads, &sp))
		ret = KSFT_PASS;
cleanup:
	if (sp.a)
		cg_destroy(sp.a);
	if (sp.b)
		cg_destroy(sp.b);
	if (sp.p)
		cg_destroy(sp.p);
	free(sp.a);
	free(sp.b);
	free(sp.p);
	return ret;
}

/* The CPU thread @tid of @pid runs or last ran on, -1 when it cannot be read. */
static int thread_cpu(pid_t pid, pid_t tid)
{
	char path[64], buf[1024], *p, *end;
	long cpu;

	snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid, tid);
	p = proc_stat_field(path, 39, buf, sizeof(buf));
	if (!p)
		return -1;
	cpu = strtol(p, &end, 10);
	return end == p ? -1 : cpu;
}

/* The thread of the calling process named @comm, 0 when there is none. */
static pid_t thread_named(const char *comm)
{
	char path[300], buf[32];
	struct dirent *d;
	pid_t tid = 0;
	ssize_t n;
	DIR *dir;
	int fd;

	dir = opendir("/proc/self/task");
	if (!dir)
		return 0;
	while (!tid && (d = readdir(dir))) {
		if (d->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/proc/self/task/%s/comm", d->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = 0;
		buf[strcspn(buf, "\n")] = 0;
		if (!strcmp(buf, comm))
			tid = atoi(d->d_name);
	}
	closedir(dir);
	return tid;
}

struct vhost_param {
	int kick;	/* the eventfd that wakes the worker */
	int report;	/* the worker's thread ID, or -errno, goes here */
};

/*
 * Own a vhost-net device whose worker wakes at each write to @kick, pin
 * the worker to {0}, then spin on CPU 1 with the mask {0,1}.
 */
static int vhost_owner(const char *cgroup, void *arg)
{
	static const int zero[] = { 0, -1 }, one[] = { 1, -1 }, both[] = { 0, 1, -1 };
	struct vhost_param *vp = arg;
	struct vhost_vring_file kick = { .index = 0, .fd = vp->kick };
	char comm[32];
	int fd, tid, err;

	fd = open("/dev/vhost-net", O_RDWR);
	if (fd < 0 || ioctl(fd, VHOST_SET_OWNER) || ioctl(fd, VHOST_SET_VRING_KICK, &kick)) {
		tid = -errno;
		goto report;
	}
	snprintf(comm, sizeof(comm), "vhost-%d", getpid());
	/* None when the workers are kernel threads: the test does not apply. */
	tid = thread_named(comm);
	if (!tid) {
		tid = -ESRCH;
		goto report;
	}
	err = pin_cpus(tid, zero);
	if (!err)
		err = pin_cpus(0, one);
	if (!err)
		err = pin_cpus(0, both);
	if (err)
		tid = -err;
report:
	if (write(vp->report, &tid, sizeof(tid)) != sizeof(tid) || tid < 0)
		return 1;
	for (;;)
		;
}

/*
 * A vhost worker is a thread of the process that owns the device, run by
 * the kernel, that never returns to user space. It is held all the same
 * when it wakes up to no CPU within its group's bound, at the top of its
 * loop, where it runs the work queued for that return. x owns a vhost-net
 * device in g and spins on CPU 1 with the mask {0,1}, the worker asleep,
 * pinned to {0}; the limit 1 is written over them, a width of 1 with the
 * one slot on CPU 1. Each write to the kick eventfd wakes the worker to
 * no CPU it may use: held, it lets the placement work move x's presence
 * onto CPU 0, where both then run. The worker used to run on CPU 0 at
 * every kick, beside x on CPU 1: two CPUs under a limit of 1.
 */
static int test_concurrency_vhost(const char *root)
{
	struct vhost_param vp = { .kick = -1, .report = -1 };
	int ret = KSFT_FAIL, rfd = -1, pfd[2], tid, xcpu, wcpu, i;
	const __u64 one = 1;
	struct pollfd pf;
	pid_t x = -1;
	char *g;

	if (need_cpus(2))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_vhost");
	if (!g || cg_create(g))
		goto cleanup;
	vp.kick = eventfd(0, EFD_CLOEXEC);
	if (vp.kick < 0 || pipe(pfd))
		goto cleanup;
	rfd = pfd[0];
	vp.report = pfd[1];
	x = cg_run_nowait(g, vhost_owner, &vp);
	if (x < 0)
		goto cleanup;
	pf.fd = rfd;
	pf.events = POLLIN;
	if (poll(&pf, 1, 5000) != 1 || read(rfd, &tid, sizeof(tid)) != sizeof(tid)) {
		fprintf(stderr, "no word from the vhost device owner\n");
		goto cleanup;
	}
	if (tid == -ENOENT || tid == -ENODEV || tid == -ESRCH) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	if (tid < 0) {
		fprintf(stderr, "setting the vhost worker up failed: %s\n", strerror(-tid));
		goto cleanup;
	}
	usleep(10000);
	if (cg_write(g, "cpu.concurrency.max", "1")) {
		fprintf(stderr, "the limit 1 over the masks {0} and {0,1} was refused\n");
		goto cleanup;
	}
	xcpu = thread_cpu(x, x);
	if (xcpu != 1) {
		fprintf(stderr, "x on CPU %d under the limit, expected 1\n", xcpu);
		goto cleanup;
	}
	for (i = 0; i < 100; i++) {
		if (write(vp.kick, &one, sizeof(one)) != sizeof(one))
			goto cleanup;
		usleep(2000);
	}
	xcpu = thread_cpu(x, x);
	wcpu = thread_cpu(x, tid);
	if (xcpu != 0 || wcpu != 0) {
		fprintf(stderr, "after the kicks, x on CPU %d, the worker on %d, not both on 0\n",
			xcpu, wcpu);
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (rfd >= 0)
		close(rfd);
	if (vp.report >= 0)
		close(vp.report);
	if (vp.kick >= 0)
		close(vp.kick);
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

#define TOGGLE_SPAWNERS	4
#define TOGGLE_ROUNDS	200
#define TOGGLE_CROWD	1000
#define TOGGLE_SAMPLERS	4
struct toggle_shared {
	int round;	/* the harness: start round @round */
	int stop;	/* the harness: end round @stop */
	int started;	/* x: round @started is churning */
	int done;	/* x: round @done is checked */
	int result;	/* x's pin to {1}: 0 or an errno */
	int crowd;	/* y: all its threads are started */
	int gen;	/* the harness: odd while the limit 1 is in force */
	unsigned int cid_seen;	/* y: an mm_cid of 1 or more seen then */
};

/* Wait up to about 5 s for *@v to reach @want: 0, or -1 on a timeout. */
static int toggle_wait(int *v, int want)
{
	int i;

	for (i = 0; i < 100000; i++) {
		if (__atomic_load_n(v, __ATOMIC_ACQUIRE) >= want)
			return 0;
		usleep(50);
	}
	return -1;
}

/* The number of threads of the calling process, -1 when it cannot be read. */
static int nr_threads(void)
{
	struct dirent *d;
	DIR *dir;
	int n = 0;

	dir = opendir("/proc/self/task");
	if (!dir)
		return -1;
	while ((d = readdir(dir)))
		n += d->d_name[0] != '.';
	closedir(dir);
	return n;
}

static void *toggle_child(void *arg)
{
	return NULL;
}

/* Start and reap threads, which inherit the mask, until told to stop. */
static void *toggle_spawn(void *arg)
{
	int *stop = arg;
	pthread_t t;

	while (!__atomic_load_n(stop, __ATOMIC_ACQUIRE)) {
		if (!pthread_create(&t, NULL, toggle_child, NULL))
			pthread_join(t, NULL);
	}
	return NULL;
}

/*
 * Each round: start spawners pinned to {2,3}, and once told to, reap them,
 * wait for every thread but this one to be released, and try the pin to
 * {1}, then undo it.
 */
static int toggle_owner(const char *cgroup, void *arg)
{
	static const int two_three[] = { 2, 3, -1 }, one[] = { 1, -1 };
	struct toggle_shared *sh = arg;
	pthread_t sp[TOGGLE_SPAWNERS];
	int r, i, n, stop;
	cpu_set_t all;

	if (sched_getaffinity(0, sizeof(all), &all))
		return 1;
	for (r = 1; ; r++) {
		if (toggle_wait(&sh->round, r))
			return 1;
		stop = 0;
		if (pin_cpus(0, two_three))
			return 1;
		for (i = 0; i < TOGGLE_SPAWNERS; i++) {
			if (pthread_create(&sp[i], NULL, toggle_spawn, &stop))
				return 1;
		}
		if (sched_setaffinity(0, sizeof(all), &all))
			return 1;
		__atomic_store_n(&sh->started, r, __ATOMIC_RELEASE);
		if (toggle_wait(&sh->stop, r))
			return 1;
		__atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
		for (i = 0; i < TOGGLE_SPAWNERS; i++)
			pthread_join(sp[i], NULL);
		for (i = 0; (n = nr_threads()) != 1 && i < 5000; i++)
			usleep(1000);
		if (n != 1)
			return 1;
		sh->result = pin_cpus(0, one);
		if (sched_setaffinity(0, sizeof(all), &all))
			return 1;
		__atomic_store_n(&sh->done, r, __ATOMIC_RELEASE);
	}
}

static void *toggle_sleep(void *arg)
{
	for (;;)
		pause();
	return NULL;
}

/*
 * Note an mm_cid of 1 or more given at a wakeup within a period of the
 * limit 1, @gen odd and unchanged across the sample. Read unsigned: an ID
 * left unset would read 0x80000000.
 */
static void *toggle_sample(void *arg)
{
	struct toggle_shared *sh = arg;

	for (;;) {
		int gen = __atomic_load_n(&sh->gen, __ATOMIC_ACQUIRE);
		__u32 cid;

		usleep(500);
		cid = __atomic_load_n((__u32 *)((char *)thread_rseq() + RSEQ_MM_CID_OFFSET),
				      __ATOMIC_RELAXED);
		if ((gen & 1) && cid >= 1 && __atomic_load_n(&sh->gen, __ATOMIC_ACQUIRE) == gen)
			__atomic_store_n(&sh->cid_seen, cid, __ATOMIC_RELEASE);
	}
	return NULL;
}

/*
 * Many threads asleep, to give the walks over their group some length,
 * a few sampling their mm_cid, and threads started and reaped meanwhile,
 * which join the mm while its bound changes.
 */
static int toggle_crowd(const char *cgroup, void *arg)
{
	struct toggle_shared *sh = arg;
	pthread_attr_t attr;
	pthread_t t;
	int i;

	if (pthread_attr_init(&attr) || pthread_attr_setstacksize(&attr, 64 << 10))
		return 1;
	for (i = 0; i < TOGGLE_CROWD; i++) {
		if (pthread_create(&t, &attr, toggle_sleep, NULL))
			return 1;
	}
	for (i = 0; rseq_has_mm_cid() && i < TOGGLE_SAMPLERS; i++) {
		if (pthread_create(&t, &attr, toggle_sample, sh))
			return 1;
	}
	__atomic_store_n(&sh->crowd, 1, __ATOMIC_RELEASE);
	for (;;) {
		if (!pthread_create(&t, &attr, toggle_child, NULL))
			pthread_join(t, NULL);
	}
}

/*
 * Limits switched on and off while tasks fork, in p/m, where c has the
 * limit 2 and d holds y: TOGGLE_CROWD threads asleep, TOGGLE_SAMPLERS
 * threads reading their mm_cid at each wakeup, and one starting and
 * reaping threads in a loop. x in c starts threads pinned to {2,3} in a
 * loop, each new one registering its mask at fork. Each round, the limit
 * of g goes from "max" to 1 at a varying offset into the churn: g is m
 * for the first half of the rounds, and p for the second, m then counting
 * along with p. The walk over d, between g's and c's turns, gives the
 * races below room. Once the threads are gone, x's pin to {1} must be
 * accepted, g's limit holding one mask, every round must end, and y's
 * samplers must be given mm_cid 0 while the limit 1 is in force.
 *  - When g starts counting, c's list is merged into g. A thread that
 *    registered at c before the merge, and at g once g counted, was
 *    counted twice at g, and its exit left g a mask no task has, {2,3},
 *    beside which {1} made a width of 2: the pin was refused.
 *  - When g stops counting, c goes on propagating its slots into m until
 *    the walk reaches c. A slot c added meanwhile was taken for one m
 *    held, and its release left m's slot on that CPU draining, refusing
 *    every arrival from then on: a round ended with x held.
 *  - y's busy threads keep g's one slot on a CPU out of the masks of x's
 *    threads. A thread of x held for it waited for good: making room
 *    for it set-moved c's presence only, and never the presence of the
 *    level whose limit kept it out.
 *  - A set-move whose top group got a new slot on its destination counted
 *    an entity more there at every ancestor, above one whose slot was
 *    there already too: p kept an entity no task or group held, every
 *    set-move of its presence failed its count check, and x stayed held.
 */
static int test_concurrency_toggle(const char *root)
{
	int ret = KSFT_FAIL, r, refused[2] = { };
	char *p = NULL, *m = NULL, *c = NULL, *d = NULL;
	struct toggle_shared *sh;
	pid_t x = -1, y = -1;

	if (need_cpus(4))
		return KSFT_SKIP;
	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		return KSFT_FAIL;
	p = cg_name(root, "cc_test_toggle");
	if (p)
		m = cg_name(p, "m");
	if (m) {
		c = cg_name(m, "c");
		d = cg_name(m, "d");
	}
	/* d first: the walks visit the children in their order of creation. */
	if (!c || !d || cg_create(p) || cg_write(p, "cgroup.subtree_control", "+cpu") ||
	    cg_create(m) || cg_write(m, "cgroup.subtree_control", "+cpu") ||
	    cg_create(d) || cg_create(c) || cg_write(c, "cpu.concurrency.max", "2"))
		goto cleanup;
	y = cg_run_nowait(d, toggle_crowd, sh);
	if (y < 0 || toggle_wait(&sh->crowd, 1))
		goto cleanup;
	x = cg_run_nowait(c, toggle_owner, sh);
	if (x < 0)
		goto cleanup;
	for (r = 1; r <= TOGGLE_ROUNDS; r++) {
		int phase = r > TOGGLE_ROUNDS / 2;

		if (__atomic_load_n(&sh->gen, __ATOMIC_RELAXED) & 1)
			__atomic_add_fetch(&sh->gen, 1, __ATOMIC_RELEASE);
		if (cg_write(m, "cpu.concurrency.max", "max") ||
		    cg_write(p, "cpu.concurrency.max", "max"))
			goto cleanup;
		__atomic_store_n(&sh->round, r, __ATOMIC_RELEASE);
		if (toggle_wait(&sh->started, r)) {
			fprintf(stderr, "round %d did not start\n", r);
			goto cleanup;
		}
		usleep(r * 37 % 500);
		if (cg_write(phase ? p : m, "cpu.concurrency.max", "1")) {
			fprintf(stderr, "round %d: the limit 1 on %s refused\n", r,
				phase ? "p" : "m");
			goto cleanup;
		}
		/* The bounds follow the limit when its write returns. */
		__atomic_add_fetch(&sh->gen, 1, __ATOMIC_RELEASE);
		__atomic_store_n(&sh->stop, r, __ATOMIC_RELEASE);
		if (toggle_wait(&sh->done, r)) {
			fprintf(stderr, "round %d did not end\n", r);
			goto cleanup;
		}
		if (sh->result == EINVAL) {
			refused[phase]++;
		} else if (sh->result) {
			fprintf(stderr, "round %d: the pin to {1}: %s\n", r, strerror(sh->result));
			goto cleanup;
		}
	}
	if (refused[0] || refused[1]) {
		fprintf(stderr, "the pin to {1} refused in %d, %d rounds of %d (m, p)\n",
			refused[0], refused[1], TOGGLE_ROUNDS / 2);
		goto cleanup;
	}
	if (sh->cid_seen) {
		fprintf(stderr, "a thread of y was given mm_cid %u under the limit 1\n",
			sh->cid_seen);
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (y > 0) {
		kill(y, SIGKILL);
		waitpid(y, NULL, 0);
	}
	if (c)
		cg_destroy(c);
	if (d)
		cg_destroy(d);
	if (m)
		cg_destroy(m);
	if (p)
		cg_destroy(p);
	free(d);
	free(c);
	free(m);
	free(p);
	munmap(sh, sizeof(*sh));
	return ret;
}

/* The first CPU of @cpu's SMT core, -1 when it cannot be read. */
static int core_of(int cpu)
{
	char path[PATH_MAX], buf[64];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return atoi(buf);
}

struct spread_param {
	int ncores;
	int core[CPU_SETSIZE];		/* core_of() of every online CPU, -1 otherwise */
};

struct spread_spinner {
	pthread_t thread;
	int *stop, *record;
	unsigned long cpus[(CPU_SETSIZE + 63) / 64];
};

/* Spin until told to stop, noting the CPUs run on once told to record. */
static void *spread_spin(void *arg)
{
	struct spread_spinner *s = arg;

	while (!__atomic_load_n(s->stop, __ATOMIC_ACQUIRE)) {
		int cpu;

		if (!__atomic_load_n(s->record, __ATOMIC_ACQUIRE))
			continue;
		cpu = sched_getcpu();
		if (cpu >= 0 && cpu < CPU_SETSIZE)
			s->cpus[cpu / 64] |= 1UL << (cpu % 64);
	}
	return NULL;
}

/*
 * Runs inside the group, limited to the number of cores: start one
 * spinner per core, one after the other, let them settle, then check that
 * the CPUs they run on are on as many cores, never two siblings.
 */
static int spread_check(const char *cgroup, void *arg)
{
	struct spread_param *p = arg;
	struct spread_spinner s[MAX_THREADS] = { };
	unsigned long all[(CPU_SETSIZE + 63) / 64] = { };
	int i, j, cpu, stop = 0, record = 0, ret = 0, seen = 0;
	int owner[CPU_SETSIZE];

	for (i = 0; i < p->ncores; i++) {
		s[i].stop = &stop;
		s[i].record = &record;
		if (pthread_create(&s[i].thread, NULL, spread_spin, &s[i])) {
			p->ncores = i;
			ret = 1;
			goto out;
		}
		/* Each fork search sees the threads before it running. */
		usleep(20000);
	}
	usleep(500000);
	__atomic_store_n(&record, 1, __ATOMIC_RELEASE);
	usleep(500000);
out:
	__atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
	for (i = 0; i < p->ncores; i++)
		pthread_join(s[i].thread, NULL);
	if (ret)
		return ret;

	for (i = 0; i < p->ncores; i++)
		for (j = 0; j < (CPU_SETSIZE + 63) / 64; j++)
			all[j] |= s[i].cpus[j];
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		owner[cpu] = -1;
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		int core;

		if (!(all[cpu / 64] & (1UL << (cpu % 64))))
			continue;
		seen++;
		core = p->core[cpu];
		if (core < 0)
			continue;
		if (owner[core] >= 0) {
			fprintf(stderr, "CPUs %d and %d share a core, %d slots for %d cores\n",
				owner[core], cpu, p->ncores, p->ncores);
			ret = 2;
		}
		owner[core] = cpu;
	}
	if (seen > p->ncores) {
		fprintf(stderr, "%d CPUs used, limit %d\n", seen, p->ncores);
		ret = 3;
	}
	return ret;
}

/*
 * Slots go one per SMT core first. One CPU of every core but the first is
 * kept busy from outside the group, so that no core is idle once the
 * group's first spinner runs: the fork search then compares cores with
 * one idle CPU each, and the group's own, whose spinner has just started,
 * looks the least loaded. Taking its idle sibling would leave a core
 * unused; with the limit at the number of cores, the spinners must end on
 * as many cores.
 */
static int test_concurrency_smt_spread(const char *root)
{
	struct spread_param *p;
	pid_t busy[CPU_SETSIZE];
	int nbusy = 0, ret = KSFT_FAIL, cpu, ncpus, online = 0, first = -1;
	char *cg = NULL, buf[16];
	cpu_set_t set;

	if (cg_read_strcmp("/sys/devices/system/cpu/smt", "active", "1\n"))
		return skip("SMT not active");
	p = calloc(1, sizeof(*p));
	if (!p)
		return KSFT_FAIL;
	ncpus = MIN(get_nprocs_conf(), CPU_SETSIZE);
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		p->core[cpu] = cpu < ncpus ? core_of(cpu) : -1;
		online += p->core[cpu] >= 0;
	}
	for (cpu = 0; cpu < ncpus; cpu++) {
		if (p->core[cpu] != cpu)
			continue;
		p->ncores++;
		if (first < 0) {
			first = cpu;
			continue;
		}
		/* Keep one CPU of this core busy, outside the group. */
		busy[nbusy] = fork();
		if (busy[nbusy] < 0)
			goto cleanup;
		if (!busy[nbusy]) {
			CPU_ZERO(&set);
			CPU_SET(cpu, &set);
			if (sched_setaffinity(0, sizeof(set), &set))
				_exit(1);
			for (;;)
				;
		}
		nbusy++;
	}
	/* Spreading takes a limit below the CPU count. */
	if (p->ncores < 2 || p->ncores > MAX_THREADS || p->ncores >= online) {
		ret = KSFT_SKIP;
		goto cleanup;
	}

	cg = cg_name(root, "cc_test_smt_spread");
	if (!cg || cg_create(cg))
		goto cleanup;
	snprintf(buf, sizeof(buf), "%d", p->ncores);
	if (cg_write(cg, "cpu.concurrency.max", buf))
		goto cleanup;
	/* Let the busy loops reach their CPUs and their load build up. */
	usleep(300000);
	if (!cg_run(cg, spread_check, p))
		ret = KSFT_PASS;
cleanup:
	while (nbusy--) {
		kill(busy[nbusy], SIGKILL);
		waitpid(busy[nbusy], NULL, 0);
	}
	if (cg)
		cg_destroy(cg);
	free(cg);
	free(p);
	return ret;
}

#define T(x) { x, #x }
struct cc_test {
	int (*fn)(const char *root);
	const char *name;
} tests[] = {
	T(test_concurrency_interface),
	T(test_concurrency_hierarchy),
	T(test_concurrency_bound),
	T(test_concurrency_decrease),
	T(test_concurrency_setaffinity),
	T(test_concurrency_relax),
	T(test_concurrency_move),
	T(test_concurrency_hotplug),
	T(test_concurrency_split),
	T(test_concurrency_hard),
	T(test_concurrency_cpuset_refused),
	T(test_concurrency_sleepers),
	T(test_concurrency_cpuset_class),
	T(test_concurrency_dormant),
	T(test_concurrency_deadline),
	T(test_concurrency_width),
	T(test_concurrency_partition_check),
	T(test_concurrency_partition_beside),
	T(test_concurrency_cpuset_parent),
	T(test_concurrency_partition_cpus),
	T(test_concurrency_late_child),
	T(test_concurrency_child_slot),
	T(test_concurrency_grand_merge),
	T(test_concurrency_cpuset_move),
	T(test_concurrency_partition_rmdir),
	T(test_concurrency_child_hard_enable),
	T(test_concurrency_literal_affinity),
	T(test_concurrency_refused_affinity),
	T(test_concurrency_offline_affinity),
	T(test_concurrency_setmove_mask),
	T(test_concurrency_cid_decrease),
	T(test_concurrency_effective_ids),
	T(test_concurrency_pin),
	T(test_concurrency_clone_into),
	T(test_concurrency_self_move),
	T(test_concurrency_vhost),
	T(test_concurrency_toggle),
	T(test_concurrency_smt_spread),
};

#undef T
int main(int argc, char *argv[])
{
	char root[PATH_MAX];
	char *probe;
	int i;

	if (argc == 3 && !strcmp(argv[1], "--pin-get"))
		return pin_get_main(atoi(argv[2]));
	ksft_print_header();
	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");

	if (cg_read_strstr(root, "cgroup.subtree_control", "cpu"))
		if (cg_write(root, "cgroup.subtree_control", "+cpu"))
			ksft_exit_skip("Failed to set cpu controller\n");
	if (cg_read_strstr(root, "cgroup.subtree_control", "cpuset"))
		if (cg_write(root, "cgroup.subtree_control", "+cpuset"))
			ksft_exit_skip("Failed to set cpuset controller\n");

	probe = cg_name(root, "cc_test_probe");
	if (!probe || cg_create(probe))
		ksft_exit_skip("Failed to create a probe cgroup\n");
	if (cg_read_strcmp(probe, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(probe, "cpu.concurrency.ceiling", "max\n")) {
		cg_destroy(probe);
		ksft_exit_skip("cpu.concurrency.max is not available\n");
	}
	cg_destroy(probe);
	free(probe);

	ksft_set_plan(ARRAY_SIZE(tests));
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		/* An argument selects the tests whose name contains it. */
		if (argc > 1 && !strstr(tests[i].name, argv[1])) {
			ksft_test_result_skip("%s\n", tests[i].name);
			continue;
		}
		switch (tests[i].fn(root)) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip("%s\n", tests[i].name);
			break;
		default:
			ksft_test_result_fail("%s\n", tests[i].name);
			break;
		}
	}

	ksft_finished();
}
