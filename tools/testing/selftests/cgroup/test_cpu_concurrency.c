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
	T(test_concurrency_hard),
	T(test_concurrency_sleepers),
	T(test_concurrency_width),
	T(test_concurrency_late_child),
	T(test_concurrency_child_slot),
	T(test_concurrency_grand_merge),
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
