// SPDX-License-Identifier: GPL-2.0

/*
 * Tests for cpu.concurrency.max, see
 * Documentation/scheduler/sched-max-concurrency.rst
 */

#include "cpu_concurrency_util.h"
/*
 * A workload which measures itself from user space, for the cases with
 * more runnable tasks than slots.
 *
 * A case describes its tasks in a struct rot, which lives in a shared
 * mapping, and starts them with rot_start(). Each task is a thread. The
 * threads of one rot_start() call are one process, so their mm_cid values
 * come from one set of IDs. Each thread sets itself up, sleeps until the
 * case calls rot_go(), and then runs until the end of the measured time.
 * The test reads the results from the mapping, also while the tasks run.
 */
#define ROT_TASKS	20
#define ROT_LATS	256
/*
 * A spinner reads the clock in a loop. A step longer than this between two
 * reads means that it was off the CPU meanwhile.
 */
#define ROT_GAP_NS	200000LL

enum rot_kind {
	ROT_SPIN,	/* spin */
	ROT_PERIODIC,	/* wake up every period_us, then spin for work_us */
};

struct rot_task {
	/* Set by the case before rot_start(). */
	enum rot_kind kind;
	int cpu_lo, cpu_hi;	/* pin itself to these CPUs, if cpu_lo >= 0 */
	int fifo;		/* its SCHED_FIFO priority, 0 for a fair task */
	const char *cg;		/* if set, move itself to this cgroup first */
	int period_us, work_us;
	/* A spinner counts a time off the CPU as a wait from this length on. */
	long long wait_min_ns;
	/* Set by the task. */
	int err;		/* errno of its setup, 0 if it went well */
	int ready, done;
	pid_t pid, tid;
	long long run_ns;	/* its time on the CPU, in user space */
	long long max_gap_ns;	/* its longest time off the CPU */
	long long wait_ns;	/* the sum of its waits */
	int nwaits;
	unsigned int max_cid;	/* the largest mm_cid it read */
	int nlat;		/* the wakeups of a periodic task */
	int lat_us[ROT_LATS];	/* how late it ran after each one */
};

struct rot {
	long long t0_ns, end_ns;	/* the measured time, CLOCK_MONOTONIC */
	int go, stop;
	struct rot_task t[ROT_TASKS];
};

static long long mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_until_ns(long long ns)
{
	struct timespec ts = { .tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

/* Record the mm_cid of the calling thread in @t if it is the largest so far. */
static void rot_note_cid(struct rot_task *t)
{
	__u32 cid;

	if (!rseq_has_mm_cid())
		return;
	/* Unsigned: an ID left unset reads 0x80000000. */
	cid = rseq_field(thread_rseq(), RSEQ_MM_CID_OFFSET);
	if (cid > t->max_cid)
		__atomic_store_n(&t->max_cid, cid, __ATOMIC_RELAXED);
}

static void rot_spin(struct rot *r, struct rot_task *t)
{
	long long last = r->t0_ns, now, gap;
	int end = 0;

	while (!end) {
		now = mono_ns();
		if (now >= r->end_ns || __atomic_load_n(&r->stop, __ATOMIC_RELAXED)) {
			now = MIN(now, r->end_ns);
			end = 1;
		}
		gap = now - last;
		if (gap <= ROT_GAP_NS) {
			__atomic_store_n(&t->run_ns, t->run_ns + gap, __ATOMIC_RELAXED);
		} else {
			if (gap > t->max_gap_ns)
				__atomic_store_n(&t->max_gap_ns, gap, __ATOMIC_RELAXED);
			if (t->wait_min_ns && gap >= t->wait_min_ns) {
				t->wait_ns += gap;
				t->nwaits++;
			}
		}
		rot_note_cid(t);
		last = now;
	}
}

static void rot_periodic(struct rot *r, struct rot_task *t)
{
	long long next = r->t0_ns, now, start;

	while (!__atomic_load_n(&r->stop, __ATOMIC_RELAXED)) {
		next += t->period_us * 1000LL;
		if (next >= r->end_ns)
			break;
		sleep_until_ns(next);
		start = mono_ns();
		if (t->nlat < ROT_LATS) {
			t->lat_us[t->nlat] = (start - next) / 1000;
			__atomic_store_n(&t->nlat, t->nlat + 1, __ATOMIC_RELEASE);
		}
		rot_note_cid(t);
		while ((now = mono_ns()) - start < t->work_us * 1000LL)
			;
		__atomic_store_n(&t->run_ns, t->run_ns + now - start, __ATOMIC_RELAXED);
		rot_note_cid(t);
		/* It missed the next wakeup: count the periods from now. */
		if (now >= next + t->period_us * 1000LL)
			next = now;
	}
}

struct rot_arg {
	struct rot *r;
	int i;
};

static void *rot_thread(void *arg)
{
	struct rot_arg *a = arg;
	struct rot *r = a->r;
	struct rot_task *t = &r->t[a->i];
	struct sched_param prio = { .sched_priority = t->fifo };
	char buf[32];
	cpu_set_t set;
	int i;

	t->tid = gettid();
	if (t->cg) {
		snprintf(buf, sizeof(buf), "%d", (int)t->tid);
		t->err = -cg_write(t->cg, "cgroup.threads", buf);
	}
	if (!t->err && t->cpu_lo >= 0) {
		CPU_ZERO(&set);
		for (i = t->cpu_lo; i <= t->cpu_hi; i++)
			CPU_SET(i, &set);
		if (sched_setaffinity(0, sizeof(set), &set))
			t->err = errno;
	}
	if (!t->err && t->fifo && sched_setscheduler(0, SCHED_FIFO, &prio))
		t->err = errno;
	__atomic_store_n(&t->ready, 1, __ATOMIC_RELEASE);
	if (t->err)
		return NULL;
	while (!__atomic_load_n(&r->go, __ATOMIC_ACQUIRE)) {
		if (__atomic_load_n(&r->stop, __ATOMIC_ACQUIRE))
			return NULL;
		usleep(1000);
	}
	sleep_until_ns(r->t0_ns);
	if (t->kind == ROT_PERIODIC)
		rot_periodic(r, t);
	else
		rot_spin(r, t);
	__atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

struct rot_proc {
	struct rot *r;
	int first, n;
};

static int rot_proc_fn(const char *cgroup, void *arg)
{
	struct rot_proc *p = arg;
	struct rot_arg a[ROT_TASKS];
	pthread_t th[ROT_TASKS];
	int i;

	for (i = 0; i < p->n; i++) {
		a[i].r = p->r;
		a[i].i = p->first + i;
		p->r->t[a[i].i].pid = getpid();
		if (pthread_create(&th[i], NULL, rot_thread, &a[i]))
			return 1;
	}
	for (i = 0; i < p->n; i++)
		pthread_join(th[i], NULL);
	return 0;
}

/* Map a struct rot with no task pinned. Return NULL on failure. */
static struct rot *rot_alloc(void)
{
	struct rot *r;
	int i;

	r = mmap(NULL, sizeof(*r), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (r == MAP_FAILED)
		return NULL;
	for (i = 0; i < ROT_TASKS; i++)
		r->t[i].cpu_lo = -1;
	return r;
}

/* Make task @i of @r a spinner pinned to CPUs @lo to @hi. */
static void rot_pinned(struct rot *r, int i, int lo, int hi)
{
	r->t[i].kind = ROT_SPIN;
	r->t[i].cpu_lo = lo;
	r->t[i].cpu_hi = hi;
}

/*
 * Start a process in @cg which runs tasks @first to @first + @n - 1 of @r.
 * Return its pid, or -1.
 */
static pid_t rot_start(struct rot *r, const char *cg, int first, int n)
{
	struct rot_proc p = { .r = r, .first = first, .n = n };

	return cg_run_nowait(cg, rot_proc_fn, &p);
}

/*
 * Wait until tasks @first to @first + @n - 1 of @r are set up. Return 0.
 * Return -1 and print the reason if a setup failed, or if a task is not
 * ready after 10 s. A setup is a few system calls, so 10 s only pass when
 * a task is stuck.
 */
static int rot_wait_ready(struct rot *r, int first, int n)
{
	int i, t;

	for (i = first; i < first + n; i++) {
		for (t = 0; t < 10000 && !__atomic_load_n(&r->t[i].ready, __ATOMIC_ACQUIRE); t++)
			usleep(1000);
		if (!r->t[i].ready) {
			fprintf(stderr, "task %d was not set up within 10 s\n", i);
			return -1;
		}
		if (r->t[i].err) {
			fprintf(stderr, "the setup of task %d failed: %s\n", i,
				strerror(r->t[i].err));
			return -1;
		}
	}
	return 0;
}

/* Let the tasks of @r run for @ms, from 50 ms after this call. */
static void rot_go(struct rot *r, int ms)
{
	r->t0_ns = mono_ns() + 50000000LL;
	r->end_ns = r->t0_ns + ms * 1000000LL;
	__atomic_store_n(&r->go, 1, __ATOMIC_RELEASE);
}

/* Stop the tasks of @r, then kill and reap its @n processes of @pid. */
static void rot_end(struct rot *r, pid_t *pid, int n)
{
	int i;

	__atomic_store_n(&r->stop, 1, __ATOMIC_RELEASE);
	for (i = 0; i < n; i++) {
		if (pid[i] > 0) {
			kill(pid[i], SIGKILL);
			waitpid(pid[i], NULL, 0);
		}
		pid[i] = -1;
	}
}

/* Return the time task @i of @r has run so far, in ms. */
static long rot_run_ms(struct rot *r, int i)
{
	return __atomic_load_n(&r->t[i].run_ns, __ATOMIC_RELAXED) / 1000000;
}

/*
 * Let the tasks @first to @first + @n - 1 of @r run for @ms more, and
 * measure that time. Return 0 if both hold:
 *  - each task ran for @min_pct percent of the time or more;
 *  - the tasks ran for @min_sum_pct to @max_sum_pct percent of it in all.
 *    A @max_sum_pct of 0 is not checked.
 * Else print the times with @what and return -1.
 *
 * The callers allow 4% above the CPUs of the limit in @max_sum_pct. A
 * task counts a step of up to 200 us off the CPU as run time. And the
 * run times of the tasks are not read at one instant.
 */
static int rot_window(struct rot *r, int first, int n, int ms, int min_pct, int min_sum_pct,
		      int max_sum_pct, const char *what)
{
	long before[ROT_TASKS], ran, sum = 0, elapsed;
	long long t0 = mono_ns();
	int i, bad = 0;

	for (i = 0; i < n; i++)
		before[i] = rot_run_ms(r, first + i);
	usleep(ms * 1000);
	elapsed = (mono_ns() - t0) / 1000000;
	for (i = 0; i < n; i++) {
		ran = rot_run_ms(r, first + i) - before[i];
		before[i] = ran;
		sum += ran;
		bad |= ran * 100 < (long)min_pct * elapsed;
	}
	bad |= sum * 100 < (long)min_sum_pct * elapsed;
	bad |= max_sum_pct && sum * 100 > (long)max_sum_pct * elapsed;
	if (!bad)
		return 0;
	fprintf(stderr, "%s: in %ld ms the tasks ran", what, elapsed);
	for (i = 0; i < n; i++)
		fprintf(stderr, " %ld", before[i]);
	fprintf(stderr, " ms; each must run %d%% or more", min_pct);
	if (min_sum_pct)
		fprintf(stderr, ", all %d%% or more", min_sum_pct);
	if (max_sum_pct)
		fprintf(stderr, ", all %d%% at most", max_sum_pct);
	fprintf(stderr, "\n");
	return -1;
}

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

/*
 * A child is never allowed more than its parent. The parent gets the
 * limit 2: the child's effective value reads 2. The child gets the limit
 * 4: it still reads 2. The child gets the limit 1: it reads 1. The parent
 * gets "max": the child still reads 1.
 */
static int test_concurrency_hierarchy(const char *root)
{
	int ret = KSFT_FAIL;
	char *parent = NULL, *child = NULL;

	/* The case writes a limit of 4. A limit above the CPUs is refused. */
	if (need_cpus(4))
		return KSFT_SKIP;
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

	ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * test_concurrency_decrease() runs this in a cgroup limited to 3. Start
 * up to 8 spinners and let them run for 300 ms. Write a limit of 1. The
 * effective value must read 1 when the write returns. Wait 300 ms and
 * forget the CPUs seen so far. From then on the spinners must all run on
 * one CPU.
 */
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
 * test_concurrency_move() runs this in a cgroup limited to 1. One thread
 * spins for a second. Three times, the main thread pins it to a CPU it
 * has not run on yet. The cgroup has one CPU, so it must leave that CPU
 * for the new one each time. The effective value must stay 1 and the
 * spinner must never read an mm_cid above 0.
 */
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

	/* The spinner starts on one CPU and is moved to three others. */
	if (need_cpus(4))
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
 * test_concurrency_sleepers() runs this in a cgroup limited to 2. Start
 * two sleepers. While they sleep, pin each to one of the last two CPUs.
 * Then start three spinners, which can run on every CPU. The sleepers
 * wake up every millisecond for 900 ms.
 *
 * Two slots serve all five threads: one on each CPU of a sleeper. A
 * sleeper can run nowhere else, and the spinners can run there.
 *
 * Checked at the end:
 *  - no thread read an mm_cid of 2 or more;
 *  - the effective value still reads 2;
 *  - each sleeper ran on its CPU alone;
 *  - each sleeper took SLEEPER_SAMPLES samples at least. It takes one at
 *    each wakeup. A sleeper which waits for a rotation at each wakeup
 *    takes 9 at most.
 * The caller checks that nr_rotations of the cgroup is still 0.
 *
 * The sleepers are pinned while asleep on purpose: they then hold no slot
 * on their CPUs when the spinners start.
 */
#define SLEEPER_SAMPLES	20

static int sleepers_and_spinners(const char *cgroup, void *arg)
{
	struct spinner s[5] = { };
	struct timespec until;
	cpu_set_t set;
	int i, n = get_nprocs(), err;
	long long max;

	clock_gettime(CLOCK_MONOTONIC, &until);
	until.tv_sec += 1;
	/*
	 * Start the sleepers first. They sleep before their first sample,
	 * and are pinned meanwhile.
	 */
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
	max = max_mm_cid_seen(s, 5);
	if (rseq_has_mm_cid() && max >= 2) {
		fprintf(stderr, "mm_cid %lld seen, limit 2\n", max);
		return 5;
	}
	for (i = 0; i < 2; i++) {
		if (s[i].samples < SLEEPER_SAMPLES || !spinner_only_on(&s[i], n - 1 - i)) {
			fprintf(stderr, "sleeper %d took %lu samples, on CPU %d only: %s\n", i,
				s[i].samples, n - 1 - i,
				spinner_only_on(&s[i], n - 1 - i) ? "yes" : "no");
			return 6;
		}
	}
	return 0;
}

static int test_concurrency_sleepers(const char *root)
{
	struct conc_stat st = { };
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
	if (conc_stat_read(cg, &st) || st.nr_rotations) {
		fprintf(stderr, "nr_rotations is %lld, the threads fit in two slots\n",
			st.nr_rotations);
		goto cleanup;
	}

	ret = KSFT_PASS;
cleanup:
	cg_destroy(cg);
	free(cg);
	return ret;
}

/*
 * The ceiling. The steps of test_concurrency_hard(), in order:
 *  - the root has cpu.concurrency.child_ceiling;
 *  - child, later, one and two are created while the parent's
 *    cpu.concurrency.child_ceiling is 4, 6, 1 and 2. Each takes that
 *    value as its ceiling and keeps it. The effective value of child
 *    reads "4 4": a ceiling without a limit is enforced as a limit;
 *  - a limit of 2, then 3, is written to child. Its ceiling stays 4;
 *  - two values written to cpu.concurrency.max fail with EINVAL. So
 *    does a limit of 5, above the ceiling;
 *  - grand is created under child while child's
 *    cpu.concurrency.child_ceiling is 8. Its ceiling reads 8. Its
 *    effective value reads "3 4": the limit and the ceiling of child.
 */
static int test_concurrency_hard(const char *root)
{
	char *parent = NULL, *child = NULL, *later = NULL, *grand = NULL;
	char *one = NULL, *two = NULL, buf[32];
	int ret = KSFT_FAIL;

	/*
	 * A value above the number of possible CPUs is refused, and the
	 * test writes 8.
	 */
	if (get_nprocs_conf() < 8)
		return skip("8 possible CPUs needed, %d", get_nprocs_conf());
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
	/* The root has cpu.concurrency.child_ceiling too. */
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
	    cg_read_strcmp(child, "cpu.concurrency.effective", "4 4\n") ||
	    cg_read_strcmp(later, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(later, "cpu.concurrency.ceiling", "6\n")) {
		fprintf(stderr, "children not created with the hard values 4 and 6\n");
		goto cleanup;
	}
	/* A limit can be written and changed. The ceiling stays 4. */
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
	/* grand's own ceiling reads 8. Its effective ceiling is child's, 4. */
	if (cg_write(child, "cgroup.subtree_control", "+cpu") ||
	    cg_write(child, "cpu.concurrency.child_ceiling", "8") || cg_create(grand) ||
	    cg_read_strcmp(grand, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(grand, "cpu.concurrency.ceiling", "8\n") ||
	    cg_read_strcmp(grand, "cpu.concurrency.effective", "3 4\n"))
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

static int busy_forever(const char *cgroup, void *arg)
{
	for (;;)
		;
	return 0;
}

/*
 * A set-move avoids isolated CPUs. With isolcpus=domain, an isolated CPU
 * is outside the scheduling domains. A task runs there only when its
 * mask names the CPU.
 * g has the limit 1. x spins in it, pinned to c0, a housekeeping CPU. I
 * is an isolated CPU. H is another housekeeping CPU, above I.
 *  - x gets the mask {I, H}. g must move to another CPU, since c0 is not
 *    in the mask. It must move to H, not to I, although I is the first
 *    CPU of the mask.
 *  - x gets the mask {I}. g must move to I.
 * Skipped without an isolated CPU below a second housekeeping CPU.
 */
static int stat_cpu(pid_t pid)
{
	char path[64], buf[1024], *p;
	int fd, field;
	ssize_t n;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	/* Field 2, the command, may hold spaces: count from its end. */
	p = strrchr(buf, ')');
	if (!p)
		return -1;
	for (field = 2; field < 39 && p; field++)
		p = strchr(p + 1, ' ');
	return p ? atoi(p + 1) : -1;
}

/*
 * A cgroup created under a limited parent is limited from its creation.
 * The parent has the limit 1. The child is created after that. Its
 * effective value must read 1. Spinners in the child must stay on one
 * CPU and read no mm_cid above 0.
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
 * A slot left by a task which exits in a child cgroup is released without
 * a rotation.
 *
 * Cgroups: cs, and its children a and b, each a cpu cgroup of its own.
 * Tasks: z in a, pinned to CPU 1, and x in b, pinned to CPU 2. Both wait
 * for the test. The steps, all done by the test:
 *  1. Write 1 to cpu.concurrency.max of cs.
 *  2. Tell z to run. It uses 300 ms of CPU time and exits. Its slot on
 *     CPU 1 stays for a while.
 *  3. 50 ms after the exit of z, tell x to run.
 * x finds the one slot of cs on CPU 1, with no task in it.
 *
 * Checked:
 *  - x gets 300 ms of CPU time within 5 s;
 *  - nr_rotations of cs is still 0. An idle slot is removed for a task
 *    which waits. It is not taken by a rotation, which would make x wait
 *    for a period.
 */
static int test_concurrency_child_slot(const char *root)
{
	char *cs = NULL, *a = NULL, *b = NULL, c = 'g';
	struct slot_task x = { }, z = { };
	struct conc_stat st = { };
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
	if (conc_stat_read(cs, &st) || st.nr_rotations) {
		fprintf(stderr, "nr_rotations is %lld: the idle slot of z was taken by a rotation\n",
			st.nr_rotations);
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
 * Read the first line of the stat file @path into @buf. Return a pointer
 * to the space before field @field, or NULL on failure. Fields are
 * numbered from 1, as in proc(5). @field is 3 or more.
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

/* Return the CPU time @pid used so far, in ms, or -1 when it is gone. */
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

/*
 * Pin @pid to CPUs @from to @to. @want is the expected errno, or 0 for a
 * success. Return 0 when the result is @want. Else print @what with the
 * result and return -1.
 */
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
 * A thread created while another process changes the affinity of its
 * creator. g has the limit 1. Two processes run in it:
 *  - M creates threads in a loop, one at a time, for 2 s.
 *  - S pins M to CPU 0 and to CPU 1 in turn, for the same 2 s.
 * A new thread copies the mask of its creator early in the fork, with no
 * lock. So it can start with the mask which M just left: the thread is
 * then pinned to one CPU, and M to the other.
 *
 * Checked:
 *  - M creates 100 threads or more. M joins each thread before it
 *    creates the next one. So a thread which is held and never placed
 *    stops M;
 *  - no new thread reads an mm_cid of 1 or more.
 */
static pid_t fa_main_tid;
static unsigned long fa_born, fa_bad_cid;

static void *fa_thread(void *arg)
{
	fa_born++;
	/* Unsigned: an ID left unset reads 0x80000000. */
	if (rseq_has_mm_cid() && rseq_field(thread_rseq(), RSEQ_MM_CID_OFFSET) >= 1)
		fa_bad_cid++;
	return NULL;
}

static int fork_affinity_run(const char *cgroup, void *arg)
{
	struct timespec now, end;
	cpu_set_t set[2];
	unsigned int i = 0;
	pthread_t t;
	pid_t s;

	fa_main_tid = getpid();
	clock_gettime(CLOCK_MONOTONIC, &end);
	end.tv_sec += 2;
	s = fork();
	if (s < 0)
		return 3;
	if (!s) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		CPU_ZERO(&set[0]);
		CPU_SET(0, &set[0]);
		CPU_ZERO(&set[1]);
		CPU_SET(1, &set[1]);
		do {
			sched_setaffinity(fa_main_tid, sizeof(set[0]), &set[i++ & 1]);
			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (ts_before(&now, &end));
		_exit(0);
	}
	do {
		if (!pthread_create(&t, NULL, fa_thread, NULL))
			pthread_join(t, NULL);
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (ts_before(&now, &end));
	waitpid(s, NULL, 0);
	if (fa_bad_cid) {
		fprintf(stderr, "%lu of %lu new threads read an mm_cid of 1 or more\n", fa_bad_cid,
			fa_born);
		return 1;
	}
	if (fa_born < 100) {
		fprintf(stderr, "the creator made %lu threads in 2 s\n", fa_born);
		return 2;
	}
	return 0;
}

static int test_concurrency_fork_affinity(const char *root)
{
	int ret = KSFT_FAIL;
	char *g;

	if (need_cpus(2))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_fork_aff");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	if (!cg_run(g, fork_affinity_run, NULL))
		ret = KSFT_PASS;
cleanup:
	if (g) {
		cg_write(g, "cpu.concurrency.max", "max");
		cg_destroy(g);
	}
	free(g);
	return ret;
}

/*
 * A cpuset write moves the tasks of a limited cgroup into its new CPUs,
 * also when one of the new CPUs is offline. CPU 2 is offline during the
 * test. Two trees:
 *  - g has the limit 1 and the cpuset 1-2. t1 spins in g. The test
 *    writes 2-4 to cpuset.cpus of g;
 *  - p has the limit 3 and two children. t2 spins in a (cpuset 1). w1,
 *    pinned to {5}, and w2, pinned to {3}, spin in b (cpuset 3-5). The
 *    test writes 2,5 to cpuset.cpus of a.
 * Checked:
 *  - both writes are accepted;
 *  - t1 is on CPU 3 or 4 within 2 s of the first write, and t2 on CPU 5
 *    within 2 s of the second;
 *  - the four spinners then each get CPU time within 2 s. t2 and w1 share
 *    CPU 5.
 * Skipped with fewer than 6 CPUs, or when CPU 2 cannot go offline.
 */
static int procs_stuck(const pid_t *pid, int n, int ms);

/*
 * A thread changes cgroup while another thread changes its affinity.
 *
 * Cgroups: p, a threaded domain with no limit, and its children a, with
 * cpu.concurrency.max 2, and b, with cpu.concurrency.max 1. One process
 * has three threads:
 *  - u sleeps in b, pinned to {1};
 *  - t spins, pinned to {1}. It starts in p for 640 rounds, then in a
 *    for 640 rounds;
 *  - the main thread, in p, pinned to {0}.
 * In each round t writes "0" to cgroup.threads of b, which moves t into
 * b. The main thread pins t to {2}. The pin starts from 10 us before the
 * move to 53 us after it. A thread which moves itself takes no
 * threadgroup rwsem, so that lock does not order the two. After each
 * round the main thread pins t to {1} again and moves it back.
 *
 * sched_setaffinity() reserves a slot in the cgroup which it finds t in,
 * and t may have left that cgroup when the slot is settled.
 *
 * Checked:
 *  - every move and every pin is accepted;
 *  - t ends its move within 5 s in every round. A thread which is held
 *    and never placed does not;
 *  - at the end, with t back in its first cgroup, nr_held of b is 0
 *    within 2 s, and cpu.concurrency.effective of b reads 1.
 * main() fails the case when the kernel warns while it runs.
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

/*
 * Return 1 if @tid is in the cgroup whose path ends with @name, 0 if
 * not, and -1 when it cannot be read.
 */
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
		struct timespec t0;
		int pinned, in_b;
		cpu_set_t set;

		sp->delay_ns = off < 0 ? -off : 0;
		__atomic_store_n(&sp->go, i + 1, __ATOMIC_RELEASE);
		spin_ns(off);
		pinned = pin_cpus(sp->tid, two);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		while (__atomic_load_n(&sp->done, __ATOMIC_ACQUIRE) != i + 1) {
			if (ns_since(&t0) > 5000000000LL) {
				fprintf(stderr, "t from %s: the move to b did not end within 5 s\n",
					sp->src == sp->a ? "a" : "p");
				return -1;
			}
		}
		in_b = thread_in(sp->tid, "b");
		if (in_b < 0 || sched_getaffinity(sp->tid, sizeof(set), &set))
			return -1;
		if (sp->moved || pinned) {
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
		fprintf(stderr, "a round did not end, or could not be undone\n");
	else if (bad)
		fprintf(stderr, "%d rounds of 640 had a move or a pin refused\n", bad);
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
	struct conc_stat st = { };
	int ret = KSFT_FAIL, i;

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
		ret = skip("no threaded cgroups");
		goto cleanup;
	}
	if (cg_write(sp.a, "cpu.concurrency.max", "2") || cg_write(sp.b, "cpu.concurrency.max", "1"))
		goto cleanup;
	if (cg_run(sp.p, self_move_threads, &sp))
		goto cleanup;
	for (i = 0; i < 200; i++) {
		if (conc_stat_read(sp.b, &st))
			goto cleanup;
		if (!st.nr_held)
			break;
		usleep(10000);
	}
	if (st.nr_held || cg_read_strcmp(sp.b, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "after the rounds, nr_held of b is %lld, or its effective value is not 1\n",
			st.nr_held);
		goto cleanup;
	}
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

/*
 * Return the CPU which thread @tid of @pid runs on or last ran on, or -1
 * when it cannot be read.
 */
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

/*
 * Return the thread of the calling process named @comm, or 0 when there
 * is none.
 */
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
 * Own a vhost-net device. Its worker wakes at each write to @kick. Pin
 * the worker to {0}. Then spin on CPU 1 with the mask {0,1}.
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
 * A vhost worker is a thread of the process which owns the device. The
 * kernel runs it, and it never returns to user space. It is still held
 * when it wakes up with no CPU within the limit of its cgroup. It is
 * held at the top of its loop, where it runs the work queued for a
 * return to user space.
 * x runs in g. It owns a vhost-net device and spins on CPU 1 with the
 * mask {0,1}. The worker sleeps, pinned to {0}. The limit 1 is then
 * written to g, and the one slot is on CPU 1. The harness writes 100
 * times to the kick eventfd, 2 ms apart. Each write wakes the worker,
 * which has no CPU it may use. Held, it lets the placement work move g
 * to CPU 0. At the end x and the worker must both be on CPU 0.
 * Skipped without /dev/vhost-net, or when the vhost workers are kernel
 * threads.
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
		ret = tid == -ESRCH ? skip("the vhost workers are kernel threads") :
				      skip("no vhost-net: %s", strerror(-tid));
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

/*
 * Wait up to about 5 s for *@v to reach @want. Return 0, or -1 on a
 * timeout.
 */
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

/*
 * Return the number of threads of the calling process, or -1 when it
 * cannot be read.
 */
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
 * The process x. In each round, pin itself to {2,3}. Start
 * TOGGLE_SPAWNERS spawner threads: they inherit the mask. Then unpin
 * itself. Once told to stop, reap them and wait until this thread is the
 * only one left. Then pin itself to {1}, record the result, and undo the
 * pin.
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
 * Sleep, then read mm_cid. Record a value of 1 or more when the limit
 * was 1 during the whole sample: @gen is odd and has not changed. Read
 * the ID unsigned: one left unset reads 0x80000000.
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
 * The process y. Start TOGGLE_CROWD sleeping threads, so that a walk
 * over the tasks of d takes time. Start TOGGLE_SAMPLERS threads which
 * sample their mm_cid. Then create and reap threads in a loop: they join
 * the mm while its bound changes.
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
 * Switch limits on and off while tasks fork. The tree is p/m/c and
 * p/m/d. p and m enable the cpu controller. c has the limit 2. d is
 * created before c: a walk visits the children in their order of
 * creation.
 *  - y runs in d. It has TOGGLE_CROWD sleeping threads, TOGGLE_SAMPLERS
 *    threads which read their mm_cid at each wakeup, and one thread
 *    which creates and reaps threads in a loop.
 *  - x runs in c. In each round it starts TOGGLE_SPAWNERS threads pinned
 *    to {2,3}. Each creates and reaps threads in a loop. A new thread
 *    registers its mask at fork.
 * In each of TOGGLE_ROUNDS rounds, the harness:
 *  - writes "max" to the limits of m and p, and lets x start its
 *    threads;
 *  - waits for a time which varies with the round, below 500 us;
 *  - writes a limit of 1: to m in the first half of the rounds, to p in
 *    the second half. The walk over the threads of d comes between the
 *    cgroup written and c. This leaves time for the forks in c to race
 *    with the change;
 *  - tells x to stop. x reaps its threads and pins itself to {1}.
 * Checked:
 *  - the pin to {1} is accepted in every round;
 *  - every round ends within 5 s. A round which does not end means that
 *    x is held and never placed;
 *  - y's samplers read mm_cid 0 while the limit 1 is in force.
 */
static int test_concurrency_toggle(const char *root)
{
	char *p = NULL, *m = NULL, *c = NULL, *d = NULL;
	int ret = KSFT_FAIL, r;
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
		if (sh->result) {
			fprintf(stderr, "round %d: the pin to {1}: %s\n", r, strerror(sh->result));
			goto cleanup;
		}
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

/* Return the first CPU of @cpu's SMT core, or -1 when it cannot be read. */
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
	/* core_of() of every online CPU, -1 otherwise */
	int core[CPU_SETSIZE];
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
 * Run inside the cgroup, which is limited to the number of cores. Start
 * one spinner per core, 20 ms apart. Let them settle for 500 ms, then
 * record their CPUs for 500 ms. Return non-zero if two of these CPUs
 * share a core, or if there are more CPUs than cores.
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
 * Slots go one per SMT core first. The limit is the number of cores.
 * Outside the cgroup, one task spins on one CPU of every core but the
 * first. The cgroup then starts one spinner per core. Once the first
 * spinner runs, no core is idle. The fork search then compares cores
 * with one idle CPU each, and the cgroup's own core looks the least
 * loaded. A second spinner on that core would leave a core unused. At
 * the end the spinners must run on as many cores as there are spinners.
 * Skipped when SMT is not active.
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
		ret = skip("%d cores, %d CPUs: spreading takes 2 to %d cores, fewer than the CPUs",
			   p->ncores, online, MAX_THREADS);
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

/*
 * Return 1 if process @pid is listed in cgroup.procs of @cg, 0 if not, and
 * -1 when the file cannot be read.
 */
static int cg_has_proc(const char *cg, pid_t pid)
{
	char path[PATH_MAX], line[32];
	int found = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/cgroup.procs", cg);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (!found && fgets(line, sizeof(line), f))
		found = atoi(line) == pid;
	fclose(f);
	return found;
}

/*
 * Wait until process @pid is in @cg. cg_run_nowait() returns before the
 * child has entered its cgroup. Return 0, or -1 if it is not there after
 * 2 s. The child enters the cgroup with its first system calls, so 2 s
 * only pass when it cannot enter.
 */
static int wait_in_cgroup(const char *cg, pid_t pid)
{
	int i;

	for (i = 0; i < 2000; i++) {
		if (cg_has_proc(cg, pid) == 1)
			return 0;
		usleep(1000);
	}
	fprintf(stderr, "process %d did not enter %s\n", (int)pid, cg);
	return -1;
}

/* Return the state letter of @pid in /proc/<pid>/stat, or 0. */
static char proc_state(pid_t pid)
{
	char path[64], buf[1024], *p;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	p = proc_stat_field(path, 3, buf, sizeof(buf));
	return p ? p[1] : 0;
}

/*
 * Wait until each of the @n processes of @pid has used more CPU time than
 * it had at the call. Return -1 when they all have. Return the index of
 * one which has not after @ms.
 */
static int procs_stuck(const pid_t *pid, int n, int ms)
{
	long before[MAX_THREADS];
	int i, t, stuck = -1;

	for (i = 0; i < n; i++)
		before[i] = proc_cpu_ms(pid[i]);
	for (t = 0; t < ms; t += 10) {
		usleep(10000);
		stuck = -1;
		for (i = 0; i < n && stuck < 0; i++) {
			if (proc_cpu_ms(pid[i]) <= before[i])
				stuck = i;
		}
		if (stuck < 0)
			break;
	}
	return stuck;
}

/*
 * Wait until the @n processes of @pid are all on one CPU of @allowed, for
 * three samples in a row, 10 ms apart. Return that CPU. Return -1 and
 * print where they are, with @when, if this has not happened after @ms.
 */
static int procs_on_one_cpu(const pid_t *pid, int n, const cpu_set_t *allowed, int ms,
			    const char *when)
{
	int i, t, cpu = -1, row = 0;

	for (t = 0; t < ms; t += 10) {
		cpu = stat_cpu(pid[0]);
		for (i = 1; i < n && cpu >= 0; i++) {
			if (stat_cpu(pid[i]) != cpu)
				cpu = -1;
		}
		row = cpu >= 0 && CPU_ISSET(cpu, allowed) ? row + 1 : 0;
		if (row == 3)
			return cpu;
		usleep(10000);
	}
	fprintf(stderr, "%s: the tasks are not on one allowed CPU after %d ms:", when, ms);
	for (i = 0; i < n; i++)
		fprintf(stderr, " %d", stat_cpu(pid[i]));
	fprintf(stderr, "\n");
	return -1;
}

/* A task of test_concurrency_rt_setmove(). */
struct rt_sm_task {
	int wake;	/* if >= 0, block on a read of this pipe first */
	int spin_us;	/* then spin that long */
	int nap_us;	/* and sleep that long, for ever */
};

static int rt_sm_fn(const char *cgroup, void *arg)
{
	struct rt_sm_task *t = arg;
	char c;

	if (t->wake >= 0 && read(t->wake, &c, 1) != 1)
		return 1;
	for (;;) {
		spin_ns(t->spin_us * 1000LL);
		usleep(t->nap_us);
	}
	return 0;
}

/*
 * Keep the kernel from placing held tasks, for a test which needs a task
 * to stay held.
 *
 * A held fair task is placed by a work item on an unbound workqueue. The
 * stall keeps the kworkers of the unbound workqueues off the CPU:
 *  - /sys/devices/virtual/workqueue/cpumask restricts them to one CPU;
 *  - the fair server of that CPU gets a runtime of 0, so it gives the
 *    fair tasks no time while a real-time task runs;
 *  - a SCHED_FIFO task spins on that CPU.
 * Every other unbound work item waits too, so a stall must be short.
 *
 * A fair task on that CPU does not run until the stall ends. The test
 * itself must not be one, or the stall would never end. So the caller is
 * kept off that CPU during the stall, and the spinner ends by itself
 * after STALL_MAX_MS.
 */
struct stall {
	pid_t hog;
	int moved;		/* the caller's mask was changed */
	cpu_set_t caller_mask;
	char wq_mask[256];
	char fs_runtime[64];
	char fs_path[128];
};

#define STALL_MAX_MS	2000

#define WQ_CPUMASK	"/sys/devices/virtual/workqueue/cpumask"
#define FAIR_SERVER	"/sys/kernel/debug/sched/fair_server"

/* Read the file @path into @buf as a string. Return 0, or -1. */
static int file_read(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return 0;
}

/* Write the string @val to the file @path. Return 0, or -1. */
static int file_write(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, val, strlen(val));
	close(fd);
	return n == (ssize_t)strlen(val) ? 0 : -1;
}

/*
 * End a stall: end the spinner, restore the two files and the mask of the
 * caller. The caller may call it twice.
 */
static void stall_stop(struct stall *st)
{
	if (st->hog > 0) {
		kill(st->hog, SIGKILL);
		waitpid(st->hog, NULL, 0);
		st->hog = -1;
	}
	if (st->moved)
		sched_setaffinity(0, sizeof(st->caller_mask), &st->caller_mask);
	st->moved = 0;
	if (st->fs_runtime[0])
		file_write(st->fs_path, st->fs_runtime);
	if (st->wq_mask[0])
		file_write(WQ_CPUMASK, st->wq_mask);
	st->fs_runtime[0] = 0;
	st->wq_mask[0] = 0;
}

/*
 * Start a stall on @cpu, which must be below 32. The caller gives its
 * other tasks masks without @cpu, or lets them be held: a fair task gets
 * no time on @cpu. Return 0. Return -1 if the machine lacks one of the
 * files, the privilege for them, or SCHED_FIFO. Nothing is left changed
 * then.
 */
static int stall_start(struct stall *st, int cpu)
{
	struct sched_param prio = { .sched_priority = 50 };
	struct timespec t0;
	char buf[64];
	cpu_set_t set;
	int ready[2];

	memset(st, 0, sizeof(*st));
	st->hog = -1;
	if (sched_getaffinity(0, sizeof(st->caller_mask), &st->caller_mask))
		return -1;
	set = st->caller_mask;
	CPU_CLR(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return -1;
	st->moved = 1;
	snprintf(st->fs_path, sizeof(st->fs_path), FAIR_SERVER "/cpu%d/runtime", cpu);
	if (file_read(WQ_CPUMASK, st->wq_mask, sizeof(st->wq_mask))) {
		stall_stop(st);
		return -1;
	}
	if (file_read(st->fs_path, st->fs_runtime, sizeof(st->fs_runtime))) {
		st->wq_mask[0] = 0;
		stall_stop(st);
		return -1;
	}
	snprintf(buf, sizeof(buf), "%x\n", 1U << cpu);
	if (file_write(WQ_CPUMASK, buf) || file_write(st->fs_path, "0\n") || pipe(ready)) {
		stall_stop(st);
		return -1;
	}
	st->hog = fork();
	if (!st->hog) {
		close(ready[0]);
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) ||
		    sched_setscheduler(0, SCHED_FIFO, &prio))
			_exit(1);
		if (write(ready[1], "r", 1) != 1)
			_exit(1);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		while (ns_since(&t0) < STALL_MAX_MS * 1000000LL)
			;
		_exit(0);
	}
	close(ready[1]);
	/* One byte when it spins. The end of the pipe when it could not. */
	if (st->hog < 0 || read(ready[0], buf, 1) != 1) {
		close(ready[0]);
		stall_stop(st);
		return -1;
	}
	close(ready[0]);
	return 0;
}

/* Return a CPU below 32 which is online and none of @a, @b and @c, or -1. */
static int cpu_other_than(int a, int b, int c)
{
	int cpu, n = MIN(get_nprocs_conf(), 32);
	cpu_set_t online;

	if (sched_getaffinity(0, sizeof(online), &online))
		return -1;
	/* From the last one down: CPU 0 often cannot go offline. */
	for (cpu = n - 1; cpu >= 0; cpu--) {
		if (cpu != a && cpu != b && cpu != c && CPU_ISSET(cpu, &online))
			return cpu;
	}
	return -1;
}

/*
 * Return true if @pid is runnable and uses no CPU time for @ms: it is
 * held. A task which runs for 10 ms shows it in its CPU time.
 */
static bool proc_held_for(pid_t pid, int ms)
{
	long before = proc_cpu_ms(pid);
	int t;

	for (t = 0; t < ms; t += 10) {
		usleep(10000);
		if (proc_state(pid) != 'R' || proc_cpu_ms(pid) != before)
			return false;
	}
	return true;
}

/* The tasks and the stall of the two cases with a held task. */
struct held_setup {
	char *g, *out;
	pid_t a, b;
	int x, y;	/* the CPUs of a and b before b moves into g */
	struct stall st;
	int stalled;
};

static void held_cleanup(struct held_setup *hs)
{
	if (hs->stalled)
		stall_stop(&hs->st);
	if (hs->a > 0) {
		kill(hs->a, SIGKILL);
		waitpid(hs->a, NULL, 0);
	}
	if (hs->b > 0) {
		kill(hs->b, SIGKILL);
		waitpid(hs->b, NULL, 0);
	}
	if (hs->g) {
		cg_write(hs->g, "cpu.concurrency.max", "max");
		cg_destroy(hs->g);
	}
	if (hs->out)
		cg_destroy(hs->out);
	free(hs->g);
	free(hs->out);
}

/*
 * Build the start of the two cases with a held task. g has the limit 1,
 * and a spins in it, on CPU x. b spins in "out", outside g. It is pinned
 * to another CPU, y. A stall starts on a third CPU. b is then moved into g.
 * g does not use y, so b finds no CPU within the limit and is held: it
 * stays runnable and uses no CPU time. Only the placement work can move g
 * to y, and the stall keeps it from running.
 *
 * Return KSFT_PASS when b has been held for 100 ms. Return KSFT_SKIP if
 * the stall cannot start, or if b runs within these 100 ms: this machine
 * then places b in spite of the stall. Return KSFT_FAIL on an error.
 * The caller calls held_cleanup() in every case.
 */
static int held_setup(struct held_setup *hs, const char *root, const char *name)
{
	char outname[64];
	int k;

	memset(hs, 0, sizeof(*hs));
	hs->a = -1;
	hs->b = -1;
	snprintf(outname, sizeof(outname), "%s_out", name);
	hs->g = cg_name(root, name);
	hs->out = cg_name(root, outname);
	if (!hs->g || !hs->out || cg_create(hs->g) || cg_create(hs->out) ||
	    cg_write(hs->g, "cpu.concurrency.max", "1"))
		return KSFT_FAIL;
	hs->a = cg_run_nowait(hs->g, busy_forever, NULL);
	hs->b = cg_run_nowait(hs->out, busy_forever, NULL);
	if (hs->a < 0 || hs->b < 0 || wait_in_cgroup(hs->g, hs->a) ||
	    wait_in_cgroup(hs->out, hs->b))
		return KSFT_FAIL;
	/* Let a settle on its CPU. */
	usleep(100000);
	hs->x = stat_cpu(hs->a);
	hs->y = cpu_other_than(hs->x, -1, -1);
	k = cpu_other_than(hs->x, hs->y, -1);
	if (hs->x < 0 || hs->y < 0 || k < 0)
		return KSFT_FAIL;
	if (expect_pin(hs->b, hs->y, hs->y, 0, "b pinned to a CPU which g does not use"))
		return KSFT_FAIL;
	if (stall_start(&hs->st, k))
		return skip("the placement work cannot be stalled: no %s, no %s, or no SCHED_FIFO",
			    WQ_CPUMASK, FAIR_SERVER);
	hs->stalled = 1;
	if (cg_enter(hs->g, hs->b)) {
		fprintf(stderr, "b cannot move into g\n");
		return KSFT_FAIL;
	}
	if (!proc_held_for(hs->b, 100))
		return skip("b was placed in spite of the stall");
	return KSFT_PASS;
}

/*
 * SIGKILL ends a hold. held_setup() leaves b held, with the placement
 * work stalled. The steps:
 *  - b gets SIGKILL;
 *  - the test waits 200 ms for b to die, and prints whether it did. The
 *    kernel lets a held task go from the placement work, which the stall
 *    delays, so this is not checked;
 *  - the stall ends.
 * b must then be reaped within 1 s. The work runs as soon as its kworker
 * gets the CPU, and lets b go without a slot. a must still get CPU time,
 * and cpu.concurrency.effective of g must read "1 max".
 * Skipped with fewer than 3 CPUs, or as held_setup() says.
 */
static int test_concurrency_held_kill(const char *root)
{
	struct held_setup hs;
	int ret, i, died = 0;

	if (need_cpus(3))
		return KSFT_SKIP;
	ret = held_setup(&hs, root, "cc_test_held_kill");
	if (ret != KSFT_PASS)
		goto cleanup;
	ret = KSFT_FAIL;
	kill(hs.b, SIGKILL);
	for (i = 0; i < 20 && !died; i++) {
		died = waitpid(hs.b, NULL, WNOHANG) == hs.b;
		if (!died)
			usleep(10000);
	}
	ksft_print_msg("a held task with SIGKILL pending %s while the placement work was stalled\n",
		       died ? "died" : "did not die in 200 ms");
	stall_stop(&hs.st);
	hs.stalled = 0;
	for (i = 0; i < 100 && !died; i++) {
		died = waitpid(hs.b, NULL, WNOHANG) == hs.b;
		if (!died)
			usleep(10000);
	}
	if (!died) {
		fprintf(stderr, "the held task did not die within 1 s of the end of the stall\n");
		goto cleanup;
	}
	hs.b = -1;
	if (procs_stuck(&hs.a, 1, 2000) >= 0) {
		fprintf(stderr, "a got no CPU time in 2 s after the held task died\n");
		goto cleanup;
	}
	if (cg_read_strcmp(hs.g, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "the effective limit is not 1 after the held task died\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	held_cleanup(&hs);
	return ret;
}

#define RT_SM_SPINNERS	3

/*
 * Check the tasks of test_concurrency_rt_setmove() after the step @when.
 * The @n tasks of @pid must all be on @cpu within @ms, and each must get
 * CPU time within 3 s. A fair task of g only gets the time which the fair
 * server takes from the real-time tasks: 50 ms in a second, by default.
 * cpu.concurrency.effective of @g must read "1 max". Return 0 if all this
 * holds.
 */
static int rt_sm_check(const char *g, const pid_t *pid, int n, int cpu, int ms, const char *when)
{
	cpu_set_t allowed;
	int i;

	CPU_ZERO(&allowed);
	CPU_SET(cpu, &allowed);
	if (procs_on_one_cpu(pid, n, &allowed, ms, when) < 0)
		return -1;
	i = procs_stuck(pid, n, 3000);
	if (i >= 0) {
		fprintf(stderr, "%s: task %d got no CPU time in 3 s\n", when, i);
		return -1;
	}
	if (cg_read_strcmp(g, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "%s: the effective limit is not 1\n", when);
		return -1;
	}
	return 0;
}

/*
 * A set-move takes the queued real-time tasks along, and a real-time task
 * which finds no CPU within the limit makes the set-move itself.
 *
 * g has the limit 1. r1, r2 and r3 are SCHED_FIFO tasks of g which can run
 * on every CPU. Each spins 2 ms and sleeps 2 ms, for ever. They share one
 * CPU, x, and ask for more than its time. So at any time one of them is
 * queued behind another. The steps:
 *  - f, a fair task of g, blocks on a pipe. The test pins it to {y}, a
 *    CPU other than x, while it sleeps. g does not use y.
 *    sched_setaffinity() must move g to y, so that f finds a CPU when it
 *    wakes up. The spinners must be on y within 2 s. The test then wakes
 *    f, which must run on y. f is then killed;
 *  - q is a SCHED_FIFO task in "out", outside g. It spins 1 ms and
 *    sleeps 3 ms. It is pinned to {z}, a CPU other than y. A stall
 *    starts on a fourth CPU, see stall_start(): the placement work
 *    cannot run. The test moves q into g. g does not use z, so q finds no
 *    CPU within the
 *    limit. A fair task would be held until the stall ends. q must move g
 *    to z by itself, at its return to user space. The spinners and q must
 *    be on z within 500 ms, while the stall is on. A set-move takes a few
 *    milliseconds.
 * After each step rt_sm_check() must hold. A real-time task which a
 * set-move left behind shows on another CPU. A task held for good gets no
 * CPU time.
 *
 * The spinners become SCHED_FIFO before the limit is written. The case is
 * skipped if that fails, or with fewer than 4 CPUs. Under the limit a
 * refused SCHED_FIFO is a failure. If the stall cannot start, the second
 * step runs without it and a note is printed. The step then passes also
 * when the placement work moves g.
 */
static int test_concurrency_rt_setmove(const char *root)
{
	struct sched_param prio = { .sched_priority = 1 };
	struct rt_sm_task spinner = { .wake = -1, .spin_us = 2000, .nap_us = 2000 };
	struct rt_sm_task sleeper = { .spin_us = 1000, .nap_us = 3000 };
	struct rt_sm_task mover = { .wake = -1, .spin_us = 1000, .nap_us = 3000 };
	int ret = KSFT_FAIL, wake[2] = { -1, -1 }, stalled = 0, i, x, y, z, k;
	pid_t pid[RT_SM_SPINNERS + 1], *last = &pid[RT_SM_SPINNERS];
	char *g = NULL, *out = NULL;
	struct stall st;
	cpu_set_t all;

	for (i = 0; i <= RT_SM_SPINNERS; i++)
		pid[i] = -1;
	if (need_cpus(4))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_rtsm");
	out = cg_name(root, "cc_test_rtsm_out");
	if (!g || !out || cg_create(g) || cg_create(out) || pipe(wake))
		goto cleanup;
	for (i = 0; i < RT_SM_SPINNERS; i++) {
		pid[i] = cg_run_nowait(g, rt_sm_fn, &spinner);
		if (pid[i] < 0 || wait_in_cgroup(g, pid[i]))
			goto cleanup;
		if (sched_setscheduler(pid[i], SCHED_FIFO, &prio)) {
			ret = skip("no SCHED_FIFO task without a limit: %s", strerror(errno));
			goto cleanup;
		}
	}
	if (cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	stress_all_cpus(&all);
	x = procs_on_one_cpu(pid, RT_SM_SPINNERS, &all, 2000, "the spinners under the limit 1");
	if (x < 0)
		goto cleanup;

	/* The fair sleeper: sched_setaffinity() moves g. */
	y = cpu_other_than(x, -1, -1);
	sleeper.wake = wake[0];
	*last = cg_run_nowait(g, rt_sm_fn, &sleeper);
	if (*last < 0 || wait_in_cgroup(g, *last))
		goto cleanup;
	/* It blocks in read() right after it entered g: 2 s is ample. */
	for (i = 0; i < 2000 && proc_state(*last) != 'S'; i++)
		usleep(1000);
	if (proc_state(*last) != 'S' ||
	    expect_pin(*last, y, y, 0, "f pinned to a CPU which g does not use") ||
	    rt_sm_check(g, pid, RT_SM_SPINNERS, y, 2000, "after the pin of the fair sleeper"))
		goto cleanup;
	if (write(wake[1], "w", 1) != 1 ||
	    rt_sm_check(g, pid, RT_SM_SPINNERS + 1, y, 2000, "after the fair sleeper woke up"))
		goto cleanup;
	kill(*last, SIGKILL);
	waitpid(*last, NULL, 0);
	*last = -1;

	/* The real-time task: it moves g itself. */
	z = cpu_other_than(y, -1, -1);
	k = cpu_other_than(y, z, -1);
	*last = cg_run_nowait(out, rt_sm_fn, &mover);
	if (*last < 0 || wait_in_cgroup(out, *last) ||
	    expect_pin(*last, z, z, 0, "q pinned to a CPU which g does not use"))
		goto cleanup;
	if (sched_setscheduler(*last, SCHED_FIFO, &prio)) {
		perror("q made SCHED_FIFO outside the limit");
		goto cleanup;
	}
	if (stall_start(&st, k))
		ksft_print_msg("the placement work cannot be stalled: q may be placed by it\n");
	else
		stalled = 1;
	if (cg_enter(g, *last)) {
		fprintf(stderr, "q cannot move into g\n");
		goto cleanup;
	}
	CPU_ZERO(&all);
	CPU_SET(z, &all);
	if (procs_on_one_cpu(pid, RT_SM_SPINNERS + 1, &all, 500,
			     "after the real-time task moved into g") < 0)
		goto cleanup;
	if (stalled)
		stall_stop(&st);
	stalled = 0;
	if (rt_sm_check(g, pid, RT_SM_SPINNERS + 1, z, 2000,
			"after the real-time task moved into g"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (stalled)
		stall_stop(&st);
	for (i = 0; i <= RT_SM_SPINNERS; i++) {
		if (pid[i] > 0) {
			kill(pid[i], SIGKILL);
			waitpid(pid[i], NULL, 0);
		}
	}
	for (i = 0; i < 2; i++) {
		if (wake[i] >= 0)
			close(wake[i]);
	}
	if (g) {
		cg_write(g, "cpu.concurrency.max", "max");
		cg_destroy(g);
	}
	if (out)
		cg_destroy(out);
	free(g);
	free(out);
	return ret;
}

struct rot_mask {
	int lo, hi;
};

/*
 * Fork a process which enters @out and pins itself to CPUs @lo to @hi, or
 * to every CPU when @lo is negative. It then calls clone3() with
 * CLONE_INTO_CGROUP into @cg, and waits for its child. The child runs task
 * @task of @r, which keeps the mask of its parent. Return the pid of the
 * process, or -1.
 *
 * A failure shows as the setup error of the task, see rot_wait_ready().
 * It is the errno of clone3(), or ECHILD when the process could not get
 * that far.
 */
static pid_t rot_clone_into(struct rot *r, const char *out, const char *cg, int lo, int hi,
			    int task)
{
	struct rot_proc p = { .r = r, .first = task, .n = 1 };
	int i, fd, err = 0;
	cpu_set_t set;
	pid_t f, c = -1;

	f = fork();
	if (f)
		return f;
	CPU_ZERO(&set);
	for (i = lo < 0 ? 0 : lo; i <= (lo < 0 ? CPU_SETSIZE - 1 : hi); i++)
		CPU_SET(i, &set);
	fd = open(cg, O_RDONLY | O_DIRECTORY);
	if (cg_enter_current(out) || sched_setaffinity(0, sizeof(set), &set) || fd < 0) {
		err = ECHILD;
	} else {
		c = clone_into_cgroup(fd);
		if (c < 0)
			err = errno;
	}
	if (!c)
		_exit(rot_proc_fn(cg, &p));
	if (err) {
		r->t[task].err = err;
		__atomic_store_n(&r->t[task].ready, 1, __ATOMIC_RELEASE);
		_exit(1);
	}
	waitpid(c, NULL, 0);
	_exit(0);
}

#define CLONE_INTO_PARENTS	4

/*
 * clone3(2) with CLONE_INTO_CGROUP from a pinned parent into a cgroup
 * whose slot is in use.
 *
 * Cgroups: g and "out". g has cpu.concurrency.max 1 and cpuset.cpus 0-2.
 * out has no limit. x spins in g, pinned to {0}. Four parents run in out,
 * one after the other. Each pins itself and clones a child into g, see
 * rot_clone_into(). The child keeps the mask of its parent, cut to the
 * CPUs of g. It spins until the end of the test. The masks of the
 * parents:
 *  - {1};
 *  - {0,1};
 *  - {2,3}, which the cpuset of g cuts to {2};
 *  - every CPU.
 * The test then measures for 3 s, and writes 0-3 and 0-2 to cpuset.cpus
 * of g, then 2 and 1 to its cpu.concurrency.max.
 *
 * Checked:
 *  - every clone3() is accepted;
 *  - x and the four children each run for 4% of the 3 s or more. The
 *    fair share is 20%;
 *  - together they run for one CPU at most. rot_window() allows 4% more;
 *  - the four writes are accepted.
 * Skipped with fewer than 4 CPUs, or without clone3(2).
 */
static int test_concurrency_clone_into(const char *root)
{
	static const struct rot_mask parent[CLONE_INTO_PARENTS] = {
		{ 1, 1 }, { 0, 1 }, { 2, 3 }, { -1, -1 },
	};
	pid_t x = -1, helper[CLONE_INTO_PARENTS];
	char *g = NULL, *out = NULL;
	struct rot *r = NULL;
	int ret = KSFT_FAIL, fd, i;

	for (i = 0; i < CLONE_INTO_PARENTS; i++)
		helper[i] = -1;
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
		ret = skip("no clone3(2)");
		goto cleanup;
	}
	if (x > 0)
		waitpid(x, NULL, 0);
	x = -1;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	rot_pinned(r, 0, 0, 0);
	x = rot_start(r, g, 0, 1);
	if (x < 0 || rot_wait_ready(r, 0, 1))
		goto cleanup;
	rot_go(r, 60000);
	for (i = 0; i < CLONE_INTO_PARENTS; i++) {
		helper[i] = rot_clone_into(r, out, g, parent[i].lo, parent[i].hi, i + 1);
		if (helper[i] < 0 || rot_wait_ready(r, i + 1, 1)) {
			fprintf(stderr, "the clone from parent %d into g failed\n", i);
			goto cleanup;
		}
	}
	usleep(100000);
	if (rot_window(r, 0, CLONE_INTO_PARENTS + 1, 3000, 4, 0, 104, "x and four clones in g"))
		goto cleanup;
	if (cg_write(g, "cpuset.cpus", "0-3") || cg_write(g, "cpuset.cpus", "0-2") ||
	    cg_write(g, "cpu.concurrency.max", "2") || cg_write(g, "cpu.concurrency.max", "1")) {
		fprintf(stderr, "writes to g after the clones failed\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (r) {
		/* A child first: its parent then ends by itself. */
		for (i = 0; i < CLONE_INTO_PARENTS; i++) {
			if (helper[i] > 0 && r->t[i + 1].pid > 0)
				kill(r->t[i + 1].pid, SIGKILL);
		}
		rot_end(r, helper, CLONE_INTO_PARENTS);
		rot_end(r, &x, 1);
		munmap(r, sizeof(*r));
	}
	if (g) {
		cg_write(g, "cpu.concurrency.max", "max");
		cg_destroy(g);
	}
	if (out)
		cg_destroy(out);
	free(out);
	free(g);
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
	T(test_concurrency_move),
	T(test_concurrency_hard),
	T(test_concurrency_sleepers),
	T(test_concurrency_late_child),
	T(test_concurrency_child_slot),
	T(test_concurrency_fork_affinity),
	T(test_concurrency_clone_into),
	T(test_concurrency_self_move),
	T(test_concurrency_vhost),
	T(test_concurrency_toggle),
	T(test_concurrency_smt_spread),
	T(test_concurrency_rt_setmove),
	T(test_concurrency_held_kill),
};

#undef T

int main(int argc, char *argv[])
{
	struct kernel_state ks;
	char root[PATH_MAX];
	int i, r, plan = 0;
	char *probe;

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

	cpus_record_online();
	/* An argument selects the tests whose name contains it. */
	for (i = 0; i < ARRAY_SIZE(tests); i++)
		plan += argc < 2 || strstr(tests[i].name, argv[1]);
	ksft_set_plan(plan);
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		if (argc > 1 && !strstr(tests[i].name, argv[1]))
			continue;
		/*
		 * A case fails when the kernel warns while it runs, whatever
		 * the case itself found.
		 */
		kernel_state_record(&ks);
		r = tests[i].fn(root);
		if (kernel_state_check(&ks, tests[i].name))
			r = KSFT_FAIL;
		switch (r) {
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
		if (cpus_restore())
			ksft_print_msg("%s left a CPU offline that does not come back\n",
				       tests[i].name);
	}

	ksft_finished();
}
