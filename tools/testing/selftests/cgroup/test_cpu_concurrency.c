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

/*
 * Wait until tasks @first to @first + @n - 1 of @r ended. Return -1. Return
 * the index of a task which has not ended 3 s after the measured time. A
 * task ends as soon as it runs after that time. So it only takes 3 s when
 * the task does not run.
 */
static int rot_wait_done(struct rot *r, int first, int n)
{
	int i;

	for (i = first; i < first + n; i++) {
		while (!__atomic_load_n(&r->t[i].done, __ATOMIC_ACQUIRE)) {
			if (mono_ns() > r->end_ns + 3000000000LL)
				return i;
			usleep(1000);
		}
	}
	return -1;
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

/* Return the share of the measured time which task @i of @r ran, in percent. */
static int rot_share(struct rot *r, int i)
{
	return r->t[i].run_ns * 100 / (r->end_ns - r->t0_ns);
}

/*
 * Return 0 if no task from @first to @first + @n - 1 of @r read an mm_cid
 * at or above @limit. Else print the task, with @what, and return -1.
 */
static int rot_check_ids(struct rot *r, int first, int n, unsigned int limit, const char *what)
{
	int i;

	for (i = first; i < first + n; i++) {
		if (rseq_has_mm_cid() && r->t[i].max_cid >= limit) {
			fprintf(stderr, "%s: task %d read mm_cid %u, limit %u\n", what, i,
				r->t[i].max_cid, limit);
			return -1;
		}
	}
	return 0;
}

/* Return the median of the wakeup latencies of periodic task @i of @r, in us. */
static int rot_lat_median_us(struct rot *r, int i)
{
	struct rot_task *t = &r->t[i];
	int sorted[ROT_LATS], n = t->nlat, a, b;

	if (!n)
		return -1;
	memcpy(sorted, t->lat_us, n * sizeof(int));
	for (a = 1; a < n; a++) {
		int v = sorted[a];

		for (b = a; b > 0 && sorted[b - 1] > v; b--)
			sorted[b] = sorted[b - 1];
		sorted[b] = v;
	}
	return sorted[n / 2];
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

#define ROT_PERIOD_FILE	"/sys/kernel/debug/sched/conc_rotation_ms"

/*
 * Return the rotation period in ms. Without the debugfs file, return the
 * default of the kernel, 100.
 */
static int rot_period_ms(void)
{
	char buf[32];
	int fd, ms = 100;
	ssize_t n;

	fd = open(ROT_PERIOD_FILE, O_RDONLY);
	if (fd < 0)
		return ms;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n > 0) {
		buf[n] = 0;
		ms = atoi(buf);
	}
	return ms > 0 ? ms : 100;
}

/*
 * Wait until cpu.concurrency.effective of @cg reads @want. Return 0, or -1
 * after @ms.
 */
static int wait_effective(const char *cg, const char *want, int ms)
{
	int t;

	for (t = 0; t < ms; t += 5) {
		if (!cg_read_strcmp(cg, "cpu.concurrency.effective", want))
			return 0;
		usleep(5000);
	}
	return cg_read_strcmp(cg, "cpu.concurrency.effective", want) ? -1 : 0;
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
 * test_concurrency_hotplug() runs this in a cgroup limited to 1. Four
 * spinners start. One has the mask "all but CPU 0", since CPU 0 cannot
 * go offline. A CPU which the spinners use goes offline for 300 ms and
 * comes back. The cgroup must move to another CPU as a whole. The
 * effective value must stay 1 and no spinner may read an mm_cid above 0.
 * While the CPU is offline, each spinner must take samples, and none on
 * that CPU. One such sample is allowed for each spinner: it may have read
 * its CPU just before the unplug moved it.
 * Skipped when the CPU cannot go offline through sysfs.
 */

static int hotplug_under_load(const char *cgroup, void *arg)
{
	struct spinner s[4] = { };
	unsigned long before[4];
	cpu_set_t not0;
	int i, cpu = -1, ret = 0;

	for (i = 0; i < 4; i++) {
		clock_gettime(CLOCK_MONOTONIC, &s[i].until);
		s[i].until.tv_sec += 2;
		s[i].max_mm_cid = -1;
		if (pthread_create(&s[i].thread, NULL, spin, &s[i]))
			return 1;
	}
	/*
	 * CPU 0 cannot go offline. Give one spinner a mask without it, so
	 * that the cgroup does not stay on CPU 0.
	 */
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
		ret = skip("CPU %d cannot go offline", cpu);
		goto join;
	}
	for (i = 0; i < 4; i++) {
		before[i] = __atomic_load_n(&s[i].samples, __ATOMIC_RELAXED);
		__atomic_store_n(&s[i].avoid, cpu + 1, __ATOMIC_RELAXED);
	}
	usleep(300000);
	if (cg_read_strcmp(cgroup, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "effective limit changed across hotplug\n");
		ret = 3;
	}
	for (i = 0; i < 4 && !ret; i++) {
		unsigned long hits = __atomic_load_n(&s[i].avoid_hits, __ATOMIC_RELAXED);

		if (__atomic_load_n(&s[i].samples, __ATOMIC_RELAXED) == before[i]) {
			fprintf(stderr, "spinner %d did not run with CPU %d offline\n", i, cpu);
			ret = 6;
		} else if (hits > 1) {
			fprintf(stderr, "spinner %d took %lu samples on CPU %d, which is offline\n",
				i, hits, cpu);
			ret = 7;
		}
		__atomic_store_n(&s[i].avoid, 0, __ATOMIC_RELAXED);
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
		/* 4 is KSFT_SKIP: the caller would report a skip. */
		return 5;
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
 * One process has threads in two cgroups. a and b are threaded children
 * of an unlimited parent. Each has the limit 1. Four threads spin in a.
 * Two of them are then moved to b and pinned to one CPU which the other
 * two have not used. The process must have run on two CPUs at least. No
 * thread may read an mm_cid of 2 or more: 2 is the sum of the two limits.
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
	 * Move two of the four threads to the other cgroup. Two limited
	 * cgroups may share a CPU. To make the process use two CPUs, pin
	 * these two threads to a CPU which the other two have not used.
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
		ret = skip("no threaded cgroups");
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
 * A sleeper confined by its cpuset, with spinners confined to the other
 * CPUs. Nothing here calls sched_setaffinity(2).
 *
 * Cgroups: the parent, with cpu.concurrency.max 2, and its threaded
 * children a and b. The cpuset of a is the last CPU. The cpuset of b is
 * every other CPU. One process has four threads, which run for 1 s:
 *  - the sleeper stays in a. It sleeps 1 ms after each sample;
 *  - three spinners move to b.
 * Two slots serve them: one on the last CPU for the sleeper, and one for
 * the spinners.
 *
 * Checked at the end:
 *  - no thread read an mm_cid of 2 or more;
 *  - the effective value of the parent still reads 2;
 *  - the sleeper ran on the last CPU alone, and took SLEEPER_SAMPLES
 *    samples at least, see sleepers_and_spinners();
 *  - nr_rotations of the parent is still 0.
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
	long long max;
	int i;

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

	max = max_mm_cid_seen(s, 4);
	if (rseq_has_mm_cid() && max >= 2) {
		fprintf(stderr, "mm_cid %lld seen, limit 2\n", max);
		return 4;
	}
	if (s[0].samples < SLEEPER_SAMPLES || !spinner_only_on(&s[0], get_nprocs() - 1)) {
		fprintf(stderr, "the sleeper took %lu samples, on the last CPU only: %s\n",
			s[0].samples, spinner_only_on(&s[0], get_nprocs() - 1) ? "yes" : "no");
		return 5;
	}
	return 0;
}

static int test_concurrency_cpuset_class(const char *root)
{
	struct cpuset_class_param param = { };
	char *parent = NULL, *a = NULL;
	struct conc_stat st = { };
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
		ret = skip("no threaded cgroups");
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
	if (conc_stat_read(parent, &st) || st.nr_rotations) {
		fprintf(stderr, "nr_rotations is %lld, the threads fit in two slots\n",
			st.nr_rotations);
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

/*
 * Test the concurrency ID namespace under a limit.
 *
 * Cgroups, by their name in struct ns_cid_param:
 *
 *   base            child_ceiling 4
 *   `- outer        child_ceiling 3
 *      |- parent    ceiling 3, cpu.concurrency.max 2
 *      |  |- a      every process starts here
 *      |  `- in
 *      `- out       ceiling 3, no limit
 *
 * Processes:
 *  - The creator runs ns_cid_run(). It must be in the namespace of init,
 *    and it stays there.
 *  - The helper is forked by the creator before unshare(2). It is
 *    single-threaded and in the namespace of the creator.
 *  - Members 0 and 1 are forked by the creator after
 *    unshare(CLONE_NEWCID). Each has NS_CID_THREADS threads which read
 *    ns_cid in a loop.
 *
 * Steps, driven by the creator:
 *  1. unshare(CLONE_NEWCID). ns/cid of the creator is still the one of
 *     init. ns/cid_for_children is new. PR_GET_NS_CID_MAX of the creator
 *     is at least the number of online CPUs.
 *  2. Fork the members. Their ns/cid is the new namespace. Each reads the
 *     maximum 3. That is the ceiling of "parent", not the 4 written on
 *     "base".
 *  3. Move member 1 to "out". The write fails with EBUSY.
 *  4. Move member 1 to "in" and back to "a". Both writes succeed.
 *  5. clone3(CLONE_NEWCID | CLONE_INTO_CGROUP) one child into "out" and
 *     one into "in". Each child is in a namespace of its own. Each reads
 *     the maximum 3 and one ns_cid below 3. A clone3(2) which fails with
 *     ENOSYS is accepted.
 *  6. Move the helper to "out". Its setns(2) to the namespace of member 0
 *     fails with EBUSY.
 *  7. Move the helper to "a". Its setns(2) succeeds. Its next
 *     NS_CID_JOIN_SAMPLES reads of ns_cid are below 2.
 *  8. Move the helper to "in" and back to "a". Its reads are still below
 *     2. It reads the maximum 3.
 *  9. The helper starts NS_CID_THREADS threads. Its setns(2) fails with
 *     EUSERS.
 * 10. Write 1 to cpu.concurrency.max of "parent" and wait for
 *     cpu.concurrency.effective to show it. Every sample of the members
 *     in the next phase is 0. The helper still reads the maximum 3.
 *
 * In all steps, no thread of a member reads an ns_cid at or above the
 * limit 2. The test does not compare the IDs of two running threads.
 */
#ifndef CLONE_NEWCID
#define CLONE_NEWCID	0x00000040
#endif
#ifndef PR_GET_NS_CID_MAX
#define PR_GET_NS_CID_MAX	82
#endif
#define NS_CID_THREADS	3
#define NS_CID_PHASES	6
#define NS_CID_SKIP	100
/* What a member's threads saw, by phase. */
struct ns_cid_seen {
	unsigned int max_cid[NS_CID_PHASES];	/* largest ns_cid seen */
	unsigned long ids[NS_CID_PHASES];	/* samples */
	volatile int violation;			/* a sample reached sh->max */
	unsigned long violations;		/* how many such samples */
};
struct ns_cid_shared {
	volatile int stop;
	volatile int phase;
	int max;			/* every sample must be below this */
	volatile int threads_up;	/* threads that registered their area */
	volatile int noext;		/* a thread got no extended rseq area */
	int member_max[2];		/* what each member read */
	pid_t member0;			/* the first member, for the helper's setns(2) */
	struct ns_cid_seen seen[2];	/* what each member's threads saw */
	/* The joining process: its own thread's samples right after setns(2). */
	unsigned int join_cid;
	unsigned long join_ids;
};

struct ns_cid_spinner {
	struct ns_cid_shared *sh;
	struct ns_cid_seen *seen;
	pthread_t thread;
	int sleepy;
};

/*
 * Record one sample of ns_cid in @seen and return it. The sample is
 * unsigned. An ID which the kernel never assigned then reads as a large
 * value and fails the bound check. As a negative value it would pass.
 *
 * Read the phase before the ID. A thread can be preempted between the two
 * reads until a later phase has started. An ID read first would then be
 * counted in a phase with a lower limit than the one it was read under.
 */
static unsigned int ns_cid_note(struct ns_cid_shared *sh, struct ns_cid_seen *seen,
				const struct rseq *r)
{
	int phase = sh->phase;
	unsigned int cid = rseq_field(r, RSEQ_NS_CID_OFFSET);
	unsigned int *m = &seen->max_cid[phase];
	unsigned int old;

	__atomic_add_fetch(&seen->ids[phase], 1, __ATOMIC_RELAXED);
	if (cid >= (unsigned int)sh->max) {
		seen->violation = 1;
		__atomic_add_fetch(&seen->violations, 1, __ATOMIC_RELAXED);
	}
	old = __atomic_load_n(m, __ATOMIC_RELAXED);
	while (cid > old &&
	       !__atomic_compare_exchange_n(m, &old, cid, true,
					    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
	return cid;
}

static void *ns_cid_spin(void *arg)
{
	struct ns_cid_spinner *sp = arg;
	struct ns_cid_shared *sh = sp->sh;
	size_t len = 0;
	struct rseq *r = rseq_ext_register(&len);
	unsigned int n = 0;

	if (!r)
		sh->noext = 1;
	__atomic_add_fetch(&sh->threads_up, 1, __ATOMIC_RELEASE);
	if (!r)
		return NULL;
	while (!sh->stop) {
		ns_cid_note(sh, sp->seen, r);
		if (sp->sleepy && ++n % 300 == 0)
			usleep(100);
	}
	rseq_ext_unregister(r, len);
	return NULL;
}

static int ns_cid_start(struct ns_cid_shared *sh, struct ns_cid_seen *seen,
			struct ns_cid_spinner *sp, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		sp[i].sh = sh;
		sp[i].seen = seen;
		sp[i].sleepy = i & 1;
		if (pthread_create(&sp[i].thread, NULL, ns_cid_spin, &sp[i]))
			return -1;
	}
	return 0;
}

static void ns_cid_join(struct ns_cid_spinner *sp, int n)
{
	int i;

	for (i = 0; i < n; i++)
		pthread_join(sp[i].thread, NULL);
}

/*
 * Run member @idx. Read the maximum, then sample ns_cid in NS_CID_THREADS
 * threads until sh->stop is set.
 */
static int ns_cid_member(struct ns_cid_shared *sh, int idx)
{
	struct ns_cid_spinner sp[NS_CID_THREADS];

	sh->member_max[idx] = prctl(PR_GET_NS_CID_MAX, 0, 0, 0, 0);
	if (ns_cid_start(sh, &sh->seen[idx], sp, NS_CID_THREADS))
		return 1;
	ns_cid_join(sp, NS_CID_THREADS);
	return 0;
}

/* The number of samples the helper takes right after it joins. */
#define NS_CID_JOIN_SAMPLES	4096
static int ns_cid_sample_join(struct ns_cid_shared *sh, struct ns_cid_seen *seen,
			      const struct rseq *r)
{
	unsigned int max = 0;
	int i;

	sh->join_ids = 0;
	for (i = 0; i < NS_CID_JOIN_SAMPLES; i++) {
		unsigned int cid = ns_cid_note(sh, seen, r);

		sh->join_ids++;
		if (cid > max)
			max = cid;
	}
	sh->join_cid = max;
	return seen->violation;
}

/*
 * Run the helper. It starts single-threaded and outside the namespace. It
 * reads one-byte commands from @cmdfd and writes an int status to @ackfd:
 *   's'  setns(2) through /proc/<member 0>/ns/cid. Reply 0, the errno, or
 *        -1 without an extended rseq area. On success, sample ns_cid
 *        right away.
 *   'j'  Sample again. Reply 1 if any sample so far was at or above
 *        sh->max.
 *   'm'  Reply prctl(PR_GET_NS_CID_MAX).
 *   'r'  Start NS_CID_THREADS sampling threads. Reply 0, or -1.
 *   'q'  Quit. No reply.
 *
 * After setns(2) the thread returns to user space without a context
 * switch. So its first read must already be an ID of the namespace.
 */
static int ns_cid_helper(int cmdfd, int ackfd, struct ns_cid_shared *sh)
{
	struct ns_cid_spinner sp[NS_CID_THREADS];
	struct ns_cid_seen seen = { };
	int running = 0, status;
	struct rseq *r = NULL;
	size_t rlen = 0;
	char c;

	while (read(cmdfd, &c, 1) == 1) {
		status = 0;
		switch (c) {
		case 's': {
			char path[64];
			int fd;

			if (!r) {
				r = rseq_ext_register(&rlen);
				if (!r) {
					status = -1;
					break;
				}
			}
			snprintf(path, sizeof(path), "/proc/%d/ns/cid", (int)sh->member0);
			fd = open(path, O_RDONLY);
			if (fd < 0) {
				status = errno;
				break;
			}
			if (setns(fd, CLONE_NEWCID))
				status = errno;
			close(fd);
			if (!status)
				ns_cid_sample_join(sh, &seen, r);
			break;
		}
		case 'j':
			status = ns_cid_sample_join(sh, &seen, r);
			break;
		case 'm':
			status = prctl(PR_GET_NS_CID_MAX, 0, 0, 0, 0);
			break;
		case 'r':
			if (ns_cid_start(sh, &seen, sp, NS_CID_THREADS))
				status = -1;
			else
				running = 1;
			break;
		case 'q':
			if (running)
				ns_cid_join(sp, NS_CID_THREADS);
			rseq_ext_unregister(r, rlen);
			return 0;
		}
		if (write(ackfd, &status, sizeof(status)) != sizeof(status))
			return 1;
	}
	return 1;
}

static int ns_cid_cmd(int cmdfd, int ackfd, char c)
{
	int status;

	if (write(cmdfd, &c, 1) != 1)
		return -1;
	if (c == 'q')
		return 0;
	if (read(ackfd, &status, sizeof(status)) != sizeof(status))
		return -1;
	return status;
}

/* Return the inode number of the namespace @link of @pid, or -1. */
static long ns_cid_inode(pid_t pid, const char *link)
{
	char path[64], buf[64];
	ssize_t n;
	char *p;

	snprintf(path, sizeof(path), "/proc/%d/ns/%s", (int)pid, link);
	n = readlink(path, buf, sizeof(buf) - 1);
	if (n < 0)
		return -1;
	buf[n] = 0;
	p = strchr(buf, '[');
	return p ? atol(p + 1) : -1;
}

struct ns_cid_param {
	char *base, *outer, *parent, *a, *in, *out;
};

/*
 * Create a child in @cg with clone3(CLONE_NEWCID | CLONE_INTO_CGROUP). The
 * child is in a namespace of its own. Return:
 *    0  the child read the maximum @max and an ns_cid below it;
 *    2  it read another maximum;
 *    3  it could not register an extended rseq area;
 *    4  it read an ns_cid at or above @max;
 *    5  it did not exit normally;
 *   <0  @cg could not be opened or clone3(2) failed: the value is -errno.
 */
static int ns_cid_clone_into(const char *cg, int max)
{
	int fd = open(cg, O_RDONLY | O_DIRECTORY), status;
	struct clone_args args = {
		.flags = CLONE_INTO_CGROUP | CLONE_NEWCID,
		.exit_signal = SIGCHLD,
	};
	pid_t c;

	if (fd < 0)
		return -errno;
	args.cgroup = fd;
	c = syscall(__NR_clone3, &args, sizeof(args));
	if (c == 0) {
		struct rseq *r;
		size_t len = 0;
		unsigned int cid;

		if (prctl(PR_GET_NS_CID_MAX, 0, 0, 0, 0) != max)
			_exit(2);
		r = rseq_ext_register(&len);
		if (!r)
			_exit(3);
		cid = rseq_field(r, RSEQ_NS_CID_OFFSET);
		_exit(cid >= (unsigned int)max ? 4 : 0);
	}
	if (c < 0) {
		status = -errno;
	} else {
		waitpid(c, &status, 0);
		status = WIFEXITED(status) ? WEXITSTATUS(status) : 5;
	}
	close(fd);
	return status;
}

/* Start phase @phase and let the threads sample in it. */
static void ns_cid_phase(struct ns_cid_shared *sh, int phase)
{
	usleep(50000);
	sh->phase = phase;
	usleep(300000);
}

static int ns_cid_run(const char *cgroup, void *arg)
{
	struct ns_cid_param *p = arg;
	struct ns_cid_shared *sh;
	int cmd[2], ack[2], i, ret, status, outside_max;
	pid_t helper, member[2] = { -1, -1 };
	long init_ns, self_ns, child_ns;
	char buf[32];

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		return 1;
	memset(sh, 0, sizeof(*sh));
	/* The limit of "parent". Every sample must be below it. */
	sh->max = 2;
	if (pipe(cmd) || pipe(ack))
		return 1;
	helper = fork();
	if (helper < 0)
		return 1;
	if (helper == 0)
		exit(ns_cid_helper(cmd[0], ack[1], sh));

	init_ns = ns_cid_inode(1, "cid");
	if (unshare(CLONE_NEWCID)) {
		if (errno == EINVAL || errno == EPERM) {
			ret = NS_CID_SKIP;
			ksft_print_msg("no concurrency ID namespace (CLONE_NEWCID)\n");
		} else {
			fprintf(stderr, "unshare(CLONE_NEWCID): %s\n", strerror(errno));
			ret = 2;
		}
		goto out;
	}
	/*
	 * The creator stays in the initial namespace. Its children get the
	 * new one.
	 */
	self_ns = ns_cid_inode(getpid(), "cid");
	child_ns = ns_cid_inode(getpid(), "cid_for_children");
	if (init_ns < 0 || self_ns != init_ns || child_ns < 0 || child_ns == init_ns) {
		fprintf(stderr, "ns/cid %ld, ns/cid_for_children %ld after unshare(CLONE_NEWCID), init's %ld\n",
			self_ns, child_ns, init_ns);
		ret = 3;
		goto out;
	}
	outside_max = prctl(PR_GET_NS_CID_MAX, 0, 0, 0, 0);
	if (outside_max < get_nprocs()) {
		fprintf(stderr, "PR_GET_NS_CID_MAX %d in the initial namespace, %d CPUs online\n",
			outside_max, get_nprocs());
		ret = 4;
		goto out;
	}

	/* Fork two members. */
	for (i = 0; i < 2; i++) {
		member[i] = fork();
		if (member[i] < 0) {
			ret = 1;
			goto stop;
		}
		if (member[i] == 0)
			exit(ns_cid_member(sh, i));
	}
	/* Store the pid of member 0 in the shared mapping, for the helper. */
	sh->member0 = member[0];
	while (__atomic_load_n(&sh->threads_up, __ATOMIC_ACQUIRE) < 2 * NS_CID_THREADS)
		usleep(1000);
	if (sh->noext) {
		fprintf(stderr, "a thread could not register an extended rseq area\n");
		ret = 5;
		goto stop;
	}
	if (ns_cid_inode(member[0], "cid") != child_ns || ns_cid_inode(member[1], "cid") != child_ns) {
		fprintf(stderr, "the members are not in the namespace unshared for them\n");
		ret = 6;
		goto stop;
	}
	if (sh->member_max[0] != 3 || sh->member_max[1] != 3) {
		fprintf(stderr, "PR_GET_NS_CID_MAX %d and %d read by the members under the ceiling 3\n",
			sh->member_max[0], sh->member_max[1]);
		ret = 7;
		goto stop;
	}

	/* Phase 1: both members must take samples in "a". */
	ns_cid_phase(sh, 1);
	for (i = 0; i < 2; i++) {
		if (!sh->seen[i].ids[1]) {
			fprintf(stderr, "member %d: no sample in the limited cgroup\n", i);
			ret = 8;
			goto stop;
		}
	}

	/* A move of member 1 to "out" must fail with EBUSY. */
	snprintf(buf, sizeof(buf), "%d", (int)member[1]);
	ret = cg_write(p->out, "cgroup.procs", buf);
	if (ret != -EBUSY) {
		fprintf(stderr, "moving a member out of the limited cgroup: %s, expected EBUSY\n",
			ret ? strerror(-ret) : "succeeded");
		ret = 9;
		goto stop;
	}
	ns_cid_phase(sh, 2);
	for (i = 0; i < 2; i++) {
		if (!sh->seen[i].ids[2]) {
			fprintf(stderr, "member %d: no sample after the refused move\n", i);
			ret = 10;
			goto stop;
		}
	}
	ret = cg_write(p->in, "cgroup.procs", buf);
	if (!ret)
		ret = cg_write(p->a, "cgroup.procs", buf);
	if (ret) {
		fprintf(stderr, "moving a member within the limited cgroup's subtree: %s\n",
			strerror(-ret));
		ret = 12;
		goto stop;
	}
	ns_cid_phase(sh, 3);
	for (i = 0; i < 2; i++) {
		if (!sh->seen[i].ids[3]) {
			fprintf(stderr, "member %d: no sample back under the limited cgroup\n", i);
			ret = 13;
			goto stop;
		}
	}

	/* Create a child in a new namespace in "out", then one in "in". */
	status = ns_cid_clone_into(p->out, 3);
	if (status != 0 && status != -ENOSYS) {
		fprintf(stderr, "clone3(CLONE_NEWCID | CLONE_INTO_CGROUP) outside the limited cgroup: %d (%s)\n",
			status, status < 0 ? strerror(-status) : "an ID below the maximum expected");
		ret = 14;
		goto stop;
	}
	status = ns_cid_clone_into(p->in, 3);
	if (status != 0 && status != -ENOSYS) {
		fprintf(stderr, "clone3(CLONE_NEWCID | CLONE_INTO_CGROUP) under the limited cgroup: %d (%s)\n",
			status, status < 0 ? strerror(-status) : "an ID below the maximum expected");
		ret = 15;
		goto stop;
	}

	/*
	 * setns(2) of the helper must fail with EBUSY from "out". It must
	 * succeed from "a", and the helper must then read IDs below 2.
	 */
	snprintf(buf, sizeof(buf), "%d", (int)helper);
	if (cg_write(p->out, "cgroup.procs", buf)) {
		ret = 16;
		goto stop;
	}
	status = ns_cid_cmd(cmd[1], ack[0], 's');
	if (status < 0) {
		fprintf(stderr, "the joining process could not register an extended rseq area\n");
		ret = 5;
		goto stop;
	}
	if (status != EBUSY) {
		fprintf(stderr, "setns(2) from outside the limited cgroup: %s, expected EBUSY\n",
			status ? strerror(status) : "succeeded");
		ret = 17;
		goto stop;
	}
	if (ns_cid_inode(helper, "cid") == child_ns) {
		fprintf(stderr, "the refused process is in the namespace\n");
		ret = 19;
		goto stop;
	}
	if (cg_write(p->a, "cgroup.procs", buf)) {
		ret = 16;
		goto stop;
	}
	status = ns_cid_cmd(cmd[1], ack[0], 's');
	if (status) {
		fprintf(stderr, "setns(2) under the limited cgroup: %s\n", strerror(status));
		ret = 17;
		goto stop;
	}
	if (!sh->join_ids || sh->join_cid >= 2) {
		fprintf(stderr, "the thread that joined saw %lu IDs, the largest %u\n",
			sh->join_ids, sh->join_cid);
		ret = 18;
		goto stop;
	}
	if (ns_cid_inode(helper, "cid") != child_ns) {
		fprintf(stderr, "the joining process is not in the namespace\n");
		ret = 19;
		goto stop;
	}
	/* Move the helper within the subtree and sample again. */
	if (cg_write(p->in, "cgroup.procs", buf) || cg_write(p->a, "cgroup.procs", buf)) {
		ret = 16;
		goto stop;
	}
	usleep(50000);
	status = ns_cid_cmd(cmd[1], ack[0], 'j');
	if (status || !sh->join_ids || sh->join_cid >= 2) {
		fprintf(stderr, "the joined process moved within the subtree saw %lu IDs, the largest %u, violation %d\n",
			sh->join_ids, sh->join_cid, status);
		ret = 20;
		goto stop;
	}
	status = ns_cid_cmd(cmd[1], ack[0], 'm');
	if (status != 3) {
		fprintf(stderr, "PR_GET_NS_CID_MAX %d read by the joining process, expected 3\n", status);
		ret = 21;
		goto stop;
	}
	if (ns_cid_cmd(cmd[1], ack[0], 'r')) {
		ret = 1;
		goto stop;
	}
	/* Now multi-threaded, the helper must get EUSERS from setns(2). */
	status = ns_cid_cmd(cmd[1], ack[0], 's');
	if (status != EUSERS) {
		fprintf(stderr, "setns(2) by a multi-threaded process: %s, expected EUSERS\n",
			status ? strerror(status) : "succeeded");
		ret = 22;
		goto stop;
	}
	ns_cid_phase(sh, 4);
	for (i = 0; i < 2; i++) {
		if (!sh->seen[i].ids[4]) {
			fprintf(stderr, "member %d: no sample with three processes\n", i);
			ret = 23;
			goto stop;
		}
	}

	/*
	 * Lower the limit to 1. Once it is in force, every ID must be 0. The
	 * maximum must stay 3.
	 */
	if (cg_write(p->parent, "cpu.concurrency.max", "1")) {
		ret = 24;
		goto stop;
	}
	for (i = 0; i < 200; i++) {
		if (!cg_read_strcmp(p->parent, "cpu.concurrency.effective", "1 3\n"))
			break;
		usleep(10000);
	}
	if (i == 200) {
		fprintf(stderr, "cpu.concurrency.effective did not reach \"1 3\"\n");
		ret = 25;
		goto stop;
	}
	ns_cid_phase(sh, 5);
	for (i = 0; i < 2; i++) {
		if (!sh->seen[i].ids[5] || sh->seen[i].max_cid[5]) {
			fprintf(stderr, "member %d under the limit 1: %lu samples, the largest ID %u\n",
				i, sh->seen[i].ids[5], sh->seen[i].max_cid[5]);
			ret = 26;
			goto stop;
		}
	}
	status = ns_cid_cmd(cmd[1], ack[0], 'm');
	if (status != 3) {
		fprintf(stderr, "PR_GET_NS_CID_MAX %d under the limit 1, expected 3\n", status);
		ret = 21;
		goto stop;
	}
	if (sh->seen[0].violation || sh->seen[1].violation) {
		fprintf(stderr, "an ns_cid at or above the limit 2 was seen\n");
		ret = 27;
		goto stop;
	}
	ret = 0;
stop:
	sh->stop = 1;
out:
	sh->stop = 1;
	ns_cid_cmd(cmd[1], ack[0], 'q');
	waitpid(helper, &status, 0);
	for (i = 0; i < 2; i++) {
		if (member[i] > 0)
			waitpid(member[i], &status, 0);
	}
	return ret;
}

/*
 * Check that ns_cid is the CPU number in the initial namespace, where
 * every other test runs. Pin the thread to up to NS_CID_CPUS CPUs in turn
 * and read the field on each. A kernel which never writes the field leaves
 * it 0, and the bound checks above would not see that. Return KSFT_SKIP if
 * no extended rseq area can be registered, 1 on a wrong value, 0
 * otherwise.
 */
#define NS_CID_CPUS	4
static int ns_cid_is_cpu(void)
{
	cpu_set_t old, one;
	struct rseq *r;
	size_t len = 0;
	int cpu, n, ret = 0;

	r = rseq_ext_register(&len);
	if (!r)
		return skip("cannot register an extended rseq area");
	if (sched_getaffinity(0, sizeof(old), &old))
		goto out;
	for (cpu = 0, n = 0; cpu < CPU_SETSIZE && n < NS_CID_CPUS; cpu++) {
		unsigned int cid;

		if (!CPU_ISSET(cpu, &old))
			continue;
		CPU_ZERO(&one);
		CPU_SET(cpu, &one);
		if (sched_setaffinity(0, sizeof(one), &one))
			break;
		n++;
		/* The migration updated the IDs before this returned. */
		cid = rseq_field(r, RSEQ_NS_CID_OFFSET);
		if (cid != (unsigned int)cpu) {
			fprintf(stderr, "ns_cid %u pinned to CPU %d in the initial namespace\n",
				cid, cpu);
			ret = 1;
			break;
		}
	}
	sched_setaffinity(0, sizeof(old), &old);
out:
	rseq_ext_unregister(r, len);
	return ret;
}

static int test_concurrency_ns_cid(const char *root)
{
	struct ns_cid_param param = { };
	int ret = KSFT_FAIL, r;

	/* The case writes a ceiling of 4, which the CPUs must allow. */
	if (need_cpus(4))
		return KSFT_SKIP;
	r = ns_cid_is_cpu();
	if (r == KSFT_SKIP)
		return KSFT_SKIP;
	if (r)
		return KSFT_FAIL;
	param.base = cg_name(root, "cc_test_ns_cid_base");
	if (!param.base)
		goto cleanup;
	param.outer = cg_name(param.base, "outer");
	if (!param.outer)
		goto cleanup;
	param.parent = cg_name(param.outer, "cc_test_ns_cid");
	param.out = cg_name(param.outer, "cc_test_ns_cid_out");
	if (!param.parent || !param.out)
		goto cleanup;
	param.a = cg_name(param.parent, "a");
	param.in = cg_name(param.parent, "in");
	if (!param.a || !param.in)
		goto cleanup;
	/* The namespace takes the nearest ceiling, not the outer one. */
	if (cg_create(param.base) || cg_write(param.base, "cgroup.subtree_control", "+cpu") ||
	    cg_write(param.base, "cpu.concurrency.child_ceiling", "4") ||
	    cg_create(param.outer) || cg_write(param.outer, "cgroup.subtree_control", "+cpu") ||
	    cg_write(param.outer, "cpu.concurrency.child_ceiling", "3") ||
	    cg_create(param.parent) || cg_write(param.parent, "cgroup.subtree_control", "+cpu") ||
	    cg_create(param.a) || cg_create(param.in) || cg_create(param.out))
		goto cleanup;
	if (cg_write(param.parent, "cpu.concurrency.max", "2") ||
	    cg_read_strcmp(param.parent, "cpu.concurrency.effective", "2 3\n") ||
	    cg_read_strcmp(param.out, "cpu.concurrency.ceiling", "3\n"))
		goto cleanup;

	r = cg_run(param.a, ns_cid_run, &param);
	if (r == NS_CID_SKIP)
		ret = KSFT_SKIP;
	if (r)
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (param.a)
		cg_destroy(param.a);
	if (param.in)
		cg_destroy(param.in);
	if (param.parent)
		cg_destroy(param.parent);
	if (param.out)
		cg_destroy(param.out);
	if (param.outer)
		cg_destroy(param.outer);
	if (param.base)
		cg_destroy(param.base);
	free(param.a);
	free(param.in);
	free(param.parent);
	free(param.out);
	free(param.outer);
	free(param.base);
	return ret;
}

/*
 * Move the members of a namespace while they run. The ID space is as
 * small as the limit.
 *
 * Cgroups, by their name in struct ns_moves_param ("base" is a local
 * variable):
 *
 *   base         child_ceiling 2
 *   |- parent    ceiling 2, cpu.concurrency.max 2
 *   |  |- a      every process starts here
 *   |  `- in
 *   `- out
 *
 * Processes:
 *  - The test process runs ns_moves_run(). It stays in its namespace.
 *  - The joiner is forked before unshare(2). It is single-threaded.
 *  - Members 0, 1 and 2 are forked after unshare(CLONE_NEWCID). Each
 *    exits at once if it does not read the maximum 2. Each has
 *    NS_CID_THREADS threads which read ns_cid in a loop.
 *
 * For NS_MOVES_SECONDS, at the same time:
 *  - A thread of the test process moves member 1 to "in" and back to "a".
 *  - The test process tries to move member 2 to "out". No try may
 *    succeed, and at least one must fail with EBUSY.
 *  - The joiner calls setns(2) into the namespace of member 0, reads
 *    ns_cid 64 times, and calls setns(2) back to its first namespace.
 *  - The test process tries to move the joiner to "out", and back to "a"
 *    when that succeeded. A setns(2) of the joiner into the namespace may
 *    fail with EBUSY, which it gets from "out". Another error fails the
 *    test.
 *
 * At the end the test checks:
 *  - at least one join succeeded, and at least one move to "in";
 *  - no thread of a member and no read of the joiner saw an ns_cid at or
 *    above 2.
 *
 * With ns == 0 (test_concurrency_mm_cid_moves) the same processes are
 * plain ones, without a namespace, and the joiner exits at once. Member 2
 * is then moved to "out" and back, and at least one such move must
 * succeed. The test checks nothing about the IDs itself. It runs the
 * moves for the mm_cid checks of CONFIG_SCHED_MAX_CONCURRENCY_DEBUG, which
 * warn in the kernel log.
 *
 * In both forms the test fails if the kernel warned while it ran, see
 * kwarn_since().
 */
#define NS_MOVES_SECONDS	3
#define TAINT_WARN_BIT	(1L << 9)

/*
 * The warning state of the kernel when a test starts: the taint flags,
 * and /dev/kmsg open at the end of the log. Either can be missing: -1.
 */
struct kwarn_mark {
	long taint;
	int fd;
};

static void kwarn_start(struct kwarn_mark *m)
{
	m->taint = read_sysctl("tainted");
	m->fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (m->fd >= 0)
		lseek(m->fd, 0, SEEK_END);
}

/*
 * Return 1 if the kernel warned since kwarn_start(). Either the taint flag
 * W appeared, or the log got a record with "WARNING:". Print each such
 * record. A WARN_ONCE() which fired in an earlier test sets no new flag
 * and prints nothing, so it is not seen. Return 0 otherwise. Close the
 * log.
 */
static int kwarn_since(struct kwarn_mark *m)
{
	long taint = read_sysctl("tainted");
	int warned = 0;
	char rec[1024];
	ssize_t n;

	if (m->taint >= 0 && taint >= 0 && !(m->taint & TAINT_WARN_BIT) &&
	    (taint & TAINT_WARN_BIT)) {
		fprintf(stderr, "the kernel warned during the test (taint flag W)\n");
		warned = 1;
	}
	while (m->fd >= 0) {
		char *text;

		n = read(m->fd, rec, sizeof(rec) - 1);
		/* EPIPE: the record under the descriptor was overwritten. */
		if (n < 0 && errno == EPIPE)
			continue;
		if (n <= 0)
			break;
		rec[n] = 0;
		if (!strstr(rec, "WARNING:"))
			continue;
		text = strchr(rec, ';');
		fprintf(stderr, "kernel log: %s", text ? text + 1 : rec);
		warned = 1;
	}
	if (m->fd >= 0)
		close(m->fd);
	return warned;
}

struct ns_moves_shared {
	struct ns_cid_shared cid;
	struct ns_cid_seen seen[3];
	volatile int members_up;
	volatile unsigned long moves_out, moves_refused, moves_in, joins, join_fail;
	pid_t member[3];
};

struct ns_moves_param {
	char *parent, *a, *in, *out;
	struct ns_moves_shared *sh;
	int ns;		/* 1: members of a namespace, 0: plain processes */
};

static int ns_moves_member(struct ns_moves_shared *sh, int idx, int ns)
{
	struct ns_cid_spinner sp[NS_CID_THREADS];

	if (ns && prctl(PR_GET_NS_CID_MAX, 0, 0, 0, 0) != 2)
		return 2;
	if (ns_cid_start(&sh->cid, &sh->seen[idx], sp, NS_CID_THREADS))
		return 1;
	__atomic_add_fetch(&sh->members_up, 1, __ATOMIC_RELEASE);
	ns_cid_join(sp, NS_CID_THREADS);
	return 0;
}

/*
 * Run the joiner. It starts in "a", outside the namespace. It calls
 * setns(2) into the namespace of member 0 and back until the test stops.
 * Return 3 if one of its samples was at or above the bound.
 */
static int ns_moves_joiner(struct ns_moves_shared *sh)
{
	struct ns_cid_seen seen = { };
	int fd_ns, fd_orig;
	struct rseq *r;
	char path[64];
	size_t len = 0;

	r = rseq_ext_register(&len);
	if (!r)
		return 11;
	snprintf(path, sizeof(path), "/proc/%d/ns/cid", (int)sh->member[0]);
	fd_ns = open(path, O_RDONLY);
	fd_orig = open("/proc/self/ns/cid", O_RDONLY);
	if (fd_ns < 0 || fd_orig < 0) {
		char st[256] = "";
		int e = errno;

		snprintf(st, sizeof(st), "/proc/%d/status", (int)sh->member[0]);
		proc_read_text(sh->member[0], false, "status", st, sizeof(st));
		fprintf(stderr, "joiner: open %s: %s (%d %d) members_up %d; status: %.80s\n", path,
			strerror(e), fd_ns, fd_orig, sh->members_up, st);
		return 12;
	}
	while (!sh->cid.stop) {
		int i;

		if (setns(fd_ns, CLONE_NEWCID)) {
			/* EBUSY is expected while the joiner is in "out". */
			if (errno != EBUSY) {
				fprintf(stderr, "joiner: setns in: %s\n", strerror(errno));
				return 14;
			}
			sh->join_fail++;
			continue;
		}
		sh->joins++;
		for (i = 0; i < 64; i++)
			ns_cid_note(&sh->cid, &seen, r);
		if (setns(fd_orig, CLONE_NEWCID)) {
			fprintf(stderr, "joiner: setns back: %s after %lu joins\n", strerror(errno), sh->joins);
			return 13;
		}
	}
	return seen.violation ? 3 : 0;
}

/* Move member 1 between "a" and "in", until the end. */
static void *ns_moves_inner(void *arg)
{
	struct ns_moves_param *p = arg;
	struct ns_moves_shared *sh = p->sh;

	while (!sh->cid.stop) {
		if (cg_enter(p->in, sh->member[1]))
			continue;
		cg_enter(p->a, sh->member[1]);
		sh->moves_in++;
	}
	return NULL;
}

static int ns_moves_run(const char *cgroup, void *arg)
{
	struct ns_moves_param *p = arg;
	struct ns_moves_shared *sh = p->sh;
	int i, ret, status, r;
	struct timespec end;
	pthread_t inner;
	pid_t joiner = -1;

	memset(sh, 0, sizeof(*sh));
	sh->cid.max = p->ns ? 2 : INT_MAX;
	joiner = fork();
	if (joiner < 0)
		return 1;
	if (joiner == 0) {
		/*
		 * Wait until the members run and their pids are stored. The
		 * test stops before that when it cannot run.
		 */
		while (__atomic_load_n(&sh->members_up, __ATOMIC_ACQUIRE) < 4) {
			if (sh->cid.stop)
				exit(0);
			usleep(1000);
		}
		exit(p->ns ? ns_moves_joiner(sh) : 0);
	}
	if (p->ns && unshare(CLONE_NEWCID)) {
		if (errno == EINVAL || errno == EPERM) {
			ret = NS_CID_SKIP;
			ksft_print_msg("no concurrency ID namespace (CLONE_NEWCID)\n");
		} else {
			fprintf(stderr, "unshare(CLONE_NEWCID): %s\n", strerror(errno));
			ret = 2;
		}
		sh->cid.stop = 1;
		waitpid(joiner, &status, 0);
		return ret;
	}
	for (i = 0; i < 3; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			ret = 1;
			goto stop;
		}
		if (pid == 0)
			exit(ns_moves_member(sh, i, p->ns));
		/* Only the parent stores the pid of the member. */
		sh->member[i] = pid;
	}
	while (__atomic_load_n(&sh->members_up, __ATOMIC_ACQUIRE) < 3)
		usleep(1000);
	if (sh->cid.noext) {
		ksft_print_msg("cannot register an extended rseq area\n");
		ret = NS_CID_SKIP;
		goto stop;
	}
	/* The joiner may start: the pids are in place. */
	__atomic_store_n(&sh->members_up, 4, __ATOMIC_RELEASE);
	if (pthread_create(&inner, NULL, ns_moves_inner, p)) {
		ret = 1;
		goto stop;
	}
	/*
	 * Try to move member 2 and the joiner to "out" and back. With a
	 * namespace, every move of member 2 to "out" must be refused.
	 */
	clock_gettime(CLOCK_MONOTONIC, &end);
	ts_add_ms(&end, NS_MOVES_SECONDS * 1000);
	for (;;) {
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (!ts_before(&now, &end))
			break;
		r = cg_enter(p->out, sh->member[2]);
		if (r == -EBUSY)
			sh->moves_refused++;
		else if (!r && !cg_enter(p->a, sh->member[2]))
			sh->moves_out++;
		/* The kernel refuses the move while the joiner is a member. */
		if (!cg_enter(p->out, joiner))
			cg_enter(p->a, joiner);
	}
	sh->cid.stop = 1;
	pthread_join(inner, NULL);
	waitpid(joiner, &status, 0);
	r = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
	joiner = -1;
	if (r) {
		fprintf(stderr, "the joining process failed: %d\n", r);
		ret = 5;
		goto stop;
	}
	if (p->ns ? (sh->moves_out || !sh->moves_refused || !sh->joins) : !sh->moves_out) {
		fprintf(stderr, "moves out %lu, refused %lu, joins %lu\n",
			sh->moves_out, sh->moves_refused, sh->joins);
		ret = 6;
		goto stop;
	}
	if (!sh->moves_in) {
		fprintf(stderr, "no move within the subtree went through\n");
		ret = 6;
		goto stop;
	}
	for (i = 0; p->ns && i < 3; i++) {
		if (sh->seen[i].violation) {
			fprintf(stderr, "member %d saw an ns_cid at or above the maximum 2: %lu of %lu samples, largest %#x\n",
				i, sh->seen[i].violations, sh->seen[i].ids[0], sh->seen[i].max_cid[0]);
			ret = 7;
			goto stop;
		}
	}
	if (p->ns)
		ksft_print_msg("%lu moves out refused, %lu moves in, %lu joins (%lu refused from outside)\n",
			       sh->moves_refused, sh->moves_in, sh->joins, sh->join_fail);
	ret = 0;
stop:
	sh->cid.stop = 1;
	if (joiner > 0)
		waitpid(joiner, &status, 0);
	for (i = 0; i < 3; i++) {
		if (sh->member[i] > 0)
			waitpid(sh->member[i], &status, 0);
	}
	return ret;
}

static int __test_concurrency_ns_cid_moves(const char *root, int ns)
{
	struct ns_moves_param p = { .ns = ns };
	struct kwarn_mark mark;
	int ret = KSFT_FAIL, r;
	char *base = NULL;

	if (need_cpus(3))
		return KSFT_SKIP;
	kwarn_start(&mark);
	p.sh = mmap(NULL, sizeof(*p.sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (p.sh == MAP_FAILED)
		return KSFT_FAIL;
	base = cg_name(root, "cc_test_ns_moves");
	if (!base)
		goto cleanup;
	p.parent = cg_name(base, "p");
	p.out = cg_name(base, "out");
	if (!p.parent || !p.out)
		goto cleanup;
	p.a = cg_name(p.parent, "a");
	p.in = cg_name(p.parent, "in");
	if (!p.a || !p.in)
		goto cleanup;
	if (cg_create(base) || cg_write(base, "cgroup.subtree_control", "+cpu") ||
	    cg_write(base, "cpu.concurrency.child_ceiling", "2") ||
	    cg_create(p.parent) || cg_write(p.parent, "cgroup.subtree_control", "+cpu") ||
	    cg_create(p.a) || cg_create(p.in) || cg_create(p.out) ||
	    cg_write(p.parent, "cpu.concurrency.max", "2"))
		goto cleanup;

	r = cg_run(p.a, ns_moves_run, &p);
	if (r == NS_CID_SKIP)
		ret = KSFT_SKIP;
	if (r)
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (p.a)
		cg_destroy(p.a);
	if (p.in)
		cg_destroy(p.in);
	if (p.parent)
		cg_destroy(p.parent);
	if (p.out)
		cg_destroy(p.out);
	if (base)
		cg_destroy(base);
	free(p.a);
	free(p.in);
	free(p.parent);
	free(p.out);
	free(base);
	munmap(p.sh, sizeof(*p.sh));
	if (kwarn_since(&mark) && ret == KSFT_PASS)
		ret = KSFT_FAIL;
	return ret;
}

static int test_concurrency_ns_cid_moves(const char *root)
{
	return __test_concurrency_ns_cid_moves(root, 1);
}

/*
 * Run the same moves with plain processes. The test checks nothing about
 * their mm_cid itself. The moves exercise the mm_cid checks of
 * CONFIG_SCHED_MAX_CONCURRENCY_DEBUG, which warn in the kernel log. The
 * test fails on such a warning, see kwarn_since().
 */
static int test_concurrency_mm_cid_moves(const char *root)
{
	return __test_concurrency_ns_cid_moves(root, 0);
}

#ifndef NS_GET_CID_CGROUP
#define NS_GET_CID_CGROUP	_IOR(0xb7, 14, __u64)
#endif

/*
 * Read the home of the namespace behind @fd into @home. It is the ID of a
 * cgroup, or 0 for a namespace without a home. Return 0, or -1 if the
 * ioctl failed.
 */
static int ns_cid_home(int fd, unsigned long long *home)
{
	__u64 id = ~0ULL;

	if (ioctl(fd, NS_GET_CID_CGROUP, &id))
		return -1;
	*home = id;
	return 0;
}

/*
 * Return the ID of the cgroup @cg, or 0. It is the inode number of the
 * directory.
 */
static unsigned long long ns_cid_cgroup_id(const char *cg)
{
	struct stat st;

	return stat(cg, &st) ? 0 : st.st_ino;
}

/*
 * Return the number of namespaces which cpu.concurrency.namespaces of @cg
 * lists, or -1 if the file cannot be read. Set *@found to 1 if the
 * namespace with the inode number @inum is one of them, to 0 otherwise.
 */
static int ns_cid_listed(const char *cg, long inum, int *found)
{
	char buf[1024], *line, *save;
	int n = 0;

	*found = 0;
	if (cg_read(cg, "cpu.concurrency.namespaces", buf, sizeof(buf)))
		return -1;
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		n++;
		if (atol(line) == inum)
			*found = 1;
	}
	return n;
}

/*
 * Check the home of the namespace @inum, which the caller reaches through
 * @fd. With @home, the ioctl must return the ID of that cgroup, and that
 * cgroup must list @inum and nothing else. Without @home, the ioctl must
 * return 0. Each cgroup of the NULL-terminated list @others must list no
 * namespace. Return 0 if all of this holds. Print what does not, with
 * @when.
 */
static int ns_cid_check_home(int fd, long inum, const char *home, const char **others,
			     const char *when)
{
	unsigned long long id, want = home ? ns_cid_cgroup_id(home) : 0;
	int n, found, ret = 0;

	if (ns_cid_home(fd, &id)) {
		fprintf(stderr, "%s: ioctl(NS_GET_CID_CGROUP): %s\n", when, strerror(errno));
		return 1;
	}
	if ((home && !want) || id != want) {
		fprintf(stderr,
			"%s: NS_GET_CID_CGROUP returned %llu, expected %llu\n", when, id, want);
		ret = 1;
	}
	if (home) {
		n = ns_cid_listed(home, inum, &found);
		if (n != 1 || !found) {
			fprintf(stderr,
				"%s: cpu.concurrency.namespaces of the home has %d lines, namespace %ld %s\n",
				when, n, inum, found ? "listed" : "not listed");
			ret = 1;
		}
	}
	for (; others && *others; others++) {
		n = ns_cid_listed(*others, inum, &found);
		if (n) {
			fprintf(stderr,
				"%s: cpu.concurrency.namespaces of %s has %d lines, expected none\n",
				when, *others, n);
			ret = 1;
		}
	}
	return ret;
}

/*
 * Remove the cgroup @cg, which was the home of a namespace. The kernel may
 * free a namespace a moment after its last user is gone. It allows the
 * removal only then. Try for 10 s. Return 0, or -1 if the cgroup is still
 * there. cg_destroy() would retry for ever.
 */
static int ns_cid_destroy_home(const char *cg)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if (!rmdir(cg) || errno == ENOENT)
			return 0;
		if (errno != EBUSY)
			break;
		usleep(10000);
	}
	fprintf(stderr, "rmdir of %s: %s\n", cg, strerror(errno));
	return -1;
}

/* The state of the single-threaded members of a namespace, see ns_home_child(). */
#define NS_HOME_CHILDREN	6
struct ns_home_shared {
	volatile int stop, sample, up, noext;
	int ns_max[NS_HOME_CHILDREN];			/* PR_GET_NS_CID_MAX */
	unsigned int max_cid[NS_HOME_CHILDREN];		/* largest ns_cid sampled */
	unsigned long samples[NS_HOME_CHILDREN];
};

/*
 * Run member @idx. Read the maximum once. Then read ns_cid in a loop until
 * sh->stop is set. Record the reads made while sh->sample is set.
 */
static int ns_home_child(struct ns_home_shared *sh, int idx)
{
	struct rseq *r;
	size_t len = 0;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	sh->ns_max[idx] = prctl(PR_GET_NS_CID_MAX, 0, 0, 0, 0);
	r = rseq_ext_register(&len);
	if (!r)
		sh->noext = 1;
	__atomic_add_fetch(&sh->up, 1, __ATOMIC_RELEASE);
	if (!r)
		return 1;
	while (!sh->stop) {
		unsigned int cid = rseq_field(r, RSEQ_NS_CID_OFFSET);

		if (!sh->sample)
			continue;
		if (cid > sh->max_cid[idx])
			sh->max_cid[idx] = cid;
		sh->samples[idx]++;
	}
	rseq_ext_unregister(r, len);
	return 0;
}

/*
 * Wait until @n members have registered their rseq area, then let them
 * sample ns_cid for 500 ms. Return 0 if each member took a sample, read an
 * ns_cid below @bound only, and read the maximum @max. With @bound 0, the
 * bound is the maximum which member 0 read. With @max 0, the maximum must
 * be the one which member 0 read.
 *
 * The wait is at most 10 s. A fork and one system call take much less,
 * also on a slow guest.
 */
static int ns_home_sample(struct ns_home_shared *sh, int n, unsigned int bound, int max)
{
	int i, ret = 0;

	for (i = 0; i < 10000 && __atomic_load_n(&sh->up, __ATOMIC_ACQUIRE) < n; i++)
		usleep(1000);
	if (sh->noext || i == 10000) {
		fprintf(stderr, "%d of %d members registered an extended rseq area\n",
			sh->noext ? 0 : sh->up, n);
		return 1;
	}
	sh->sample = 1;
	usleep(500000);
	sh->sample = 0;
	if (!max)
		max = sh->ns_max[0];
	if (!bound)
		bound = max;
	for (i = 0; i < n; i++) {
		if (!sh->samples[i]) {
			fprintf(stderr, "member %d took no sample in 500 ms\n", i);
			ret = 1;
		}
		if (sh->max_cid[i] >= bound) {
			fprintf(stderr, "member %d read ns_cid %u, expected one below %u\n",
				i, sh->max_cid[i], bound);
			ret = 1;
		}
		if (sh->ns_max[i] != max) {
			fprintf(stderr, "member %d read the maximum %d, expected %d\n",
				i, sh->ns_max[i], max);
			ret = 1;
		}
	}
	return ret;
}

/* Stop the @n members @child and reap them. Return 0 if all exited with 0. */
static int ns_home_reap(struct ns_home_shared *sh, pid_t *child, int n)
{
	int i, status, ret = 0;

	sh->stop = 1;
	for (i = 0; i < n; i++) {
		if (waitpid(child[i], &status, 0) != child[i] || !WIFEXITED(status) ||
		    WEXITSTATUS(status))
			ret = 1;
	}
	return ret;
}

/* Fail unshare(CLONE_NEWCID): return NS_CID_SKIP on a kernel without it, or 2. */
static int ns_home_no_unshare(void)
{
	if (errno == EINVAL || errno == EPERM) {
		ksft_print_msg("no concurrency ID namespace (CLONE_NEWCID)\n");
		return NS_CID_SKIP;
	}
	fprintf(stderr, "unshare(CLONE_NEWCID): %s\n", strerror(errno));
	return 2;
}

/* The cgroups of the ns_home cases, by their name in the comment of each case. */
struct ns_home_param {
	char *base, *l, *a, *in, *n, *lp;
	struct ns_home_shared *sh;
};

static void ns_home_param_free(struct ns_home_param *p)
{
	free(p->a);
	free(p->in);
	free(p->l);
	free(p->lp);
	free(p->n);
	free(p->base);
	if (p->sh && p->sh != MAP_FAILED)
		munmap(p->sh, sizeof(*p->sh));
}

static struct ns_home_shared *ns_home_map(void)
{
	return mmap(NULL, sizeof(struct ns_home_shared), PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

/*
 * The home of a namespace made by unshare(2) is decided at that call, from
 * the cgroup of the caller.
 *
 * Cgroups:
 *
 *   base
 *   |- l       cpu.concurrency.max 2
 *   |  |- a    the creator starts here
 *   |  `- in
 *   `- n       no limit
 *
 * Processes:
 *  - The creator runs ns_home_unshare_run(). It stays in the namespace of
 *    init.
 *  - Members 0 to 3 are forked by the creator in step 4. Each is
 *    single-threaded and reads ns_cid in a loop.
 *
 * Steps, all done by the creator:
 *  1. unshare(CLONE_NEWCID) in "a". Open /proc/self/ns/cid_for_children.
 *  2. Check the home, see below. No process is in the namespace yet.
 *  3. Move itself to "n" and call fork(). It must fail with EBUSY: the
 *     child would be a member outside the subtree of "l". Move back to "a".
 *  4. Fork members 0 to 3 in "a". Let them sample ns_cid for 500 ms. Every
 *     sample must be below 2, the limit of "l".
 *  5. clone3(CLONE_INTO_CGROUP) a member into "n". It must fail with EBUSY.
 *     The same call into "in" must succeed.
 *  6. Check the home again.
 *
 * "Check the home" is: the ioctl NS_GET_CID_CGROUP on the namespace returns
 * the ID of "l". cpu.concurrency.namespaces of "l" has one line, the inode
 * number of the namespace. The file of "a" and the file of "n" are empty.
 *
 * A kernel which decides the home when the first member joins fails step
 * 2. It also accepts the fork of step 3.
 */
static int ns_home_unshare_run(const char *cgroup, void *arg)
{
	struct ns_home_param *p = arg;
	const char *others[] = { p->a, p->n, NULL };
	struct ns_home_shared *sh = p->sh;
	pid_t child[4], pid;
	int fd, cgfd, n = 0, ret, status;
	struct stat st;

	if (unshare(CLONE_NEWCID))
		return ns_home_no_unshare();
	fd = open("/proc/self/ns/cid_for_children", O_RDONLY);
	if (fd < 0 || fstat(fd, &st)) {
		fprintf(stderr, "/proc/self/ns/cid_for_children: %s\n", strerror(errno));
		return 3;
	}
	if (ns_cid_check_home(fd, st.st_ino, p->l, others, "after unshare(2), before the first member"))
		return 4;

	if (cg_enter_current(p->n))
		return 5;
	pid = fork();
	ret = errno;
	if (pid == 0)
		_exit(0);
	if (pid > 0)
		waitpid(pid, &status, 0);
	if (cg_enter_current(p->a))
		return 5;
	if (pid >= 0 || ret != EBUSY) {
		fprintf(stderr,
			"fork(2) of a member outside the subtree of the home: %s, expected EBUSY\n",
			pid >= 0 ? "succeeded" : strerror(ret));
		return 6;
	}

	for (n = 0; n < 4; n++) {
		child[n] = fork();
		if (child[n] == 0)
			exit(ns_home_child(sh, n));
		if (child[n] < 0) {
			fprintf(stderr, "fork of member %d in the home: %s\n", n, strerror(errno));
			ret = 7;
			goto stop;
		}
	}
	ret = 8;
	if (ns_home_sample(sh, n, 2, 0))
		goto stop;

	ret = 9;
	cgfd = open(p->n, O_RDONLY | O_DIRECTORY);
	if (cgfd < 0)
		goto stop;
	pid = clone_into_cgroup(cgfd);
	status = errno;
	close(cgfd);
	if (pid == 0)
		_exit(0);
	if (pid > 0)
		waitpid(pid, NULL, 0);
	if (pid < 0 && status == ENOSYS) {
		ksft_print_msg("no clone3(2): step 5 skipped\n");
	} else if (pid >= 0 || status != EBUSY) {
		fprintf(stderr,
			"clone3(CLONE_INTO_CGROUP) of a member outside the subtree of the home: %s, expected EBUSY\n",
			pid >= 0 ? "succeeded" : strerror(status));
		goto stop;
	} else {
		cgfd = open(p->in, O_RDONLY | O_DIRECTORY);
		if (cgfd < 0)
			goto stop;
		pid = clone_into_cgroup(cgfd);
		status = errno;
		close(cgfd);
		if (pid == 0)
			_exit(0);
		if (pid < 0) {
			fprintf(stderr,
				"clone3(CLONE_INTO_CGROUP) of a member under the home: %s\n",
				strerror(status));
			goto stop;
		}
		waitpid(pid, NULL, 0);
	}
	ret = ns_cid_check_home(fd, st.st_ino, p->l, others, "with four members") ? 10 : 0;
stop:
	if (ns_home_reap(sh, child, n) && !ret)
		ret = 11;
	return ret;
}

static int test_concurrency_ns_home_unshare(const char *root)
{
	struct ns_home_param p = { };
	int ret = KSFT_FAIL, r;

	if (need_cpus(3))
		return KSFT_SKIP;
	r = ns_cid_is_cpu();
	if (r)
		return r == KSFT_SKIP ? KSFT_SKIP : KSFT_FAIL;
	p.sh = ns_home_map();
	if (p.sh == MAP_FAILED)
		return KSFT_FAIL;
	p.base = cg_name(root, "cc_test_ns_home_unshare");
	if (!p.base)
		goto cleanup;
	p.l = cg_name(p.base, "l");
	p.n = cg_name(p.base, "n");
	if (!p.l || !p.n)
		goto cleanup;
	p.a = cg_name(p.l, "a");
	p.in = cg_name(p.l, "in");
	if (!p.a || !p.in)
		goto cleanup;
	if (cg_create(p.base) || cg_write(p.base, "cgroup.subtree_control", "+cpu") ||
	    cg_create(p.l) || cg_write(p.l, "cgroup.subtree_control", "+cpu") ||
	    cg_create(p.a) || cg_create(p.in) || cg_create(p.n) ||
	    cg_write(p.l, "cpu.concurrency.max", "2"))
		goto cleanup;
	r = cg_run(p.a, ns_home_unshare_run, &p);
	if (r == NS_CID_SKIP)
		ret = KSFT_SKIP;
	else if (!r)
		ret = KSFT_PASS;
cleanup:
	if (p.a)
		cg_destroy(p.a);
	if (p.in)
		cg_destroy(p.in);
	if (p.l && ns_cid_destroy_home(p.l) && ret == KSFT_PASS)
		ret = KSFT_FAIL;
	if (p.n)
		cg_destroy(p.n);
	if (p.base)
		cg_destroy(p.base);
	ns_home_param_free(&p);
	return ret;
}

/* Run member 0 of ns_home_clone: fork members 1 to 3, then sample as they do. */
static int ns_home_clone_member0(struct ns_home_shared *sh)
{
	pid_t child[3];
	int i, n = 0, ret;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	for (i = 1; i < 4; i++) {
		child[n] = fork();
		if (child[n] == 0)
			exit(ns_home_child(sh, i));
		if (child[n] < 0) {
			fprintf(stderr, "fork of member %d in the home: %s\n", i, strerror(errno));
			sh->stop = 1;
			break;
		}
		n++;
	}
	ret = n == 3 ? ns_home_child(sh, 0) : 1;
	for (i = 0; i < n; i++)
		waitpid(child[i], NULL, 0);
	return ret;
}

/*
 * The home of a namespace made by clone3(CLONE_NEWCID) is decided from the
 * cgroup of the child.
 *
 * Cgroups:
 *
 *   base
 *   |- n       no limit, no ceiling; the parent runs here
 *   `- lp      cpu.concurrency.child_ceiling 3
 *      `- l    ceiling 3, cpu.concurrency.max 2
 *
 * Processes:
 *  - The parent runs ns_home_clone_run() in "n". It stays in the namespace
 *    of init.
 *  - Member 0 is the child of the clone3(2) of step 1. It forks members 1
 *    to 3 in "l". Each member is single-threaded and reads ns_cid in a loop.
 *
 * Steps, all done by the parent:
 *  1. clone3(CLONE_NEWCID | CLONE_INTO_CGROUP) member 0 into "l".
 *  2. Wait until the four members run. Open /proc/<member 0>/ns/cid. It
 *     must not be the namespace of the parent.
 *  3. The ioctl NS_GET_CID_CGROUP must return the ID of "l".
 *     cpu.concurrency.namespaces of "l" must have one line, the inode
 *     number of the namespace. The files of "n" and "lp" must be empty.
 *  4. Let the members sample ns_cid for 500 ms. Every sample must be below
 *     2, the limit of "l". Each member must have read the maximum 3, the
 *     ceiling of "l".
 *  5. Move member 0 to "n". It must fail with EBUSY.
 *
 * A kernel which takes the home from the cgroup of the parent gives no
 * home. Step 3 then reads 0. The members then read the number of possible
 * CPUs as their maximum.
 */
static int ns_home_clone_run(const char *cgroup, void *arg)
{
	struct clone_args args = {
		.flags = CLONE_INTO_CGROUP | CLONE_NEWCID,
		.exit_signal = SIGCHLD,
	};
	struct ns_home_param *p = arg;
	const char *others[] = { p->n, p->lp, NULL };
	struct ns_home_shared *sh = p->sh;
	int cgfd, fd = -1, ret, r;
	char path[64];
	struct stat st;
	pid_t m0;

	cgfd = open(p->l, O_RDONLY | O_DIRECTORY);
	if (cgfd < 0)
		return 1;
	args.cgroup = cgfd;
	m0 = syscall(__NR_clone3, &args, sizeof(args));
	if (m0 == 0)
		exit(ns_home_clone_member0(sh));
	if (m0 < 0) {
		if (errno == ENOSYS || errno == EINVAL) {
			ksft_print_msg("no clone3(2) with CLONE_NEWCID\n");
			return NS_CID_SKIP;
		}
		fprintf(stderr, "clone3(CLONE_NEWCID | CLONE_INTO_CGROUP): %s\n", strerror(errno));
		return 2;
	}
	close(cgfd);
	/* Start to sample only once the home is checked: wait here first. */
	for (r = 0; r < 10000 && __atomic_load_n(&sh->up, __ATOMIC_ACQUIRE) < 4 && !sh->stop; r++)
		usleep(1000);
	snprintf(path, sizeof(path), "/proc/%d/ns/cid", (int)m0);
	fd = open(path, O_RDONLY);
	ret = 3;
	if (fd < 0 || fstat(fd, &st)) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		goto stop;
	}
	if ((long)st.st_ino == ns_cid_inode(getpid(), "cid")) {
		fprintf(stderr,
			"the child of clone3(CLONE_NEWCID) is in the namespace of its parent\n");
		goto stop;
	}
	ret = 4;
	if (ns_cid_check_home(fd, st.st_ino, p->l, others, "after clone3(2) into the limited cgroup"))
		goto stop;
	ret = 5;
	if (ns_home_sample(sh, 4, 2, 3))
		goto stop;
	r = cg_enter(p->n, m0);
	if (r != -EBUSY) {
		fprintf(stderr, "moving member 0 out of the home: %s, expected EBUSY\n",
			r ? strerror(-r) : "succeeded");
		ret = 6;
		goto stop;
	}
	ret = 0;
stop:
	if (fd >= 0)
		close(fd);
	if (ns_home_reap(sh, &m0, 1) && !ret)
		ret = 7;
	return ret;
}

static int test_concurrency_ns_home_clone(const char *root)
{
	struct ns_home_param p = { };
	int ret = KSFT_FAIL, r;

	if (need_cpus(4))
		return KSFT_SKIP;
	r = ns_cid_is_cpu();
	if (r)
		return r == KSFT_SKIP ? KSFT_SKIP : KSFT_FAIL;
	p.sh = ns_home_map();
	if (p.sh == MAP_FAILED)
		return KSFT_FAIL;
	p.base = cg_name(root, "cc_test_ns_home_clone");
	if (!p.base)
		goto cleanup;
	p.lp = cg_name(p.base, "lp");
	p.n = cg_name(p.base, "n");
	if (!p.lp || !p.n)
		goto cleanup;
	p.l = cg_name(p.lp, "l");
	if (!p.l)
		goto cleanup;
	if (cg_create(p.base) || cg_write(p.base, "cgroup.subtree_control", "+cpu") ||
	    cg_create(p.n) || cg_create(p.lp) ||
	    cg_write(p.lp, "cgroup.subtree_control", "+cpu") ||
	    cg_write(p.lp, "cpu.concurrency.child_ceiling", "3") ||
	    cg_create(p.l) || cg_write(p.l, "cpu.concurrency.max", "2") ||
	    cg_read_strcmp(p.l, "cpu.concurrency.effective", "2 3\n"))
		goto cleanup;
	r = cg_run(p.n, ns_home_clone_run, &p);
	if (r == NS_CID_SKIP)
		ret = KSFT_SKIP;
	else if (!r)
		ret = KSFT_PASS;
cleanup:
	if (p.l && ns_cid_destroy_home(p.l) && ret == KSFT_PASS)
		ret = KSFT_FAIL;
	if (p.lp)
		cg_destroy(p.lp);
	if (p.n)
		cg_destroy(p.n);
	if (p.base)
		cg_destroy(p.base);
	ns_home_param_free(&p);
	return ret;
}

/*
 * A namespace created where no limit applies has no home, and never gets
 * one.
 *
 * Cgroups:
 *
 *   base
 *   |- n    no limit; the creator starts here
 *   `- l    cpu.concurrency.max 2
 *
 * Processes:
 *  - The creator runs ns_no_home_run(). It stays in the namespace of init.
 *  - Members 0 to 5 are forked by the creator after unshare(CLONE_NEWCID).
 *    Each is single-threaded. Each reads PR_GET_NS_CID_MAX once, then reads
 *    ns_cid in a loop.
 *
 * Steps, all done by the creator:
 *  1. unshare(CLONE_NEWCID) in "n". Open /proc/self/ns/cid_for_children.
 *  2. Check that the namespace has no home, see below.
 *  3. Fork member 0 in "n".
 *  4. Move itself to "l". Fork members 1 to 5. They are born under the
 *     limit 2. No fork may fail.
 *  5. Move member 1 to "n". The move must succeed.
 *  6. Let the six members sample ns_cid for 500 ms. All must have read the
 *     same maximum. Every sample must be below that maximum.
 *  7. Check again that the namespace has no home.
 *
 * "No home" is: the ioctl NS_GET_CID_CGROUP on the namespace returns 0, and
 * cpu.concurrency.namespaces of "n" and of "l" are empty.
 *
 * The limit of "l" does not bound the IDs of this namespace. So the samples
 * of members 2 to 5 may be at or above 2. The test prints the largest one.
 * A kernel which gives the namespace the home "l" in step 4 refuses step 5
 * with EBUSY.
 */
static int ns_no_home_run(const char *cgroup, void *arg)
{
	struct ns_home_param *p = arg;
	const char *others[] = { p->n, p->l, NULL };
	struct ns_home_shared *sh = p->sh;
	pid_t child[NS_HOME_CHILDREN];
	unsigned int largest = 0;
	int fd, i, n = 0, ret, r;
	struct stat st;

	if (unshare(CLONE_NEWCID))
		return ns_home_no_unshare();
	fd = open("/proc/self/ns/cid_for_children", O_RDONLY);
	if (fd < 0 || fstat(fd, &st)) {
		fprintf(stderr, "/proc/self/ns/cid_for_children: %s\n", strerror(errno));
		return 3;
	}
	if (ns_cid_check_home(fd, st.st_ino, NULL, others, "after unshare(2) where no limit applies"))
		return 4;
	for (n = 0; n < NS_HOME_CHILDREN; n++) {
		if (n == 1 && cg_enter_current(p->l)) {
			fprintf(stderr, "the creator cannot move to the limited cgroup\n");
			ret = 5;
			goto stop;
		}
		child[n] = fork();
		if (child[n] == 0)
			exit(ns_home_child(sh, n));
		if (child[n] < 0) {
			fprintf(stderr, "fork of member %d %s: %s\n", n,
				n ? "under the limit" : "where no limit applies", strerror(errno));
			ret = 6;
			goto stop;
		}
	}
	r = cg_enter(p->n, child[1]);
	if (r) {
		fprintf(stderr, "moving member 1 out of the limited cgroup: %s\n", strerror(-r));
		ret = 7;
		goto stop;
	}
	ret = 8;
	if (ns_home_sample(sh, n, 0, 0))
		goto stop;
	for (i = 2; i < n; i++)
		largest = MAX(largest, sh->max_cid[i]);
	ksft_print_msg("members under the limit 2 read ns_cid up to %u, the maximum is %d\n",
		       largest, sh->ns_max[0]);
	ret = ns_cid_check_home(fd, st.st_ino, NULL, others, "with members under a limit") ? 9 : 0;
stop:
	if (ns_home_reap(sh, child, n) && !ret)
		ret = 10;
	return ret;
}

static int test_concurrency_ns_no_home(const char *root)
{
	struct ns_home_param p = { };
	int ret = KSFT_FAIL, r;

	if (need_cpus(3))
		return KSFT_SKIP;
	r = ns_cid_is_cpu();
	if (r)
		return r == KSFT_SKIP ? KSFT_SKIP : KSFT_FAIL;
	p.sh = ns_home_map();
	if (p.sh == MAP_FAILED)
		return KSFT_FAIL;
	p.base = cg_name(root, "cc_test_ns_no_home");
	if (!p.base)
		goto cleanup;
	p.n = cg_name(p.base, "n");
	p.l = cg_name(p.base, "l");
	if (!p.n || !p.l)
		goto cleanup;
	if (cg_create(p.base) || cg_write(p.base, "cgroup.subtree_control", "+cpu") ||
	    cg_create(p.n) || cg_create(p.l) || cg_write(p.l, "cpu.concurrency.max", "2"))
		goto cleanup;
	r = cg_run(p.n, ns_no_home_run, &p);
	if (r == NS_CID_SKIP)
		ret = KSFT_SKIP;
	else if (!r)
		ret = KSFT_PASS;
cleanup:
	if (p.l && ns_cid_destroy_home(p.l) && ret == KSFT_PASS)
		ret = KSFT_FAIL;
	if (p.n)
		cg_destroy(p.n);
	if (p.base)
		cg_destroy(p.base);
	ns_home_param_free(&p);
	return ret;
}

/*
 * ns_ceiling_run() runs this as the creator, see
 * test_concurrency_ns_ceiling_only().
 */
static int ns_ceiling_run(const char *cgroup, void *arg)
{
	struct ns_home_shared *sh = arg;
	pid_t child[4];
	int n, ret;

	if (unshare(CLONE_NEWCID))
		return ns_home_no_unshare();
	for (n = 0; n < 4; n++) {
		child[n] = fork();
		if (child[n] == 0)
			exit(ns_home_child(sh, n));
		if (child[n] < 0) {
			fprintf(stderr, "fork of member %d: %s\n", n, strerror(errno));
			ret = 3;
			goto stop;
		}
	}
	ret = ns_home_sample(sh, n, 0, 2) ? 4 : 0;
stop:
	if (ns_home_reap(sh, child, n) && !ret)
		ret = 5;
	return ret;
}

/*
 * The IDs of a namespace stay below its maximum when its cgroup has a
 * ceiling and no limit.
 *
 * Cgroups: base, and its child c. The test writes 2 to
 * cpu.concurrency.child_ceiling of base before it creates c. So c has the
 * ceiling 2. No cgroup has a limit. The effective value of c reads "2 2":
 * a ceiling without a limit is enforced as a limit.
 *
 * Processes:
 *  - The creator runs ns_ceiling_run() in c. It calls
 *    unshare(CLONE_NEWCID) and stays in the namespace of init.
 *  - Members 0 to 3 are forked by the creator, in c. Each is
 *    single-threaded and can run on every CPU. Each reads
 *    PR_GET_NS_CID_MAX once, then reads ns_cid in a loop.
 * The four members sample ns_cid for 500 ms, see ns_home_sample().
 *
 * Checked: each member reads the maximum 2, and every sample is below 2.
 * A process sizes its per-ID data by the maximum.
 * Skipped with fewer than 4 CPUs, or without a concurrency ID namespace.
 */
static int test_concurrency_ns_ceiling_only(const char *root)
{
	char *base = NULL, *c = NULL;
	struct ns_home_shared *sh;
	int ret = KSFT_FAIL, r;

	if (need_cpus(4))
		return KSFT_SKIP;
	r = ns_cid_is_cpu();
	if (r)
		return r == KSFT_SKIP ? KSFT_SKIP : KSFT_FAIL;
	sh = ns_home_map();
	if (sh == MAP_FAILED)
		return KSFT_FAIL;
	base = cg_name(root, "cc_test_ns_ceiling");
	c = base ? cg_name(base, "c") : NULL;
	if (!c || cg_create(base) || cg_write(base, "cgroup.subtree_control", "+cpu") ||
	    cg_write(base, "cpu.concurrency.child_ceiling", "2") || cg_create(c) ||
	    cg_read_strcmp(c, "cpu.concurrency.effective", "2 2\n"))
		goto cleanup;
	r = cg_run(c, ns_ceiling_run, sh);
	if (r == NS_CID_SKIP)
		ret = KSFT_SKIP;
	else if (!r)
		ret = KSFT_PASS;
cleanup:
	if (c)
		cg_destroy(c);
	if (base)
		cg_destroy(base);
	free(c);
	free(base);
	munmap(sh, sizeof(*sh));
	return ret;
}

/* Open the namespace file @link of @pid and read its home, as ns_cid_home(). */
static int ns_cid_home_of(pid_t pid, const char *link, unsigned long long *home)
{
	char path[64];
	int fd, ret;

	snprintf(path, sizeof(path), "/proc/%d/ns/%s", (int)pid, link);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ret = ns_cid_home(fd, home);
	close(fd);
	return ret;
}

/*
 * Run the creator of the ns_keeps_home cases. It writes one byte to @ackfd
 * after each step and waits for one byte on @cmdfd before the next one:
 *  1. Move to @l and call unshare(CLONE_NEWCID). Write 'k', or 's' on a
 *     kernel without the namespace, or 'f'.
 *  2. Move to @n. Write 'k' or 'f'.
 *  3. Exit.
 */
static int ns_keeps_creator(const char *l, const char *n, int cmdfd, int ackfd)
{
	char c;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	c = 'k';
	if (cg_enter_current(l))
		c = 'f';
	else if (unshare(CLONE_NEWCID))
		c = errno == EINVAL || errno == EPERM ? 's' : 'f';
	if (write(ackfd, &c, 1) != 1 || c != 'k' || read(cmdfd, &c, 1) != 1)
		return 1;
	c = cg_enter_current(n) ? 'f' : 'k';
	if (write(ackfd, &c, 1) != 1 || c != 'k' || read(cmdfd, &c, 1) != 1)
		return 1;
	return 0;
}

/*
 * Wait until cpu.concurrency.namespaces of @cg is empty. Return 0, or -1
 * after 10 s. The kernel may free the namespace a moment after its last
 * user is gone.
 */
static int ns_keeps_wait_empty(const char *cg)
{
	int i, found;

	for (i = 0; i < 1000; i++) {
		if (!ns_cid_listed(cg, 0, &found))
			return 0;
		usleep(10000);
	}
	return -1;
}

/*
 * Check that the home "l" of the namespace @inum cannot be removed:
 *  - rmdir(2) of "l" fails with EBUSY;
 *  - the write of "-cpu" to cgroup.subtree_control of "base" fails with
 *    EBUSY;
 *  - cpu.concurrency.namespaces of "l" still lists @inum, alone.
 * Return 0 if all three hold. Print what does not, with @when.
 */
static int ns_keeps_check(struct ns_home_param *p, long inum, const char *when)
{
	int r, n, found, ret = 0;

	if (!rmdir(p->l)) {
		fprintf(stderr, "%s: rmdir(2) of the home succeeded\n", when);
		return 1;
	}
	if (errno != EBUSY) {
		fprintf(stderr,
			"%s: rmdir(2) of the home: %s, expected EBUSY\n", when, strerror(errno));
		ret = 1;
	}
	r = cg_write(p->base, "cgroup.subtree_control", "-cpu");
	if (r != -EBUSY) {
		fprintf(stderr,
			"%s: disabling the cpu controller for the home: %s, expected EBUSY\n",
			when, r ? strerror(-r) : "succeeded");
		if (!r)
			cg_write(p->base, "cgroup.subtree_control", "+cpu");
		return 1;
	}
	n = ns_cid_listed(p->l, inum, &found);
	if (n != 1 || !found) {
		fprintf(stderr,
			"%s: cpu.concurrency.namespaces of the home has %d lines, namespace %ld %s\n",
			when, n, inum, found ? "listed" : "not listed");
		ret = 1;
	}
	return ret;
}

/*
 * A namespace keeps its home.
 *
 * Cgroups:
 *
 *   base    cgroup.subtree_control has "cpu"
 *   |- l    cpu.concurrency.max 2
 *   `- n    no limit
 *
 * Processes:
 *  - The test process stays in the root cgroup and does the checks.
 *  - The creator is a child of the test process, see ns_keeps_creator(). It
 *    never forks: the namespace has no member.
 *
 * Steps:
 *  1. The creator moves to "l" and calls unshare(CLONE_NEWCID).
 *  2. The test reads the inode number of /proc/<creator>/ns/cid_for_children.
 *     With @keep_fd, it opens that file and keeps the descriptor.
 *  3. The creator moves to "n". The test checks that cgroup.procs of "l"
 *     is empty.
 *  4. The test checks that "l" cannot be removed, see ns_keeps_check().
 *  5. The creator exits and the test reaps it.
 *
 * Without @keep_fd (test_concurrency_ns_keeps_home), nothing refers to the
 * namespace after step 5:
 *  6. The test waits until cpu.concurrency.namespaces of "l" is empty, at
 *     most 10 s. Then rmdir(2) of "l" must succeed at the first call.
 *
 * With @keep_fd (test_concurrency_ns_fd_keeps_home), the descriptor of the
 * test still refers to it:
 *  6. The test waits 500 ms, so that a kernel which frees the namespace
 *     with its creator has the time to do it. It checks again that "l"
 *     cannot be removed, and that NS_GET_CID_CGROUP on the descriptor
 *     returns the ID of "l".
 *  7. The test closes the descriptor. Then as step 6 above.
 */
static int __test_concurrency_ns_keeps_home(const char *root, int keep_fd)
{
	int cmd[2] = { -1, -1 }, ack[2] = { -1, -1 }, fd = -1, status, found;
	int ret = KSFT_FAIL, l_gone = 0;
	struct ns_home_param p = { };
	unsigned long long home = 0;
	char buf[64] = "", c = 0;
	pid_t creator = -1;
	long inum;

	p.base = cg_name(root, keep_fd ? "cc_test_ns_fd_keeps" : "cc_test_ns_keeps");
	if (!p.base)
		goto cleanup;
	p.l = cg_name(p.base, "l");
	p.n = cg_name(p.base, "n");
	if (!p.l || !p.n)
		goto cleanup;
	if (cg_create(p.base) || cg_write(p.base, "cgroup.subtree_control", "+cpu") ||
	    cg_create(p.l) || cg_create(p.n) || cg_write(p.l, "cpu.concurrency.max", "2"))
		goto cleanup;
	if (pipe(cmd) || pipe(ack))
		goto cleanup;
	creator = fork();
	if (creator < 0)
		goto cleanup;
	if (creator == 0) {
		close(cmd[1]);
		close(ack[0]);
		exit(ns_keeps_creator(p.l, p.n, cmd[0], ack[1]));
	}
	if (read(ack[0], &c, 1) != 1 || c != 'k') {
		if (c == 's')
			ret = skip("no concurrency ID namespace (CLONE_NEWCID)");
		else
			fprintf(stderr,
				"the creator could not create the namespace in the limited cgroup\n");
		goto cleanup;
	}
	inum = ns_cid_inode(creator, "cid_for_children");
	if (inum < 0 || inum == ns_cid_inode(creator, "cid")) {
		fprintf(stderr, "ns/cid_for_children of the creator: %ld, its ns/cid: %ld\n",
			inum, ns_cid_inode(creator, "cid"));
		goto cleanup;
	}
	if (keep_fd) {
		snprintf(buf, sizeof(buf), "/proc/%d/ns/cid_for_children", (int)creator);
		fd = open(buf, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "%s: %s\n", buf, strerror(errno));
			goto cleanup;
		}
	}
	if (ns_cid_home_of(creator, "cid_for_children", &home)) {
		fprintf(stderr, "ioctl(NS_GET_CID_CGROUP) on the namespace of the creator: %s\n",
			strerror(errno));
		goto cleanup;
	}
	if (!home || home != ns_cid_cgroup_id(p.l)) {
		fprintf(stderr,
			"NS_GET_CID_CGROUP after unshare(2) in the limited cgroup: %llu, expected %llu\n",
			home, ns_cid_cgroup_id(p.l));
		goto cleanup;
	}
	if (write(cmd[1], "m", 1) != 1 || read(ack[0], &c, 1) != 1 || c != 'k') {
		fprintf(stderr, "the creator could not leave the limited cgroup\n");
		goto cleanup;
	}
	buf[0] = 0;
	if (cg_read(p.l, "cgroup.procs", buf, sizeof(buf)) || buf[0]) {
		fprintf(stderr, "cgroup.procs of the home is not empty: %s\n", buf);
		goto cleanup;
	}
	if (ns_keeps_check(&p, inum, "the creator left the home")) {
		if (access(p.l, F_OK))
			l_gone = 1;
		goto cleanup;
	}
	if (write(cmd[1], "x", 1) != 1 || waitpid(creator, &status, 0) != creator)
		goto cleanup;
	creator = -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		goto cleanup;
	if (keep_fd) {
		usleep(500000);
		if (ns_keeps_check(&p, inum, "the creator exited, a descriptor is open")) {
			if (access(p.l, F_OK))
				l_gone = 1;
			goto cleanup;
		}
		if (ns_cid_home(fd, &home) || home != ns_cid_cgroup_id(p.l)) {
			fprintf(stderr,
				"NS_GET_CID_CGROUP on the descriptor after the creator exited: %llu, expected %llu\n",
				home, ns_cid_cgroup_id(p.l));
			goto cleanup;
		}
		close(fd);
		fd = -1;
	}
	if (ns_keeps_wait_empty(p.l)) {
		ns_cid_listed(p.l, inum, &found);
		fprintf(stderr,
			"cpu.concurrency.namespaces of the home is not empty 10 s after the last reference went away (namespace %ld %s)\n",
			inum, found ? "listed" : "not listed");
		goto cleanup;
	}
	if (rmdir(p.l)) {
		fprintf(stderr,
			"rmdir(2) of the home once the namespace is gone: %s\n", strerror(errno));
		goto cleanup;
	}
	l_gone = 1;
	ret = KSFT_PASS;
cleanup:
	if (fd >= 0)
		close(fd);
	if (creator > 0) {
		kill(creator, SIGKILL);
		waitpid(creator, &status, 0);
	}
	if (cmd[0] >= 0) {
		close(cmd[0]);
		close(cmd[1]);
	}
	if (ack[0] >= 0) {
		close(ack[0]);
		close(ack[1]);
	}
	if (p.l && !l_gone && ns_cid_destroy_home(p.l) && ret == KSFT_PASS)
		ret = KSFT_FAIL;
	if (p.n)
		cg_destroy(p.n);
	if (p.base)
		cg_destroy(p.base);
	ns_home_param_free(&p);
	return ret;
}

static int test_concurrency_ns_keeps_home(const char *root)
{
	return __test_concurrency_ns_keeps_home(root, 0);
}

/*
 * A file descriptor on the namespace keeps the home after the creator
 * exited, until it is closed. See __test_concurrency_ns_keeps_home().
 */
static int test_concurrency_ns_fd_keeps_home(const char *root)
{
	return __test_concurrency_ns_keeps_home(root, 1);
}

/*
 * test_concurrency_dormant() runs dormant_run() in a cgroup limited to
 * 1. A thread pins itself to the last CPU, runs there for 50 ms, then
 * sleeps on a pipe. Two unpinned workers spin and nap. The last CPU goes
 * offline for a second and comes back. Then the sleeper is woken.
 *  - The unplug must not widen the mask of the sleeper. A sleeping task
 *    keeps its affinity, as it does without a limit.
 *    select_fallback_rq() widens it only if the task wakes up while the
 *    CPU is away.
 *  - The workers must not be held while the CPU is offline. No CPU can
 *    serve the mask of the sleeper then.
 *  - Once the CPU is back, the sleeper must run within 3 s of its
 *    wakeup, with its mask of one CPU. The workers must still run.
 * Skipped when the CPU cannot go offline through sysfs.
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

/* Return true if both workers completed a round within @ms. */
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
		ret = skip("CPU %d cannot go offline", d.cpu);
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

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE	6
#endif /* !SCHED_DEADLINE */

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
	unsigned long *ran = arg;

	if (set_deadline(0, 2000, 10000))
		return skip("SCHED_DEADLINE admission refused");
	for (;;) {
		spin_ns(1000000);
		__atomic_fetch_add(ran, 1, __ATOMIC_RELAXED);
		usleep(3000);
	}
	return 0;
}

#define RT_RUNTIME	"/proc/sys/kernel/sched_rt_runtime_us"

/*
 * Read sched_rt_runtime_us as text into @buf. read_sysctl() returns -1 on
 * error, and -1 is also the value for "admission off".
 */
static int rt_runtime_read(char *buf, size_t len)
{
	int fd = open(RT_RUNTIME, O_RDONLY);
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

static int rt_runtime_write(const char *val)
{
	int fd = open(RT_RUNTIME, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, val, strlen(val));
	close(fd);
	return n == (ssize_t)strlen(val) ? 0 : -1;
}

static const char *refusal(int err)
{
	return err ? strerror(-err) : "accepted";
}

/*
 * Check the sched_conc_refuse event of a refusal. It must name the judged
 * task @pid, the operation @op, the reason @why and the error @err.
 * Return 0 if the trace buffer has it, or if the events cannot be used.
 */
static int refusal_traced(pid_t pid, const char *op, const char *why, int err)
{
	char text[256];

	snprintf(text, sizeof(text), "pid=%d op=%s why=%s err=%d", (int)pid, op, why, err);
	return conc_trace_find(text, "sched_conc_refuse:");
}

/*
 * No SCHED_DEADLINE task is allowed in a limited subtree. The deadline
 * class admits bandwidth against every CPU of the root domain. A limit
 * leaves a task fewer CPUs, as an affinity narrower than the root domain
 * would.
 * lim has the limit 1 and x spins in it. free has the child sub. d is a
 * SCHED_DEADLINE task in sub.
 *  - x cannot become SCHED_DEADLINE (EPERM).
 *  - d cannot move into lim (EBUSY).
 *  - A limit of 1 cannot be written to free (EBUSY). Its
 *    cpu.concurrency.max still reads "max".
 *  - Once d has left SCHED_DEADLINE, the limit and the move are
 *    accepted.
 * Each refusal must show in the sched_conc_refuse event with its task,
 * its operation, its reason and its error. This is checked when tracefs
 * can be used.
 * Skipped with deadline admission off, where nothing is refused, and
 * without the privilege or the bandwidth for SCHED_DEADLINE.
 */
static int test_concurrency_deadline(const char *root)
{
	struct sched_param other = { .sched_priority = 0 };
	char *lim = NULL, *free_cg = NULL, *sub = NULL, rt[32];
	int ret = KSFT_FAIL, status, err;
	unsigned long ran = 0;
	pid_t x = -1, d = -1;

	if (rt_runtime_read(rt, sizeof(rt)))
		return skip("no %s", RT_RUNTIME);
	if (atol(rt) < 0)
		return skip("deadline admission control is off");
	lim = cg_name(root, "cc_test_dl_lim");
	free_cg = cg_name(root, "cc_test_dl_free");
	sub = free_cg ? cg_name(free_cg, "sub") : NULL;
	if (!lim || !free_cg || !sub || cg_create(lim) || cg_create(free_cg) ||
	    cg_write(free_cg, "cgroup.subtree_control", "+cpu") || cg_create(sub))
		goto cleanup;
	if (cg_write(lim, "cpu.concurrency.max", "1"))
		goto cleanup;
	x = cg_run_nowait(lim, busy_forever, NULL);
	d = cg_run_nowait(sub, deadline_task, &ran);
	if (x < 0 || d < 0)
		goto cleanup;
	usleep(200000);
	if (waitpid(d, &status, WNOHANG) == d) {
		d = -1;
		if (WIFEXITED(status) && WEXITSTATUS(status) == KSFT_SKIP)
			ret = KSFT_SKIP;
		goto cleanup;
	}

	conc_trace_start();
	conc_trace_clear();
	err = set_deadline(x, 2000, 10000);
	if (err != -EPERM) {
		fprintf(stderr, "a task under a limit made SCHED_DEADLINE: %s, not EPERM\n",
			refusal(err));
		goto cleanup;
	}
	if (refusal_traced(x, "sched_setattr", "SCHED_DEADLINE under a limit", -EPERM))
		goto cleanup;
	err = cg_enter(lim, d);
	if (err != -EBUSY) {
		fprintf(stderr, "a SCHED_DEADLINE task moved under a limit: %s, not EBUSY\n",
			refusal(err));
		goto cleanup;
	}
	if (refusal_traced(d, "cgroup migration", "a SCHED_DEADLINE task", -EBUSY))
		goto cleanup;
	err = cg_write(free_cg, "cpu.concurrency.max", "1");
	if (err != -EBUSY) {
		fprintf(stderr, "a limit started over a SCHED_DEADLINE task: %s, not EBUSY\n",
			refusal(err));
		goto cleanup;
	}
	if (refusal_traced(d, "cpu.concurrency.max", "a SCHED_DEADLINE task", -EBUSY))
		goto cleanup;
	if (cg_read_strcmp(free_cg, "cpu.concurrency.max", "max\n")) {
		fprintf(stderr, "the refused limit over a SCHED_DEADLINE task was set\n");
		goto cleanup;
	}

	/* Out of SCHED_DEADLINE, it may be limited and move. */
	if (sched_setscheduler(d, SCHED_OTHER, &other))
		goto cleanup;
	if (cg_write(free_cg, "cpu.concurrency.max", "1")) {
		fprintf(stderr, "a limit over a task out of SCHED_DEADLINE refused\n");
		goto cleanup;
	}
	if (cg_enter(lim, d)) {
		fprintf(stderr, "a task out of SCHED_DEADLINE refused under a limit\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	conc_trace_stop();
	if (d > 0) {
		kill(d, SIGKILL);
		sched_setscheduler(d, SCHED_OTHER, &other);
		waitpid(d, NULL, 0);
	}
	if (x > 0) {
		/* Made SCHED_DEADLINE where the refusal is missing. */
		kill(x, SIGKILL);
		sched_setscheduler(x, SCHED_OTHER, &other);
		waitpid(x, NULL, 0);
	}
	if (sub)
		cg_destroy(sub);
	if (free_cg)
		cg_destroy(free_cg);
	if (lim)
		cg_destroy(lim);
	free(sub);
	free(free_cg);
	free(lim);
	return ret;
}

/*
 * test_concurrency_deadline_unadmitted() runs this with deadline
 * admission off. full has the limit 1, and a task spins on its one CPU.
 * dl is a SCHED_DEADLINE task in src. 60 times, dl is moved into full,
 * then back to src. In full it arrives on another CPU and finds no slot.
 * It is held at its return to user space, then placed on full's CPU.
 * Every third round its policy is set to SCHED_OTHER and back while it
 * is in full.
 * The deadline class keeps bandwidth per runqueue. It warns when the
 * bandwidth does not follow such a move. The kernel must not warn, and dl
 * must still run at the end. This needs "sched/deadline: Do
 * not lose a throttled task switched out and back". Without it, a task
 * which is throttled when its policy is switched away is never
 * replenished if it is switched back before its 0-lag time.
 * main() fails the case when the kernel warns while it runs.
 * Skipped without the privilege for SCHED_DEADLINE.
 */
static int deadline_moves(const char *root)
{
	struct sched_param other = { .sched_priority = 0 };
	char *full = NULL, *src = NULL;
	int ret = KSFT_FAIL, i, status, err;
	unsigned long *ran, seen;
	pid_t busy = -1, dl = -1;

	if (need_cpus(2))
		return KSFT_SKIP;
	ran = mmap(NULL, sizeof(*ran), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (ran == MAP_FAILED)
		return KSFT_FAIL;
	full = cg_name(root, "cc_test_dl_full");
	src = cg_name(root, "cc_test_dl_src");
	if (!full || !src || cg_create(full) || cg_create(src))
		goto cleanup;
	if (cg_write(full, "cpu.concurrency.max", "1"))
		goto cleanup;
	busy = cg_run_nowait(full, busy_forever, NULL);
	dl = cg_run_nowait(src, deadline_task, ran);
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
		if (cg_enter(full, dl))
			goto cleanup;
		if (i % 3 == 1) {
			sched_setscheduler(dl, SCHED_OTHER, &other);
			err = set_deadline(dl, 2000, 10000);
			if (err) {
				fprintf(stderr, "switch back to SCHED_DEADLINE refused: %s\n",
					strerror(-err));
				goto cleanup;
			}
		}
		usleep(30000 + (i % 7) * 10000);
		if (cg_enter(src, dl))
			goto cleanup;
		usleep(20000 + (i % 5) * 10000);
	}
	/* It runs 1ms every 4ms, throttled 10ms at most: give it a second. */
	seen = __atomic_load_n(ran, __ATOMIC_RELAXED);
	for (i = 0; i < 100 && __atomic_load_n(ran, __ATOMIC_RELAXED) == seen; i++)
		usleep(10000);
	if (__atomic_load_n(ran, __ATOMIC_RELAXED) == seen) {
		fprintf(stderr, "the deadline task never ran again after its policy switches\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (dl > 0) {
		kill(dl, SIGKILL);
		/*
		 * Set to SCHED_OTHER, a task left throttled is requeued and
		 * can die.
		 */
		sched_setscheduler(dl, SCHED_OTHER, &other);
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
	munmap(ran, sizeof(*ran));
	return ret;
}

static int test_concurrency_deadline_unadmitted(const char *root)
{
	char saved[32];
	int ret;

	if (rt_runtime_read(saved, sizeof(saved)))
		return skip("no %s", RT_RUNTIME);
	if (rt_runtime_write("-1"))
		return skip("deadline admission control cannot be turned off");
	ret = deadline_moves(root);
	if (rt_runtime_write(saved)) {
		fprintf(stderr, "%s not restored to %s", RT_RUNTIME, saved);
		ret = KSFT_FAIL;
	}
	return ret;
}

/*
 * A partition root with a limit is a deadline domain. Its deadline tasks
 * are admitted against the limit, not against its number of CPUs. Each
 * CPU keeps the share of its fair server.
 * P is a partition root on the last 4 CPUs, with the limit 2 and two
 * children, a and c. Five tasks sleep in a: t0 to t3 and f. A deadline
 * task below takes 50% of a CPU. The steps:
 *  - t0, t1 and t2 become SCHED_DEADLINE: admitted (1.5 CPUs);
 *  - t3 fails with EBUSY (2.0 CPUs). With the limit raised to 3 it is
 *    admitted;
 *  - the limit cannot go back to 2 (EBUSY);
 *  - f is pinned to the last CPU of P: accepted;
 *  - t0 moves to c. A limit of 1 on c fails with EBUSY;
 *  - P cannot become a member again (EBUSY);
 *  - t3 leaves the policy. The limit of 2 is then accepted;
 *  - t0, t1 and t2 leave the policy. f is unpinned and pinned to the
 *    last CPU again. t1 then becomes SCHED_DEADLINE again: admitted
 *    beside the pinned fair task.
 * The results hold for a fair server share from 0 to 20%. The case is
 * skipped:
 *  - with fewer than 6 CPUs;
 *  - with deadline admission off;
 *  - without a partition root;
 *  - when P takes no deadline task before it has a limit.
 */
static int sleep_forever(const char *cgroup, void *arg);
static int dl_admitted_in(const char *cg);

#define DL_DOM_TASKS	4

static int test_concurrency_deadline_domain(const char *root)
{
	struct sched_param other = { .sched_priority = 0 };
	int ret = KSFT_FAIL, n = get_nprocs(), i, err;
	char *P = NULL, *a = NULL, *c = NULL, rt[32], buf[32];
	pid_t t[DL_DOM_TASKS], f = -1;
	cpu_set_t set, all;

	for (i = 0; i < DL_DOM_TASKS; i++)
		t[i] = -1;
	if (need_cpus(6))
		return KSFT_SKIP;
	if (rt_runtime_read(rt, sizeof(rt)) || atol(rt) < 0)
		return skip("deadline admission control is off");
	P = cg_name(root, "cc_test_dl_dom");
	a = P ? cg_name(P, "a") : NULL;
	c = P ? cg_name(P, "c") : NULL;
	if (!P || !a || !c || cg_create(P))
		goto cleanup;
	snprintf(buf, sizeof(buf), "%d-%d", n - 4, n - 1);
	if (cg_write(P, "cpuset.cpus", buf) || cg_write(P, "cpuset.cpus.partition", "root") ||
	    cg_read_strcmp(P, "cpuset.cpus.partition", "root\n")) {
		ret = skip("no partition root on CPUs %s", buf);
		goto cleanup;
	}
	if (cg_write(P, "cgroup.subtree_control", "+cpuset +cpu") || cg_create(a) ||
	    cg_create(c))
		goto cleanup;
	/*
	 * Skip only when P takes no deadline task before it has a limit.
	 * Under the limit, a refusal of the first task is a failure.
	 */
	err = dl_admitted_in(a);
	if (err) {
		ret = skip("no SCHED_DEADLINE task in a partition root without a limit: %s",
			   strerror(-err));
		goto cleanup;
	}
	if (cg_write(P, "cpu.concurrency.max", "2"))
		goto cleanup;
	for (i = 0; i < DL_DOM_TASKS; i++) {
		t[i] = cg_run_nowait(a, sleep_forever, NULL);
		if (t[i] < 0)
			goto cleanup;
	}
	f = cg_run_nowait(a, sleep_forever, NULL);
	if (f < 0)
		goto cleanup;
	usleep(100000);

	/* Against 2 CPUs, not 4. */
	for (i = 0; i < 3; i++) {
		err = set_deadline(t[i], 5000, 10000);
		if (err) {
			fprintf(stderr, "deadline task %d of the domain refused: %s\n",
				i, strerror(-err));
			goto cleanup;
		}
	}
	err = set_deadline(t[3], 5000, 10000);
	if (err != -EBUSY) {
		fprintf(stderr, "2.0 CPUs of deadline tasks under a limit of 2: %s, not EBUSY\n",
			refusal(err));
		goto cleanup;
	}
	if (cg_write(P, "cpu.concurrency.max", "3") || set_deadline(t[3], 5000, 10000)) {
		fprintf(stderr, "2.0 CPUs of deadline tasks refused under a limit of 3\n");
		goto cleanup;
	}
	err = cg_write(P, "cpu.concurrency.max", "2");
	if (err != -EBUSY || cg_read_strcmp(P, "cpu.concurrency.max", "3\n")) {
		fprintf(stderr, "a limit lowered under the deadline bandwidth: %s, not EBUSY\n",
			refusal(err));
		goto cleanup;
	}

	/*
	 * While P holds deadline tasks: a fair task may be pinned, no limit
	 * starts over one of them, and P stays a partition root.
	 */
	CPU_ZERO(&set);
	CPU_SET(n - 1, &set);
	err = sched_setaffinity(f, sizeof(set), &set) ? -errno : 0;
	if (err) {
		fprintf(stderr, "a fair task pinned to one CPU of a deadline domain: %s\n",
			refusal(err));
		goto cleanup;
	}
	err = cg_enter(c, t[0]);
	if (err) {
		fprintf(stderr, "a deadline task refused within its domain: %s\n", refusal(err));
		goto cleanup;
	}
	err = cg_write(c, "cpu.concurrency.max", "1");
	if (err != -EBUSY) {
		fprintf(stderr, "a limit over a deadline task in its domain: %s, not EBUSY\n",
			refusal(err));
		goto cleanup;
	}
	err = cg_write(P, "cpuset.cpus.partition", "member");
	if (err != -EBUSY) {
		fprintf(stderr, "a deadline domain made a member: %s, not EBUSY\n", refusal(err));
		goto cleanup;
	}

	/* One leaves the policy: the limit may drop to 2 (1.5 CPUs). */
	if (sched_setscheduler(t[3], SCHED_OTHER, &other) ||
	    cg_write(P, "cpu.concurrency.max", "2")) {
		fprintf(stderr, "a limit refused over 1.5 CPUs of deadline tasks\n");
		goto cleanup;
	}

	/*
	 * No deadline task is left. Pin f again, then admit a deadline task
	 * beside it.
	 */
	for (i = 0; i < 3; i++) {
		if (sched_setscheduler(t[i], SCHED_OTHER, &other))
			goto cleanup;
	}
	usleep(50000);
	stress_all_cpus(&all);
	if (sched_setaffinity(f, sizeof(all), &all) || sched_setaffinity(f, sizeof(set), &set)) {
		fprintf(stderr, "a fair task pinned with no deadline task: %s\n", strerror(errno));
		goto cleanup;
	}
	err = set_deadline(t[1], 5000, 10000);
	if (err) {
		fprintf(stderr, "a deadline task beside a pinned fair task: %s\n", refusal(err));
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	for (i = 0; i < DL_DOM_TASKS; i++) {
		if (t[i] > 0) {
			kill(t[i], SIGKILL);
			sched_setscheduler(t[i], SCHED_OTHER, &other);
			waitpid(t[i], NULL, 0);
		}
	}
	if (f > 0) {
		kill(f, SIGKILL);
		waitpid(f, NULL, 0);
	}
	if (P) {
		cg_write(P, "cpu.concurrency.max", "max");
		cg_write(P, "cpuset.cpus.partition", "member");
	}
	if (a)
		cg_destroy(a);
	if (c)
		cg_destroy(c);
	if (P)
		cg_destroy(P);
	free(a);
	free(c);
	free(P);
	return ret;
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

/* Parse a cpulist such as "1-2,5" into @set. Return 0, or -1 on error. */
static int parse_cpulist(const char *list, cpu_set_t *set)
{
	const char *p = list;
	char *end;

	CPU_ZERO(set);
	while (*p && *p != '\n') {
		long a = strtol(p, &end, 10), b = a;

		if (end == p)
			return -1;
		p = end;
		if (*p == '-') {
			b = strtol(p + 1, &end, 10);
			p = end;
		}
		for (; a <= b; a++)
			CPU_SET(a, set);
		if (*p == ',')
			p++;
	}
	return 0;
}

static int test_concurrency_isolated(const char *root)
{
	int ret = KSFT_FAIL, n = get_nprocs(), c0 = -1, iso = -1, hk = -1, cpu, fd, i;
	cpu_set_t isolated, set;
	char *g = NULL, buf[256];
	pid_t x = -1;
	ssize_t len;

	fd = open("/sys/devices/system/cpu/isolated", O_RDONLY);
	if (fd < 0)
		return skip("no /sys/devices/system/cpu/isolated");
	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[len > 0 ? len : 0] = 0;
	if (parse_cpulist(buf, &isolated))
		return skip("unreadable isolated CPUs: %s", buf);
	for (i = 0; i < n && c0 < 0; i++)
		if (!CPU_ISSET(i, &isolated))
			c0 = i;
	for (i = 0; i < n && iso < 0; i++)
		if (CPU_ISSET(i, &isolated))
			iso = i;
	for (i = iso + 1; iso >= 0 && i < n && hk < 0; i++)
		if (!CPU_ISSET(i, &isolated) && i != c0)
			hk = i;
	if (c0 < 0 || iso < 0 || hk < 0)
		return skip("no isolated CPU below a second housekeeping CPU (isolcpus=domain)");

	g = cg_name(root, "cc_test_isolated");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	x = cg_run_nowait(g, busy_forever, NULL);
	if (x < 0)
		goto cleanup;
	CPU_ZERO(&set);
	CPU_SET(c0, &set);
	if (sched_setaffinity(x, sizeof(set), &set))
		goto cleanup;
	usleep(200000);

	CPU_ZERO(&set);
	CPU_SET(iso, &set);
	CPU_SET(hk, &set);
	if (sched_setaffinity(x, sizeof(set), &set)) {
		fprintf(stderr, "the mask {%d,%d} refused: %s\n", iso, hk, strerror(errno));
		goto cleanup;
	}
	usleep(300000);
	cpu = stat_cpu(x);
	if (cpu != hk) {
		fprintf(stderr, "given {%d,%d}, %d isolated, the group moved to CPU %d, not %d\n",
			iso, hk, iso, cpu, hk);
		goto cleanup;
	}

	CPU_ZERO(&set);
	CPU_SET(iso, &set);
	if (sched_setaffinity(x, sizeof(set), &set)) {
		fprintf(stderr, "the mask {%d} refused: %s\n", iso, strerror(errno));
		goto cleanup;
	}
	usleep(300000);
	cpu = stat_cpu(x);
	if (cpu != iso) {
		fprintf(stderr, "given {%d} alone, the group runs on CPU %d\n", iso, cpu);
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (x > 0) {
		kill(x, SIGKILL);
		waitpid(x, NULL, 0);
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

static int sleep_forever(const char *cgroup, void *arg)
{
	for (;;)
		pause();
	return 0;
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
 * Send SIGKILL to @pid and reap it. Return 0 if it died within @ms, else
 * -ETIMEDOUT.
 */
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
 * The removal of a partition root widens the cpuset of a limited cgroup
 * beside it.
 *
 * Cgroups: po, a partition root on CPUs 1 to n-1, and its children:
 *  - p, a partition root on CPUs n-2 and n-1. It is empty;
 *  - a, which names CPU n-3. x spins in it;
 *  - b, which names the CPUs of p. They are taken, so b runs on the CPUs
 *    of po without them. z spins and s sleeps in b.
 * The steps, all done by the test:
 *  1. Write 1 to cpu.concurrency.max of po.
 *  2. Make p a member, then a partition root again.
 *  3. Remove p. b gets the CPUs n-2 and n-1 which it names. x and z then
 *     have no CPU in common, and share the one slot of po.
 *
 * Checked:
 *  - every write of steps 1 and 2 and the removal are accepted;
 *  - 300 ms after the removal, cpu.concurrency.effective of po reads 1;
 *  - x and z both get CPU time during the next second: ten rotation
 *    periods;
 *  - z and s each die within 1 s of SIGKILL.
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
		ret = skip("no partition root on CPUs %s", buf);
		goto cleanup;
	}
	if (cg_write(po, "cgroup.subtree_control", "+cpuset") ||
	    cg_create(p) || cg_create(a) || cg_create(b))
		goto cleanup;
	snprintf(pcpus, sizeof(pcpus), "%d-%d", last - 1, last);
	if (cg_write(p, "cpuset.cpus", pcpus) || cg_write(p, "cpuset.cpus.partition", "root")) {
		ret = skip("no partition root on CPUs %s", pcpus);
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

	if (expect_write(po, "cpu.concurrency.max", "1", 0, "a limit over b, naming p's CPUs") ||
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
	usleep(1000000);
	x1 = proc_cpu_ms(x);
	z1 = proc_cpu_ms(z);
	if (x1 <= x0 || z1 <= z0) {
		fprintf(stderr, "after the rmdir, in 1 s x ran %ld ms, z %ld ms\n",
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
 * Enabling the cpu controller on existing cgroups gives each one the
 * ceiling in its parent's cpu.concurrency.child_ceiling.
 * p names CPU 0 and has a cpu.concurrency.child_ceiling of 1. Its child
 * x names the last CPU, which p does not have. "+cpu" is written to
 * cgroup.subtree_control of p. The write must be accepted, and x must
 * read the ceiling 1.
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
	if (expect_write(p, "cgroup.subtree_control", "+cpu", 0,
			 "the cpu controller over x, naming a CPU p does not have"))
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
 * Copy the value of @field of /proc/<pid>/status into @buf, without its
 * newline. Return @buf, or NULL when the field is not found.
 */
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
 * An affinity change to an offline CPU is refused at once. g has the
 * limit 1. x spins in it. CPU c, the last one, goes offline. x is pinned
 * to {c} 50 times. Each call must fail with EINVAL, and return within
 * 1 s. The refusal needs no room, so it waits for nothing. A kernel which
 * tries to make room for the offline CPU sleeps for seconds in each call.
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
		ret = skip("CPU %d cannot go offline", c);
		goto cleanup;
	}
	offline = 1;
	for (i = 0; i < 50; i++) {
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (expect_pin(x, c, c, EINVAL, "x pinned to an offline CPU"))
			goto cleanup;
		ms = ns_since(&t0) / 1000000;
		if (ms >= 1000) {
			fprintf(stderr, "refused change %d took %lld ms\n", i, ms);
			goto cleanup;
		}
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

static int test_concurrency_setmove_mask(const char *root)
{
	static const int w1m[] = { 5, -1 }, w2m[] = { 3, -1 };
	char *g = NULL, *p = NULL, *a = NULL, *b = NULL;
	pid_t t1 = -1, t2 = -1, w1 = -1, w2 = -1, pid[4];
	int ret = KSFT_FAIL, offline = 0, err, i;

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
	    cg_write(p, "cpu.concurrency.max", "3"))
		goto cleanup;
	t1 = cg_run_nowait(g, busy_forever, NULL);
	t2 = cg_run_nowait(a, busy_forever, NULL);
	w1 = cg_run_nowait(b, busy_forever, NULL);
	w2 = cg_run_nowait(b, busy_forever, NULL);
	if (t1 < 0 || t2 < 0 || w1 < 0 || w2 < 0)
		goto cleanup;
	err = pin_cpus(w1, w1m);
	if (!err)
		err = pin_cpus(w2, w2m);
	if (err) {
		fprintf(stderr, "pinning before the test: %s\n", strerror(err));
		goto cleanup;
	}
	usleep(100000);
	if (write_cpu_online(2, 0)) {
		ret = skip("CPU 2 cannot go offline");
		goto cleanup;
	}
	offline = 1;
	usleep(50000);

	if (expect_write(g, "cpuset.cpus", "2-4", 0, "g's cpuset to 2-4"))
		goto cleanup;
	/* The write moves t1 before it returns. Allow 2 s for the stopper. */
	for (i = 0; i < 200 && stat_cpu(t1) != 3 && stat_cpu(t1) != 4; i++)
		usleep(10000);
	if (i == 200) {
		fprintf(stderr, "t1 is on CPU %d after g's cpuset became 2-4, CPU 2 offline\n",
			stat_cpu(t1));
		goto cleanup;
	}

	if (expect_write(a, "cpuset.cpus", "2,5", 0, "a's cpuset to {2,5}"))
		goto cleanup;
	for (i = 0; i < 200 && stat_cpu(t2) != 5; i++)
		usleep(10000);
	if (i == 200) {
		fprintf(stderr, "t2 is on CPU %d after a's cpuset became {2,5}, CPU 2 offline\n",
			stat_cpu(t2));
		goto cleanup;
	}
	pid[0] = t1;
	pid[1] = t2;
	pid[2] = w1;
	pid[3] = w2;
	i = procs_stuck(pid, 4, 2000);
	if (i >= 0) {
		fprintf(stderr, "spinner %d got no CPU time in 2 s after the cpuset writes\n", i);
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

/*
 * Making room for a set-move by another set-move. p has the limit 2 and
 * three children, each with one spinning task:
 *  - U in u, pinned to {0};
 *  - S in s, pinned to {1}, then to {0-3};
 *  - T in t, pinned to {0}.
 * p uses CPUs 0 and 1. T is then pinned to {4}. Two slots still serve
 * the three tasks: one on CPU 4 for T, and one on CPU 0 for U and S. t's
 * presence must move from CPU 0 to CPU 4. p does not use CPU 4 and has
 * no room for it, because U stays on CPU 0. p must give up CPU 1. S
 * cannot join another CPU of its own cgroup, so s is set-moved to CPU 0,
 * inside the first set-move.
 * The pin must succeed. T must end on CPU 4, S and U on CPU 0, within
 * 1 s. nr_rotations of p must still be 0.
 */
static int test_concurrency_headroom_setmove(const char *root)
{
	static const int zero[] = { 0, -1 }, one[] = { 1, -1 }, four[] = { 4, -1 },
			 low[] = { 0, 1, 2, 3, -1 };
	char *p = NULL, *cu = NULL, *cs = NULL, *ct = NULL;
	int ret = KSFT_FAIL, err, i, cpu_t = -1, cpu_s = -1, cpu_u = -1;
	pid_t u = -1, s = -1, t = -1;
	struct conc_stat st = { };

	if (need_cpus(5))
		return KSFT_SKIP;
	p = cg_name(root, "cc_test_hsm");
	cu = cg_name(p, "u");
	cs = cg_name(p, "s");
	ct = cg_name(p, "t");
	if (!p || !cu || !cs || !ct || cg_create(p) ||
	    cg_write(p, "cgroup.subtree_control", "+cpu") || cg_create(cu) ||
	    cg_create(cs) || cg_create(ct) || cg_write(p, "cpu.concurrency.max", "2"))
		goto cleanup;
	u = cg_run_nowait(cu, busy_forever, NULL);
	s = cg_run_nowait(cs, busy_forever, NULL);
	t = cg_run_nowait(ct, busy_forever, NULL);
	if (u < 0 || s < 0 || t < 0)
		goto cleanup;
	err = pin_cpus(u, zero);
	if (!err)
		err = pin_cpus(s, one);
	if (!err)
		err = pin_cpus(t, zero);
	if (!err) {
		usleep(100000);
		err = pin_cpus(s, low);
	}
	if (err) {
		fprintf(stderr, "pinning before the test: %s\n", strerror(err));
		goto cleanup;
	}
	usleep(100000);
	if (stat_cpu(s) != 1 || stat_cpu(t) != 0 || stat_cpu(u) != 0) {
		fprintf(stderr, "before the change: s on %d, t on %d, u on %d; want 1, 0, 0\n",
			stat_cpu(s), stat_cpu(t), stat_cpu(u));
		goto cleanup;
	}
	err = pin_cpus(t, four);
	if (err) {
		fprintf(stderr, "t's task pinned to {4}: %s, not accepted\n", strerror(err));
		goto cleanup;
	}
	for (i = 0; i < 100; i++) {
		cpu_t = stat_cpu(t);
		cpu_s = stat_cpu(s);
		cpu_u = stat_cpu(u);
		if (cpu_t == 4 && cpu_s == 0 && cpu_u == 0)
			break;
		usleep(10000);
	}
	if (i == 100) {
		fprintf(stderr, "after the change: t on %d, s on %d, u on %d; want 4, 0, 0\n",
			cpu_t, cpu_s, cpu_u);
		goto cleanup;
	}
	if (conc_stat_read(p, &st) || st.nr_rotations) {
		fprintf(stderr, "nr_rotations is %lld, the tasks fit in two slots\n",
			st.nr_rotations);
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (p)
		cg_write(p, "cpu.concurrency.max", "max");
	if (u > 0) {
		kill(u, SIGKILL);
		waitpid(u, NULL, 0);
	}
	if (s > 0) {
		kill(s, SIGKILL);
		waitpid(s, NULL, 0);
	}
	if (t > 0) {
		kill(t, SIGKILL);
		waitpid(t, NULL, 0);
	}
	if (cu)
		cg_destroy(cu);
	if (cs)
		cg_destroy(cs);
	if (ct)
		cg_destroy(ct);
	if (p)
		cg_destroy(p);
	free(cu);
	free(cs);
	free(ct);
	free(p);
	return ret;
}

/*
 * State shared with the process of one round of
 * test_concurrency_cid_decrease().
 */
struct cid_dec {
	volatile long long max_cid;	/* -1 for none */
	volatile int go, ready, done, stop, at1;
	char cg[256];
};

static struct cid_dec *cd;

/* Return 1 if @cg's effective value reads 1 within a second, else 0. */
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

/*
 * Sleep, so that the thread is scheduled in again under the current
 * bound. Then record its ID.
 */
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

/*
 * The second thread of the process. Write the decrease, wait until the
 * effective value reads 1, then record the ID of this thread.
 */
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

/*
 * The process of one round. With @arg set, it has two threads and one of
 * them writes the decrease. Without, it has three threads, then starts a
 * fourth after the decrease.
 */
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
 * After a decrease of the limit, no thread gets an ID at or above it at
 * its next schedule-in. g gets the limit 2 at the start of each round. A
 * new process then runs in g. The limit is lowered to 1, and the round
 * waits until the effective value reads 1. Two kinds of round, 60 of
 * each:
 *  - The process has two threads, so each holds an ID of its own. The
 *    second thread writes the decrease. Then each thread sleeps and
 *    reads its ID.
 *  - The process has three threads. This is more than the IDs below 2,
 *    so the CPUs hold the IDs. The harness writes the decrease. The
 *    process then creates a fourth thread, which reads its ID at its
 *    start and again after a sleep.
 * Every ID read must be 0.
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
	volatile int at1, stop, ready, bad;
	volatile int sampled;		/* samples taken while the limit was 1 */
	volatile int judged;		/* those of them which read the value 1 */
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

/*
 * The process of c0. Start @arg threads which sleep. A bound update
 * visits each of them.
 */
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
 * Loop: run for a millisecond, read the effective value, sleep, then
 * read the ID given at the schedule-in. A sample counts only when the
 * harness had set the limit to 1 before and after it. Otherwise the
 * limit may have gone up between the read of the file and the
 * schedule-in. A sample is judged when the file read 1: only then does
 * it show that a decrease reached the IDs.
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
		__sync_fetch_and_add(&ei->sampled, 1);
		if (v == 1)
			__sync_fetch_and_add(&ei->judged, 1);
		if (cid >= (__u32)v) {
			__sync_fetch_and_add(&ei->bad, 1);
			ei->bad_cid = cid;
		}
	}
	return NULL;
}

/* The process of one round. Its two threads are readers. */
static int eff_ids_proc(const char *cgroup, void *arg)
{
	pthread_t t;

	pthread_create(&t, NULL, eff_ids_reader, NULL);
	eff_ids_reader(NULL);
	pthread_join(t, NULL);
	return 0;
}

/*
 * cpu.concurrency.effective shows a decrease only once the ID bounds
 * follow it. So a thread which reads the file, and is scheduled in after
 * that, gets an ID below the value it read.
 * g has the limit 2 and two children. c0 is created first. It holds a
 * process of 1000 sleeping threads. The bounds of a subtree are applied
 * one after the other, c0's before c1's, so the update takes time before
 * it reaches c1. In each of 20 rounds, a new process runs in c1. Its two
 * threads loop: run 1 ms, read c1's effective value, sleep, read their
 * ID. The harness writes a limit of 1 to g, waits 100 ms, then writes 2
 * again. Of the samples taken while the harness had the limit at 1, no
 * ID may be at or above the value read before it. At least one of these
 * samples must have read the value 1.
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
		fprintf(stderr, "the effective value never read 1 in %d samples taken under the limit 1\n",
			ei->sampled);
		goto cleanup;
	}
	ksft_print_msg("%d samples under the limit 1, %d of them after the effective value read 1\n",
		       ei->sampled, ei->judged);
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

/*
 * Shared state of the cases which check that two tasks running at once
 * never read the same ID: ids_unique, limit_raise_ids and
 * ids_complete_after_rounds. It lives in a shared mapping.
 *
 * Each thread adds 1 to the entry of its ID in a table, with
 * lib_rseq_count_by_id(). The table of mm_cid belongs to one process. The
 * table of ns_cid belongs to one namespace. A thread adds its number of
 * successful calls to @counted when it ends. The sum of the tables can
 * then be below @counted. If so, two threads wrote one entry at the same
 * time. They ran at once with the same ID.
 */
#define IDU_IDS		1024	/* entries of a table */
#define IDU_SLOTS	16	/* threads which the harness watches */
#define IDU_PROCS	4	/* processes, each with a table of mm_cid */
#define IDU_MM		LIB_RSEQ_MM_CID
#define IDU_NS		LIB_RSEQ_NS_CID
struct idu_slot {
	unsigned long ids_seen;		/* bit i: read ID i; the harness clears it */
	volatile unsigned long loops;
	volatile int ack;		/* the last epoch after which it slept */
};

struct idu_shared {
	volatile int stop;
	volatile int up;		/* watched threads which run */
	volatile int noext;		/* a thread has no rseq area with its IDs */
	volatile int epoch;		/* see idu_thread() */
	volatile int nap_us;		/* see idu_thread() */
	int nthreads;			/* threads of idu_spin_proc() */
	unsigned int limit_ids;		/* every ID must be below this */
	int bad;			/* 1 + IDU_MM or IDU_NS: an ID was not */
	unsigned long bad_reads;	/* how many reads were not */
	volatile unsigned int bad_id;	/* the first one */
	volatile unsigned long bad_loops;	/* loops of its thread before it */
	volatile int bad_slot;		/* slot of its thread, -1 if not watched */
	unsigned long counted[2];	/* by IDU_MM and IDU_NS */
	unsigned long churned;		/* threads and processes which ended */
	volatile unsigned long writes;	/* limit writes which returned */
	struct idu_slot slot[IDU_SLOTS];
	unsigned long mm_table[IDU_PROCS][IDU_IDS];
	unsigned long ns_table[IDU_IDS];
};

struct idu_arg {
	struct idu_shared *sh;
	pthread_t thread;
	unsigned long *mm_table;
	unsigned long *ns_table;	/* NULL: count by mm_cid only */
	unsigned long rounds;		/* end after that many loops; 0: at sh->stop */
	int slot;			/* -1: not watched */
};

/*
 * Count once by the ID @kind of the calling thread, in the table of @a.
 * Return 0 when the entry was incremented. Otherwise the ID was not below
 * sh->limit_ids: record the first such read in @sh, count it, and return
 * 1. @loops is the number of loops the thread has done.
 */
static int idu_count(struct idu_arg *a, int kind, unsigned long loops)
{
	unsigned long *table = kind == IDU_NS ? a->ns_table : a->mm_table;
	struct idu_shared *sh = a->sh;
	unsigned int id;
	int none = 0;

	if (!lib_rseq_count_by_id(kind, table, sh->limit_ids, &id))
		return 0;
	if (__atomic_compare_exchange_n(&sh->bad, &none, 1 + kind, false,
					__ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
		sh->bad_id = id;
		sh->bad_loops = loops;
		sh->bad_slot = a->slot;
	}
	__atomic_add_fetch(&sh->bad_reads, 1, __ATOMIC_RELAXED);
	return 1;
}

/* About one millisecond of loops in idu_thread() on the test machines. */
#define IDU_NAP_EVERY	20000

/*
 * Count by mm_cid in a->mm_table, and by ns_cid in a->ns_table if it is
 * set, until sh->stop or for a->rounds loops. A read of an ID which is not
 * below sh->limit_ids is reported in sh->bad and counts nothing. Set
 * sh->noext and count nothing if the rseq area of the thread lacks one of
 * these IDs.
 *
 * A watched thread (a->slot >= 0) also does this for the harness:
 *  - It records each ID it reads in its slot: ns_cid if it counts by it,
 *    else mm_cid.
 *  - When sh->epoch changes, it sleeps 100 us and then stores the epoch in
 *    its slot. The sleep makes it schedule in again, so the IDs it reads
 *    from then on were given after the change.
 *  - While sh->nap_us is set, it sleeps that long every IDU_NAP_EVERY
 *    loops.
 */
static void *idu_thread(void *arg)
{
	struct idu_arg *a = arg;
	struct idu_shared *sh = a->sh;
	struct idu_slot *slot = a->slot >= 0 ? &sh->slot[a->slot] : NULL;
	int seen_kind = a->ns_table ? IDU_NS : IDU_MM;
	unsigned long ok[2] = { }, loops = 0;
	int epoch = sh->epoch, nap, ready;

	ready = !lib_rseq_register() && lib_rseq_has_id(IDU_MM) &&
		(!a->ns_table || lib_rseq_has_id(IDU_NS));
	if (!ready)
		sh->noext = 1;
	if (slot)
		__atomic_add_fetch(&sh->up, 1, __ATOMIC_RELEASE);
	if (!ready)
		goto out;
	while (!sh->stop && (!a->rounds || loops < a->rounds)) {
		if (idu_count(a, IDU_MM, loops))
			continue;
		ok[IDU_MM]++;
		if (a->ns_table) {
			if (idu_count(a, IDU_NS, loops))
				continue;
			ok[IDU_NS]++;
		}
		loops++;
		if (!slot)
			continue;
		__atomic_fetch_or(&slot->ids_seen, 1UL << (lib_rseq_id(seen_kind) & 63),
				  __ATOMIC_RELAXED);
		slot->loops = loops;
		if (sh->epoch != epoch) {
			epoch = sh->epoch;
			usleep(100);
			slot->ack = epoch;
		}
		nap = sh->nap_us;
		if (nap && loops % IDU_NAP_EVERY == 0)
			usleep(nap);
	}
	__atomic_add_fetch(&sh->counted[IDU_MM], ok[IDU_MM], __ATOMIC_RELAXED);
	__atomic_add_fetch(&sh->counted[IDU_NS], ok[IDU_NS], __ATOMIC_RELAXED);
out:
	lib_rseq_unregister();
	return NULL;
}

/*
 * Start @n watched threads which count until sh->stop. Their slots start
 * at @slot. Return 0, or -1.
 */
static int idu_start(struct idu_shared *sh, struct idu_arg *a, int n, int slot,
		     unsigned long *mm_table, unsigned long *ns_table)
{
	int i;

	for (i = 0; i < n; i++) {
		a[i] = (struct idu_arg){ .sh = sh, .mm_table = mm_table, .ns_table = ns_table,
					 .slot = slot + i };
		if (pthread_create(&a[i].thread, NULL, idu_thread, &a[i]))
			return -1;
	}
	return 0;
}

/*
 * Until sh->stop: start @n threads which each count for 20000 loops, and
 * join them. They are not watched.
 */
static void idu_churn(struct idu_shared *sh, int n, unsigned long *mm_table,
		      unsigned long *ns_table)
{
	struct idu_arg a[IDU_SLOTS];
	int i, started;

	while (!sh->stop) {
		for (started = 0; started < n; started++) {
			a[started] = (struct idu_arg){ .sh = sh, .mm_table = mm_table,
						       .ns_table = ns_table, .rounds = 20000,
						       .slot = -1 };
			if (pthread_create(&a[started].thread, NULL, idu_thread, &a[started]))
				break;
		}
		for (i = 0; i < started; i++)
			pthread_join(a[i].thread, NULL);
		__atomic_add_fetch(&sh->churned, started, __ATOMIC_RELAXED);
		if (!started)
			usleep(1000);
	}
}

/*
 * Wait until @pid ends. Return its exit status, or -1 if it was killed by
 * a signal. Kill it and return -1 if it has not ended after @seconds.
 */
static int idu_reap(pid_t pid, int seconds)
{
	int i, status;

	for (i = 0; i < seconds * 1000; i++) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		usleep(1000);
	}
	kill(pid, SIGKILL);
	/* Do not wait for ever: a task stuck in the kernel does not die. */
	for (i = 0; i < 1000; i++) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			break;
		usleep(1000);
	}
	return -1;
}

/*
 * Write cpu.concurrency.max of @g every 10 ms until sh->stop. The values
 * are those of limits[], in a loop. Count the writes in sh->writes. Run in
 * a process of its own, so that the test can time out when a write does
 * not return.
 */
static int idu_writer(const char *g, struct idu_shared *sh)
{
	static char * const limits[] = { "1", "3", "max", "2" };
	int i = 0;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	while (!sh->stop) {
		if (cg_write(g, "cpu.concurrency.max", limits[i++ % ARRAY_SIZE(limits)]))
			return 1;
		sh->writes++;
		usleep(10000);
	}
	return 0;
}

/* Wait up to 10 s until @n watched threads run. Return 0, or -1. */
static int idu_wait_up(struct idu_shared *sh, int n)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (sh->noext)
			return -1;
		if (__atomic_load_n(&sh->up, __ATOMIC_ACQUIRE) >= n)
			return 0;
		usleep(1000);
	}
	return -1;
}

/*
 * Compare the tables with the counts, once every thread has ended. Print
 * what is wrong. Return 0 if no ID was out of range and no increment was
 * lost.
 */
static int idu_check(struct idu_shared *sh)
{
	unsigned long sum[2] = { };
	static const char * const name[] = { "mm_cid", "ns_cid" };
	int i, j, ret = 0;

	for (i = 0; i < IDU_PROCS; i++)
		for (j = 0; j < IDU_IDS; j++)
			sum[IDU_MM] += sh->mm_table[i][j];
	for (j = 0; j < IDU_IDS; j++)
		sum[IDU_NS] += sh->ns_table[j];
	if (sh->bad) {
		fprintf(stderr, "%lu reads of an ID which is not below %u; the first: %s %#x, by a thread which had counted %lu times\n",
			sh->bad_reads, sh->limit_ids, name[sh->bad - 1], sh->bad_id, sh->bad_loops);
		ret = 1;
	}
	for (i = 0; i < 2; i++) {
		if (sum[i] == sh->counted[i])
			continue;
		fprintf(stderr, "%lu of %lu increments by %s lost: two threads ran at once with one ID\n",
			sh->counted[i] - sum[i], sh->counted[i], name[i]);
		ret = 1;
	}
	return ret;
}

/*
 * Wait until cpu.concurrency.effective of @cg shows the limit @want. Return
 * 0, or -1 after 2 s. The file follows a write once the tasks are within
 * the limit and the ID bounds are applied. Both take a few scheduler
 * ticks.
 */
static int idu_wait_effective(const char *cg, int want)
{
	char buf[64];
	int i;

	for (i = 0; i < 2000; i++) {
		if (!cg_read(cg, "cpu.concurrency.effective", buf, sizeof(buf)) &&
		    buf[0] != 'm' && atoi(buf) == want)
			return 0;
		usleep(1000);
	}
	return -1;
}

/*
 * Run the case @name in a new execution of this program, and return its
 * result. A case calls this when lib_rseq_glibc_hides_ns_cid(): glibc
 * registered an rseq area without ns_cid for the threads of this program.
 *
 * A child of the calling process sets GLIBC_TUNABLES=glibc.pthread.rseq=0
 * and executes this program with the arguments OWN_RSEQ_ARG and @name.
 * glibc then registers no rseq area there, and the rseq selftest library
 * registers one with ns_cid for each thread of the case. main() of the new
 * execution runs the case @name alone, see own_rseq_main(). The exit
 * status is the result of the case. This is how the tests of
 * tools/testing/selftests/rseq get such an area too.
 *
 * The new execution runs this function if glibc registered an area
 * without ns_cid again. The case is then skipped.
 */
#define OWN_RSEQ_ARG	"--own-rseq"
static int own_rseq_child;	/* this is the new execution */

static int own_rseq_run(const char *name)
{
	const char *old = getenv("GLIBC_TUNABLES");
	char tunables[1024];
	int status;
	pid_t pid;

	if (own_rseq_child)
		return skip("glibc.pthread.rseq=0 does not stop the rseq area of glibc, without ns_cid");
	ksft_print_msg("%s runs in a new execution, without the rseq area of glibc\n", name);
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	if (pid < 0)
		return KSFT_FAIL;
	if (!pid) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		snprintf(tunables, sizeof(tunables), "%s:glibc.pthread.rseq=0", old ? old : "");
		if (!setenv("GLIBC_TUNABLES", tunables, 1))
			execl("/proc/self/exe", "test_cpu_concurrency", OWN_RSEQ_ARG, name, NULL);
		fprintf(stderr, "cannot execute this program again: %s\n", strerror(errno));
		_exit(KSFT_FAIL);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return KSFT_FAIL;
	return WEXITSTATUS(status);
}

/*
 * Say whether the calling case can count: by mm_cid, and with @ns by
 * ns_cid too. Return -1 if it can. Otherwise return the result of the
 * case @name: KSFT_SKIP, or what own_rseq_run() returns.
 */
static int idu_setup(const char *name, int ns)
{
	if (ns && lib_rseq_glibc_hides_ns_cid())
		return own_rseq_run(name);
	if (!lib_rseq_has_id(IDU_MM))
		return skip("no rseq area with mm_cid, or no rseq library for this architecture");
	return -1;
}

static struct idu_shared *idu_map(void)
{
	struct idu_shared *sh;

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		return NULL;
	memset(sh, 0, sizeof(*sh));
	return sh;
}

/* The process of the first part of ids_unique. */
static int idu_mm_proc(const char *cgroup, void *arg)
{
	struct idu_shared *sh = arg;
	struct idu_arg a[4];
	int i;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (idu_start(sh, a, 4, 0, sh->mm_table[0], NULL))
		return 1;
	idu_churn(sh, 4, sh->mm_table[0], NULL);
	for (i = 0; i < 4; i++)
		pthread_join(a[i].thread, NULL);
	return 0;
}

/* Member @idx of the second part of ids_unique. */
static int idu_ns_member(struct idu_shared *sh, int idx)
{
	struct idu_arg a[3];
	int i;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (idu_start(sh, a, 3, 3 * idx, sh->mm_table[idx], sh->ns_table))
		return 1;
	idu_churn(sh, 2, sh->mm_table[idx], sh->ns_table);
	for (i = 0; i < 3; i++)
		pthread_join(a[i].thread, NULL);
	return 0;
}

/* The creator of the second part of ids_unique. */
static int idu_ns_proc(const char *cgroup, void *arg)
{
	struct idu_shared *sh = arg;
	pid_t member[3], pid;
	int i, n, ret = 0, status;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (unshare(CLONE_NEWCID)) {
		if (errno == EINVAL || errno == EPERM) {
			ksft_print_msg("no concurrency ID namespace (CLONE_NEWCID)\n");
			return NS_CID_SKIP;
		}
		fprintf(stderr, "unshare(CLONE_NEWCID): %s\n", strerror(errno));
		return 2;
	}
	for (n = 0; n < 3; n++) {
		member[n] = fork();
		if (member[n] == 0)
			exit(idu_ns_member(sh, n));
		if (member[n] < 0) {
			fprintf(stderr, "fork of member %d: %s\n", n, strerror(errno));
			ret = 3;
			sh->stop = 1;
			break;
		}
	}
	/* Short members: each counts in one thread for 20000 loops. */
	while (!sh->stop) {
		pid = fork();
		if (pid == 0) {
			struct idu_arg a = { .sh = sh, .mm_table = sh->mm_table[3],
					     .ns_table = sh->ns_table, .rounds = 20000,
					     .slot = -1 };

			prctl(PR_SET_PDEATHSIG, SIGKILL);
			idu_thread(&a);
			exit(0);
		}
		if (pid < 0) {
			fprintf(stderr, "fork of a short member: %s\n", strerror(errno));
			ret = 3;
			break;
		}
		waitpid(pid, &status, 0);
		__atomic_add_fetch(&sh->churned, 1, __ATOMIC_RELAXED);
	}
	for (i = 0; i < n; i++) {
		waitpid(member[i], &status, 0);
		if (!ret && (!WIFEXITED(status) || WEXITSTATUS(status)))
			ret = 4;
	}
	return ret;
}

/*
 * Run one part of ids_unique in @g: idu_mm_proc() or idu_ns_proc() with
 * @threads watched threads. Return 0, NS_CID_SKIP, or another value on
 * failure.
 */
static int idu_part(const char *g, struct idu_shared *sh,
		    int (*proc)(const char *cgroup, void *arg), int threads, const char *what)
{
	pid_t pid, writer;
	int ret;

	memset(sh, 0, sizeof(*sh));
	sh->limit_ids = MIN(get_nprocs_conf(), IDU_IDS);
	/* The first member of the namespace must join under a limit. */
	if (cg_write(g, "cpu.concurrency.max", "2"))
		return 1;
	pid = cg_run_nowait(g, proc, sh);
	if (pid < 0)
		return 1;
	if (idu_wait_up(sh, threads)) {
		sh->stop = 1;
		ret = idu_reap(pid, 10);
		if (ret == NS_CID_SKIP)
			return ret;
		if (sh->noext)
			return skip("no ns_cid in the rseq area of a thread") ? NS_CID_SKIP : 1;
		fprintf(stderr, "%s: %d of %d threads started in 10 s\n", what, sh->up, threads);
		return 1;
	}
	writer = fork();
	if (writer == 0)
		exit(idu_writer(g, sh));
	if (writer < 0) {
		sh->stop = 1;
		idu_reap(pid, 10);
		return 1;
	}
	sleep(3);
	sh->stop = 1;
	/*
	 * A write walks the tasks of the cgroup, a few hundred at most. The
	 * threads only have to see sh->stop and return. 30 s is far more
	 * than either takes.
	 */
	ret = idu_reap(writer, 30);
	if (ret) {
		fprintf(stderr, "%s: a write of cpu.concurrency.max failed or has not returned after 30 s (%lu writes done)\n",
			what, sh->writes);
		idu_reap(pid, 1);
		return 1;
	}
	ret = idu_reap(pid, 30);
	if (ret) {
		fprintf(stderr, "%s: the process did not end normally within 30 s (%d)\n", what, ret);
		return 1;
	}
	if (idu_check(sh))
		return 1;
	if (!sh->counted[IDU_MM] || !sh->churned) {
		fprintf(stderr, "%s: %lu increments, %lu threads came and went\n", what,
			sh->counted[IDU_MM], sh->churned);
		return 1;
	}
	ksft_print_msg("%s: %lu increments by mm_cid, %lu by ns_cid, none lost; %lu threads or processes came and went, %lu limit writes\n",
		       what, sh->counted[IDU_MM], sh->counted[IDU_NS], sh->churned, sh->writes);
	return 0;
}

/*
 * Two tasks which run at the same time never read the same mm_cid, and
 * never the same ns_cid within one namespace.
 *
 * Cgroup: g. The test process stays outside. Each part lasts 3 s. During
 * a part, a child of the test process writes cpu.concurrency.max of g
 * every 10 ms, see idu_writer(). The child is outside g too.
 *
 * Part 1, mm_cid. One process in g. Four of its threads count for the
 * whole part. Its first thread starts four more threads. Each counts
 * 20000 times and ends. The first thread then starts four again, until
 * the end of the part.
 *
 * Part 2, ns_cid. A creator in g calls unshare(CLONE_NEWCID) while the
 * limit is 2, so the namespace gets the home g. It forks three members.
 * In each member, three threads count for the whole part, and two more at
 * a time count 20000 times and end. The creator also forks a fourth
 * member again and again: it counts 20000 times in its only thread and
 * exits.
 *
 * "Count" is one call of lib_rseq_count_by_id() by mm_cid, in a table of the
 * process. In part 2 it is also one call by ns_cid, in a table which all
 * members share. See struct idu_shared for what a lost increment proves.
 *
 * The test fails if an increment was lost, or if a thread read an ID
 * which is not below the number of possible CPUs. It also fails if the
 * writer or the processes of g have not ended 30 s after the end of a
 * part. It is skipped without part 2 if the kernel has no concurrency ID
 * namespace.
 */
static int test_concurrency_ids_unique(const char *root)
{
	struct idu_shared *sh;
	int ret = KSFT_FAIL, r;
	char *g = NULL;

	r = idu_setup("test_concurrency_ids_unique", 1);
	if (r >= 0)
		return r;
	if (need_cpus(4))
		return KSFT_SKIP;
	sh = idu_map();
	if (!sh)
		return KSFT_FAIL;
	g = cg_name(root, "cc_test_ids_unique");
	if (!g || cg_create(g))
		goto cleanup;
	if (idu_part(g, sh, idu_mm_proc, 4, "one process"))
		goto cleanup;
	r = idu_part(g, sh, idu_ns_proc, 9, "one namespace");
	if (r == NS_CID_SKIP)
		ret = KSFT_SKIP;
	else if (!r)
		ret = KSFT_PASS;
cleanup:
	if (g) {
		cg_write(g, "cpu.concurrency.max", "max");
		cg_destroy(g);
	}
	free(g);
	munmap(sh, sizeof(*sh));
	return ret;
}

/*
 * The process of limit_raise_ids and ids_complete_after_rounds. Its
 * sh->nthreads threads, the first one included, are watched and count by
 * mm_cid until sh->stop.
 */
static int idu_spin_proc(const char *cgroup, void *arg)
{
	struct idu_shared *sh = arg;
	struct idu_arg a[IDU_SLOTS];
	int i, n = sh->nthreads - 1;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (idu_start(sh, a, n, 0, sh->mm_table[0], NULL))
		return 1;
	a[n] = (struct idu_arg){ .sh = sh, .mm_table = sh->mm_table[0], .slot = n };
	idu_thread(&a[n]);
	for (i = 0; i < n; i++)
		pthread_join(a[i].thread, NULL);
	return 0;
}

/*
 * Start idu_spin_proc() in @g with @threads threads. Every ID they read
 * must be below @limit_ids. Return the pid once all threads run, or -1.
 */
static pid_t idu_spin_start(const char *g, struct idu_shared *sh, int threads,
			    unsigned int limit_ids)
{
	pid_t pid;

	memset(sh, 0, sizeof(*sh));
	sh->limit_ids = limit_ids;
	sh->nthreads = threads;
	pid = cg_run_nowait(g, idu_spin_proc, sh);
	if (pid < 0)
		return -1;
	if (idu_wait_up(sh, threads)) {
		fprintf(stderr, "%d of %d threads started in 10 s\n", sh->up, threads);
		sh->stop = 1;
		idu_reap(pid, 10);
		return -1;
	}
	return pid;
}

/*
 * Stop the process of idu_spin_start() and check its counts. Return 0 if
 * it ended within 30 s, read no ID out of range and lost no increment.
 */
static int idu_spin_stop(struct idu_shared *sh, pid_t pid)
{
	int ret;

	sh->stop = 1;
	ret = idu_reap(pid, 30);
	if (ret) {
		fprintf(stderr, "the process did not end normally within 30 s (%d)\n", ret);
		return 1;
	}
	return idu_check(sh);
}

/*
 * Forget the IDs which the first @n watched threads have read. Wait @ms.
 * Return the IDs they read meanwhile, one bit per ID.
 */
static unsigned long idu_ids_seen(struct idu_shared *sh, int n, int ms)
{
	unsigned long ids = 0;
	int i;

	for (i = 0; i < n; i++)
		__atomic_exchange_n(&sh->slot[i].ids_seen, 0, __ATOMIC_RELAXED);
	usleep(ms * 1000);
	for (i = 0; i < n; i++)
		ids |= __atomic_load_n(&sh->slot[i].ids_seen, __ATOMIC_RELAXED);
	return ids;
}

/*
 * Wait until one window of 20 ms shows the IDs @want, and no other, for
 * the first @n watched threads. Return 0. Return -1 after 2 s, with the
 * IDs of the last window in @got. The threads run all the time, so each
 * one reads its ID in every window in which it gets the CPU. 2 s is 100
 * windows.
 */
static int idu_wait_ids(struct idu_shared *sh, int n, unsigned long want, unsigned long *got)
{
	int i;

	for (i = 0; i < 100; i++) {
		*got = idu_ids_seen(sh, n, 20);
		if (*got == want)
			return 0;
	}
	return -1;
}

/*
 * Make the first @n watched threads sleep once, see idu_thread(). Return
 * 0 when all have, -1 after 2 s. A thread sees the request within one
 * loop and sleeps 100 us. With more threads than CPUs it must first get
 * the CPU, which takes a few scheduler periods.
 */
static int idu_new_epoch(struct idu_shared *sh, int n)
{
	int e = sh->epoch + 1, i, j;

	sh->epoch = e;
	for (i = 0; i < 2000; i++) {
		for (j = 0; j < n && sh->slot[j].ack == e; j++)
			;
		if (j == n)
			return 0;
		usleep(1000);
	}
	return -1;
}

/*
 * Raise a limit over a process whose IDs are owned by the CPUs.
 *
 * Cgroup: g, with cpu.concurrency.max 2. One process in g has three
 * threads. Each counts by mm_cid without a pause, see idu_thread(). The
 * process has more threads than IDs below 2, so the CPUs own the IDs.
 *
 * 50 rounds, all steps done by the test process from outside g:
 *  1. Write 3 to cpu.concurrency.max. Wait until cpu.concurrency.effective
 *     shows 3. The kernel then gives each thread an ID of its own.
 *  2. Wait for a window of 20 ms in which the three threads read the IDs
 *     0, 1 and 2, see idu_wait_ids(). Three IDs below 3 exist only if no
 *     earlier round lost one.
 *  3. Write 2. Wait until the file shows 2. Then make each thread sleep
 *     once, so that it is scheduled in under the new limit.
 *  4. Read the IDs of the three threads for 20 ms. Each must be 0 or 1.
 *
 * The test also fails if a thread read an ID at or above 3 at any time.
 * It fails if an increment was lost: two threads ran at once with one ID.
 *
 * The kernel gives each thread its own ID in step 1 only if the three
 * threads are fewer than half the possible CPUs. So the test needs 8.
 */
static int test_concurrency_limit_raise_ids(const char *root)
{
	struct idu_shared *sh;
	int ret = KSFT_FAIL, r;
	unsigned long ids;
	pid_t pid = -1;
	char *g = NULL;

	r = idu_setup("test_concurrency_limit_raise_ids", 0);
	if (r >= 0)
		return r;
	if (get_nprocs_conf() < 8 || need_cpus(8))
		return skip("8 CPUs needed");
	sh = idu_map();
	if (!sh)
		return KSFT_FAIL;
	g = cg_name(root, "cc_test_raise_ids");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "2"))
		goto cleanup;
	pid = idu_spin_start(g, sh, 3, 3);
	if (pid < 0)
		goto cleanup;
	for (r = 0; r < 50; r++) {
		if (cg_write(g, "cpu.concurrency.max", "3") || idu_wait_effective(g, 3)) {
			fprintf(stderr, "round %d: the effective value did not reach 3\n", r);
			goto stop;
		}
		if (idu_wait_ids(sh, 3, 0x7, &ids)) {
			fprintf(stderr, "round %d: three threads under the limit 3 read the IDs %#lx (one bit per ID) for 2 s, expected 0x7\n",
				r, ids);
			goto stop;
		}
		if (cg_write(g, "cpu.concurrency.max", "2") || idu_wait_effective(g, 2)) {
			fprintf(stderr, "round %d: the effective value did not reach 2\n", r);
			goto stop;
		}
		if (idu_new_epoch(sh, 3)) {
			fprintf(stderr, "round %d: a thread did not run for 2 s under the limit 2\n", r);
			goto stop;
		}
		ids = idu_ids_seen(sh, 3, 20);
		if (!ids || (ids & ~0x3UL)) {
			fprintf(stderr, "round %d: the IDs %#lx (one bit per ID) read after the effective value showed 2\n",
				r, ids);
			goto stop;
		}
	}
	if (!idu_spin_stop(sh, pid))
		ret = KSFT_PASS;
	pid = -1;
stop:
	if (pid > 0) {
		/* Report a wrong ID or a lost increment as well. */
		idu_spin_stop(sh, pid);
	}
cleanup:
	if (g) {
		cg_write(g, "cpu.concurrency.max", "max");
		cg_destroy(g);
	}
	free(g);
	munmap(sh, sizeof(*sh));
	return ret;
}

/*
 * No ID is lost when a limit goes down and up many times.
 *
 * Cgroup: g, with cpu.concurrency.max 3. One process in g has six
 * threads. Each counts by mm_cid, see idu_thread(). During step 1 each
 * thread also sleeps 1 ms after about every millisecond of counting.
 *
 * Steps, all done by the test process from outside g:
 *  1. 500 times: write 1, then 3, to cpu.concurrency.max. After each
 *     write, wait 0 to 2 ms. The waits come from rand_r() with a fixed
 *     seed.
 *  2. Stop the sleeps of the threads. Wait until
 *     cpu.concurrency.effective shows 3.
 *  3. Wait for a window of 20 ms in which the threads read the IDs 0, 1
 *     and 2, see idu_wait_ids(). An ID lost in step 1 leaves two IDs
 *     below 3, and such a window never comes.
 *  4. Twice: wait 250 ms and check that each thread has counted
 *     meanwhile. With six threads on three CPUs, a thread which does not
 *     run for 250 ms is waiting for an ID.
 *
 * The test also fails if a thread read an ID at or above 3 at any time.
 * It fails if an increment was lost: two threads ran at once with one ID.
 */
static int test_concurrency_ids_complete_after_rounds(const char *root)
{
	unsigned long ids, loops[6];
	struct idu_shared *sh;
	int ret = KSFT_FAIL, r, i;
	unsigned int seed = 1;
	pid_t pid = -1;
	char *g = NULL;

	r = idu_setup("test_concurrency_ids_complete_after_rounds", 0);
	if (r >= 0)
		return r;
	if (need_cpus(4))
		return KSFT_SKIP;
	sh = idu_map();
	if (!sh)
		return KSFT_FAIL;
	g = cg_name(root, "cc_test_ids_complete");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "3"))
		goto cleanup;
	pid = idu_spin_start(g, sh, 6, 3);
	if (pid < 0)
		goto cleanup;
	sh->nap_us = 1000;
	for (r = 0; r < 1000; r++) {
		if (cg_write(g, "cpu.concurrency.max", r & 1 ? "3" : "1")) {
			fprintf(stderr, "round %d: the limit cannot be written\n", r / 2);
			goto stop;
		}
		usleep(rand_r(&seed) % 2000);
	}
	sh->nap_us = 0;
	if (idu_wait_effective(g, 3)) {
		fprintf(stderr, "the effective value did not reach 3\n");
		goto stop;
	}
	if (idu_wait_ids(sh, 6, 0x7, &ids)) {
		fprintf(stderr, "six threads under the limit 3 read the IDs %#lx (one bit per ID) for 2 s, expected 0x7\n",
			ids);
		goto stop;
	}
	for (r = 0; r < 2; r++) {
		for (i = 0; i < 6; i++)
			loops[i] = sh->slot[i].loops;
		usleep(250000);
		for (i = 0; i < 6; i++) {
			if (sh->slot[i].loops == loops[i]) {
				fprintf(stderr, "thread %d did not count for 250 ms\n", i);
				goto stop;
			}
		}
	}
	if (!idu_spin_stop(sh, pid))
		ret = KSFT_PASS;
	pid = -1;
stop:
	if (pid > 0) {
		/* Report a wrong ID or a lost increment as well. */
		idu_spin_stop(sh, pid);
	}
cleanup:
	if (g) {
		cg_write(g, "cpu.concurrency.max", "max");
		cg_destroy(g);
	}
	free(g);
	munmap(sh, sizeof(*sh));
	return ret;
}

/*
 * A join widens the CPUs of a namespace while the parent of the new member
 * is a member of another namespace.
 *
 * Cgroups, by their name in struct ns_widen_param:
 *
 *   base
 *   |- x       cpuset.cpus 0, no limit
 *   `- l       cpu.concurrency.max 4, all CPUs
 *      |- l1   cpuset.cpus 0
 *      `- l2   all CPUs
 *
 * Processes. Each is single-threaded. "Counts" means that it loops in
 * idu_thread(): it counts by mm_cid and by ns_cid.
 *  - K runs ns_widen_run() in "x". It calls unshare(CLONE_NEWCID): its
 *    children are in a new namespace A.
 *  - M2 and M are forked by K, in "x". They are the members of A, and
 *    both run on CPU 0 only. M2 counts and sleeps 1 ms about every
 *    millisecond. M runs the steps below. A has two members and one CPU,
 *    so CPU 0 owns the one ID of A.
 *  - The members of a namespace B are C1, C2, C4 and C5. The steps create
 *    them. Each counts without a pause.
 *
 * Steps, all done by M:
 *  1. Move to "l1", call unshare(CLONE_NEWCID) and move back to "x". The
 *     children of M are in a new namespace B. B is created under "l", so
 *     it has the home "l" and the soft limit 4. M stays a member of A,
 *     which has no home, so M may be in any cgroup.
 *  2. clone3(CLONE_INTO_CGROUP) C1 into "l1". C1 forks C2. B has two
 *     members and one CPU. So CPU 0 owns the one ID of B. Wait until C1
 *     and C2 count, then 20 ms more.
 *  3. clone3(CLONE_INTO_CGROUP) C4 into "l2". C4 may run on all CPUs.
 *     This join widens B to all CPUs, and B gives each member an ID of its
 *     own from now on. M runs this join on CPU 0, as a member of A.
 *  4. clone3(CLONE_INTO_CGROUP) C5 into "l2". Move C1 and C2 to "l2".
 *     The four members of B may now run on four CPUs at once.
 *  5. Wait until the four members of B count. Then wait for a window of
 *     20 ms in which they read the ns_cid values 0 to 3, see
 *     idu_wait_ids(). Then check twice that each has counted within
 *     250 ms.
 *  6. Count 200000 times as a member of A, next to M2.
 *
 * The test fails in these cases:
 *  - NS_GET_CID_CGROUP does not return the ID of "l" for B after step 1;
 *  - a member of B does not start to count within 10 s;
 *  - the window of step 5 does not come within 2 s;
 *  - a member of B stops counting;
 *  - a member of B read an ns_cid at or above 4;
 *  - an increment by ns_cid was lost in A or in B.
 * Each of the first three means that B has fewer than four IDs below its
 * soft limit.
 *
 * The join of step 3 gives each member its own ID only if three members
 * are fewer than half the possible CPUs. So the test needs 8 CPUs.
 */
struct ns_widen_param {
	char *x, *l, *l1, *l2;
	struct idu_shared *a, *b;	/* the state of namespace A, and of B */
};

/* Count as the member of B in slot @slot. With @fork_one, fork the next one first. */
static int ns_widen_member(struct idu_shared *sh, int slot, int fork_one)
{
	struct idu_arg a = { .sh = sh, .mm_table = sh->mm_table[slot],
			     .ns_table = sh->ns_table, .slot = slot };
	pid_t c = -1;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (fork_one) {
		c = fork();
		if (c == 0)
			exit(ns_widen_member(sh, slot + 1, 0));
		if (c < 0)
			return 1;
	}
	idu_thread(&a);
	if (c > 0)
		waitpid(c, NULL, 0);
	return 0;
}

/*
 * Create a member of B in @cg with clone3(CLONE_INTO_CGROUP). Return its
 * pid, or -1.
 */
static pid_t ns_widen_clone(const char *cg, struct idu_shared *sh, int slot, int fork_one)
{
	int fd = open(cg, O_RDONLY | O_DIRECTORY);
	pid_t pid;

	if (fd < 0)
		return -1;
	pid = clone_into_cgroup(fd);
	if (pid == 0)
		exit(ns_widen_member(sh, slot, fork_one));
	if (pid < 0)
		fprintf(stderr, "clone3(CLONE_INTO_CGROUP) into %s: %s\n", cg, strerror(errno));
	close(fd);
	return pid;
}

/* Run M. Return 0, NS_CID_SKIP, or another value on failure. */
static int ns_widen_m(struct ns_widen_param *p)
{
	struct idu_arg self = { .sh = p->a, .mm_table = p->a->mm_table[1],
				.ns_table = p->a->ns_table, .rounds = 200000, .slot = 1 };
	struct idu_shared *b = p->b;
	pid_t c1 = -1, c4 = -1, c5 = -1;
	unsigned long ids, loops[4];
	unsigned long long home = 0;
	int ret = 1, i, r;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	/* The cgroup of the caller of unshare(2) decides the home of B. */
	if (cg_enter_current(p->l1)) {
		fprintf(stderr, "M cannot move under the limited cgroup\n");
		return 1;
	}
	if (unshare(CLONE_NEWCID)) {
		fprintf(stderr, "the second unshare(CLONE_NEWCID): %s\n", strerror(errno));
		return 1;
	}
	if (cg_enter_current(p->x)) {
		fprintf(stderr, "M cannot move back from the limited cgroup\n");
		return 1;
	}
	if (ns_cid_home_of(getpid(), "cid_for_children", &home) ||
	    home != ns_cid_cgroup_id(p->l)) {
		fprintf(stderr, "NS_GET_CID_CGROUP of the second namespace: %llu, expected %llu\n",
			home, ns_cid_cgroup_id(p->l));
		return 1;
	}
	c1 = ns_widen_clone(p->l1, b, 0, 1);
	if (c1 < 0)
		return errno == ENOSYS ? NS_CID_SKIP : 1;
	if (idu_wait_up(b, 2)) {
		fprintf(stderr, "%d of the first 2 members of the new namespace count after 10 s\n", b->up);
		goto out;
	}
	usleep(20000);
	c4 = ns_widen_clone(p->l2, b, 2, 0);
	if (c4 < 0)
		goto out;
	c5 = ns_widen_clone(p->l2, b, 3, 0);
	if (c5 < 0)
		goto out;
	/* C2 is the child of C1: its pid is the next one in "l1". */
	for (i = 0; i < 2; i++) {
		char buf[64] = "";

		if (cg_read(p->l1, "cgroup.procs", buf, sizeof(buf)) || !atoi(buf) ||
		    cg_enter(p->l2, atoi(buf))) {
			fprintf(stderr, "member %d cannot move to the cgroup with all CPUs\n", i);
			goto out;
		}
	}
	if (idu_wait_up(b, 4)) {
		fprintf(stderr, "%d of the 4 members of the new namespace count after 10 s\n", b->up);
		goto out;
	}
	if (idu_wait_ids(b, 4, 0xf, &ids)) {
		fprintf(stderr, "4 members under the soft limit 4 read the ns_cid values %#lx (one bit per ID) for 2 s, expected 0xf\n",
			ids);
		goto out;
	}
	for (r = 0; r < 2; r++) {
		for (i = 0; i < 4; i++)
			loops[i] = b->slot[i].loops;
		usleep(250000);
		for (i = 0; i < 4; i++) {
			if (b->slot[i].loops == loops[i]) {
				fprintf(stderr, "member %d of the new namespace did not count for 250 ms\n", i);
				goto out;
			}
		}
	}
	idu_thread(&self);
	ret = 0;
out:
	b->stop = 1;
	/* A member which waits for an ID in the kernel does not see the flag. */
	if (ret) {
		usleep(100000);
		if (c1 > 0)
			kill(c1, SIGKILL);
		if (c4 > 0)
			kill(c4, SIGKILL);
		if (c5 > 0)
			kill(c5, SIGKILL);
	}
	if (c1 > 0)
		waitpid(c1, NULL, 0);
	if (c4 > 0)
		waitpid(c4, NULL, 0);
	if (c5 > 0)
		waitpid(c5, NULL, 0);
	return ret;
}

/* Run K. */
static int ns_widen_run(const char *cgroup, void *arg)
{
	struct idu_arg m2_arg = { .mm_table = NULL };
	struct ns_widen_param *p = arg;
	pid_t m, m2;
	int status, ret;

	if (unshare(CLONE_NEWCID)) {
		if (errno == EINVAL || errno == EPERM) {
			ksft_print_msg("no concurrency ID namespace (CLONE_NEWCID)\n");
			return NS_CID_SKIP;
		}
		fprintf(stderr, "unshare(CLONE_NEWCID): %s\n", strerror(errno));
		return 1;
	}
	p->a->nap_us = 1000;
	m2 = fork();
	if (m2 < 0)
		return 1;
	if (m2 == 0) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		m2_arg = (struct idu_arg){ .sh = p->a, .mm_table = p->a->mm_table[0],
					   .ns_table = p->a->ns_table, .slot = 0 };
		idu_thread(&m2_arg);
		exit(0);
	}
	if (idu_wait_up(p->a, 1)) {
		ret = p->a->noext ? NS_CID_SKIP : 1;
		goto out;
	}
	m = fork();
	if (m < 0) {
		ret = 1;
		goto out;
	}
	if (m == 0)
		exit(ns_widen_m(p));
	waitpid(m, &status, 0);
	ret = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
out:
	p->a->stop = 1;
	waitpid(m2, &status, 0);
	return ret;
}

static int test_concurrency_ns_join_widen(const char *root)
{
	struct ns_widen_param p = { };
	int ret = KSFT_FAIL, r;
	char *base = NULL;

	r = idu_setup("test_concurrency_ns_join_widen", 1);
	if (r >= 0)
		return r;
	if (get_nprocs_conf() < 8 || need_cpus(8))
		return skip("8 CPUs needed");
	r = ns_cid_is_cpu();
	if (r)
		return r == KSFT_SKIP ? KSFT_SKIP : KSFT_FAIL;
	p.a = idu_map();
	p.b = idu_map();
	if (!p.a || !p.b)
		return KSFT_FAIL;
	p.a->limit_ids = MIN(get_nprocs_conf(), IDU_IDS);
	p.b->limit_ids = 4;
	base = cg_name(root, "cc_test_ns_widen");
	if (!base)
		goto cleanup;
	p.x = cg_name(base, "x");
	p.l = cg_name(base, "l");
	if (!p.x || !p.l)
		goto cleanup;
	p.l1 = cg_name(p.l, "l1");
	p.l2 = cg_name(p.l, "l2");
	if (!p.l1 || !p.l2)
		goto cleanup;
	if (cg_create(base) || cg_write(base, "cgroup.subtree_control", "+cpu +cpuset") ||
	    cg_create(p.x) || cg_write(p.x, "cpuset.cpus", "0") ||
	    cg_create(p.l) || cg_write(p.l, "cgroup.subtree_control", "+cpu +cpuset") ||
	    cg_create(p.l1) || cg_write(p.l1, "cpuset.cpus", "0") ||
	    cg_create(p.l2) || cg_write(p.l, "cpu.concurrency.max", "4")) {
		fprintf(stderr, "the cgroups cannot be set up\n");
		goto cleanup;
	}
	r = cg_run(p.x, ns_widen_run, &p);
	if (r == NS_CID_SKIP) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	/* Both namespaces are checked, also after a failure of the steps. */
	if (idu_check(p.a))
		r = 1;
	if (idu_check(p.b))
		r = 1;
	if (!r)
		ret = KSFT_PASS;
cleanup:
	if (p.l)
		cg_write(p.l, "cpu.concurrency.max", "max");
	if (p.l2)
		cg_destroy(p.l2);
	if (p.l1)
		cg_destroy(p.l1);
	if (p.l)
		cg_destroy(p.l);
	if (p.x)
		cg_destroy(p.x);
	if (base)
		cg_destroy(base);
	free(p.l2);
	free(p.l1);
	free(p.l);
	free(p.x);
	free(base);
	munmap(p.a, sizeof(*p.a));
	munmap(p.b, sizeof(*p.b));
	return ret;
}

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

/*
 * Start a sleeping task in @cg and make it SCHED_DEADLINE, 1 ms every
 * 100 ms. Return 0 if it was admitted, or the error of sched_setattr().
 * Return -ECHILD if the task could not be started. The task is gone when
 * this returns.
 *
 * A test of a deadline domain calls this before it writes the limit. An
 * error then tells that the machine does not take a deadline task in this
 * partition root at all. The same error after the limit is written is a
 * failure of the limit.
 */
static int dl_admitted_in(const char *cg)
{
	struct sched_param other = { .sched_priority = 0 };
	pid_t pid;
	int err;

	pid = cg_run_nowait(cg, sleep_forever, NULL);
	if (pid < 0)
		return -ECHILD;
	err = wait_in_cgroup(cg, pid) ? -ECHILD : set_deadline(pid, 1000, 100000);
	/* Give the bandwidth back before the task dies. */
	sched_setscheduler(pid, SCHED_OTHER, &other);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return err;
}

/*
 * Call sched_setaffinity(@pid, @set) from a child process. Return 0, or
 * the errno of the call. Return -ETIMEDOUT if the call has not returned
 * after @ms. The child is then left behind: SIGKILL does not end a task
 * which sleeps on a mutex.
 */
static int setaffinity_within(pid_t pid, const cpu_set_t *set, int ms)
{
	int st, i;
	pid_t c;

	c = fork();
	if (c < 0)
		return -ECHILD;
	if (!c)
		_exit(sched_setaffinity(pid, sizeof(*set), set) ? errno : 0);
	for (i = 0; i < ms; i++) {
		if (waitpid(c, &st, WNOHANG) == c)
			return WIFEXITED(st) ? WEXITSTATUS(st) : -ECHILD;
		usleep(1000);
	}
	kill(c, SIGKILL);
	return -ETIMEDOUT;
}

#define DL_AFF_TIMEOUT_MS	10000

/*
 * Apply @set to @pid with setaffinity_within(). @want is the expected
 * errno, or 0 for a success. Return 0 when the result is @want and
 * Cpus_allowed_list of @pid then reads @list. Else print @what with the
 * result and return -1.
 *
 * A call which does not return leaves cpuset_mutex locked. No cgroup can
 * then be removed and no later case can run. So the whole run ends there.
 */
static int expect_dl_pin(pid_t pid, const cpu_set_t *set, int want, const char *list,
			 const char *what)
{
	int err = setaffinity_within(pid, set, DL_AFF_TIMEOUT_MS);
	char buf[64] = "";

	if (err == -ETIMEDOUT)
		ksft_exit_fail_msg("%s: sched_setaffinity() did not return within %d ms\n",
				   what, DL_AFF_TIMEOUT_MS);
	if (err != want) {
		fprintf(stderr, "%s: %s, not %s\n", what,
			err < 0 ? "failed" : err ? strerror(err) : "accepted",
			want ? strerror(want) : "accepted");
		return -1;
	}
	if (!proc_status(pid, "Cpus_allowed_list", buf, sizeof(buf)) || strcmp(buf, list)) {
		fprintf(stderr, "%s: Cpus_allowed_list is \"%s\" after it, not %s\n", what, buf,
			list);
		return -1;
	}
	return 0;
}

/*
 * sched_setaffinity(2) on a task of a deadline domain returns, with the
 * documented result. The call takes cpuset_mutex for a deadline task. A
 * kernel which takes it a second time on the way never returns.
 *
 * P is a partition root on the last two CPUs, L-1 and L, with the limit 1.
 * d and f sleep in P. d is SCHED_DEADLINE, 10 ms every 100 ms. f is a fair
 * task. A child of the test makes each call, so that the test can see a
 * call which does not return:
 *  - d gets {L-1,L}, the CPUs of P: accepted;
 *  - d gets {L}: EBUSY. A deadline task may not get a mask narrower than
 *    its root domain, with or without a limit;
 *  - f gets {L}: accepted;
 *  - f gets {L-1,L}: accepted;
 *  - d gets every CPU: accepted.
 * After each call Cpus_allowed_list of the task must read "L-1-L", or "L"
 * for f pinned to {L}.
 *
 * Each call must return within 10 s. It waits for cpuset_mutex, for
 * conc_mutex, and for the affinity changes judged before it. Nothing else
 * runs here, so 10 s only pass when the call is stuck.
 *
 * Skipped:
 *  - with fewer than 3 CPUs;
 *  - with deadline admission off;
 *  - without a partition root;
 *  - when P takes no deadline task before it has a limit.
 */
static int test_concurrency_dl_setaffinity_in_domain(const char *root)
{
	struct sched_param other = { .sched_priority = 0 };
	int ret = KSFT_FAIL, n = get_nprocs(), L = n - 1, err;
	cpu_set_t both, last, all;
	char *P = NULL, rt[32], cpus[32], one[32];
	pid_t d = -1, f = -1;

	if (need_cpus(3))
		return KSFT_SKIP;
	if (rt_runtime_read(rt, sizeof(rt)) || atol(rt) < 0)
		return skip("deadline admission control is off");
	P = cg_name(root, "cc_test_dl_aff");
	if (!P || cg_create(P))
		goto cleanup;
	snprintf(one, sizeof(one), "%d", L);
	snprintf(cpus, sizeof(cpus), "%d-%d", L - 1, L);
	if (cg_write(P, "cpuset.cpus", cpus) || cg_write(P, "cpuset.cpus.partition", "root") ||
	    cg_read_strcmp(P, "cpuset.cpus.partition", "root\n")) {
		ret = skip("no partition root on CPUs %s", cpus);
		goto cleanup;
	}
	err = dl_admitted_in(P);
	if (err) {
		ret = skip("no SCHED_DEADLINE task in a partition root without a limit: %s",
			   strerror(-err));
		goto cleanup;
	}
	if (cg_write(P, "cpu.concurrency.max", "1"))
		goto cleanup;
	d = cg_run_nowait(P, sleep_forever, NULL);
	f = cg_run_nowait(P, sleep_forever, NULL);
	if (d < 0 || f < 0 || wait_in_cgroup(P, d) || wait_in_cgroup(P, f))
		goto cleanup;
	err = set_deadline(d, 10000, 100000);
	if (err) {
		fprintf(stderr, "a deadline task of 0.1 CPU refused in a deadline domain: %s\n",
			strerror(-err));
		goto cleanup;
	}
	CPU_ZERO(&both);
	CPU_SET(L - 1, &both);
	CPU_SET(L, &both);
	CPU_ZERO(&last);
	CPU_SET(L, &last);
	stress_all_cpus(&all);
	if (expect_dl_pin(d, &both, 0, cpus, "the deadline task pinned to the CPUs of P") ||
	    expect_dl_pin(d, &last, EBUSY, cpus, "the deadline task pinned to one CPU of P") ||
	    expect_dl_pin(f, &last, 0, one, "a fair task pinned to one CPU of P") ||
	    expect_dl_pin(f, &both, 0, cpus, "a fair task pinned to the CPUs of P") ||
	    expect_dl_pin(d, &all, 0, cpus, "the deadline task pinned to every CPU"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (d > 0) {
		kill(d, SIGKILL);
		sched_setscheduler(d, SCHED_OTHER, &other);
		waitpid(d, NULL, 0);
	}
	if (f > 0) {
		kill(f, SIGKILL);
		waitpid(f, NULL, 0);
	}
	if (P) {
		cg_write(P, "cpu.concurrency.max", "max");
		cg_write(P, "cpuset.cpus.partition", "member");
		cg_destroy(P);
	}
	free(P);
	return ret;
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
 * Sample the CPUs of the @n processes of @pid every 10 ms, @samples times.
 * Return 0 if they were on @limit CPUs at most. Return the number of CPUs
 * found if three samples in a row had more.
 *
 * One sample reads the CPUs one after the other. A set-move between two
 * reads can show a task on its old CPU and another on the new one. Three
 * samples in a row are 20 ms apart, and a set-move does not last that
 * long.
 */
static int procs_cpus_above(const pid_t *pid, int n, int limit, int samples)
{
	int i, k, row = 0;

	for (k = 0; k < samples; k++) {
		cpu_set_t seen;

		CPU_ZERO(&seen);
		for (i = 0; i < n; i++) {
			int cpu = stat_cpu(pid[i]);

			if (cpu >= 0)
				CPU_SET(cpu, &seen);
		}
		row = CPU_COUNT(&seen) > limit ? row + 1 : 0;
		if (row == 3)
			return CPU_COUNT(&seen);
		usleep(10000);
	}
	return 0;
}

/*
 * Check the three spinners of test_concurrency_offline_pinned_running()
 * @when: "after the unplug" or "after the replug". @c is the CPU which s1
 * was pinned to. Return 0 if all the checks listed there hold.
 */
static int offline_pinned_check(const char *g, const pid_t *pid, int c, int c_online,
				const char *when)
{
	cpu_set_t allowed;
	char buf[128] = "";
	int i, cpu;

	i = procs_stuck(pid, 3, 2000);
	if (i >= 0) {
		fprintf(stderr, "%s: s%d got no CPU time in 2 s\n", when, i + 1);
		return -1;
	}
	if (!proc_status(pid[0], "Cpus_allowed_list", buf, sizeof(buf)) ||
	    parse_cpulist(buf, &allowed) || (CPU_COUNT(&allowed) == 1 && CPU_ISSET(c, &allowed))) {
		fprintf(stderr, "%s: Cpus_allowed_list of s1 is \"%s\"\n", when, buf);
		return -1;
	}
	cpu = stat_cpu(pid[0]);
	if (cpu < 0 || !CPU_ISSET(cpu, &allowed) || (!c_online && cpu == c)) {
		fprintf(stderr, "%s: s1 is on CPU %d, its mask is %s\n", when, cpu, buf);
		return -1;
	}
	i = procs_cpus_above(pid, 3, 2, 50);
	if (i) {
		fprintf(stderr, "%s: the spinners are on %d CPUs, limit 2\n", when, i);
		return -1;
	}
	if (cg_read_strcmp(g, "cpu.concurrency.effective", "2 max\n")) {
		fprintf(stderr, "%s: the effective limit is not 2\n", when);
		return -1;
	}
	return 0;
}

/*
 * A running task loses its only CPU to hotplug, under a limit. g has the
 * limit 2. s1, s2 and s3 spin in g. s1 is pinned to {c}, the last CPU.
 * s2 and s3 can run on every CPU. So g uses c and one other CPU.
 *  - The test waits until s1 is on c.
 *  - c goes offline.
 *  - c comes back online.
 * The kernel must give s1 a wider mask when c goes, as it does without a
 * limit. After each of the last two steps:
 *  - each spinner gets CPU time within 2 s. Three spinners share two
 *    CPUs, so each one runs every few milliseconds;
 *  - Cpus_allowed_list of s1 is no longer "c" alone, and s1 is on a CPU
 *    of that list, which is not c while c is offline;
 *  - the three spinners are on two CPUs at most, see procs_cpus_above();
 *  - cpu.concurrency.effective of g reads "2 max".
 * Skipped with fewer than 3 CPUs, or when c cannot go offline.
 */
static int test_concurrency_offline_pinned_running(const char *root)
{
	int ret = KSFT_FAIL, n = get_nprocs(), c = n - 1, offline = 0, i;
	pid_t pid[3] = { -1, -1, -1 };
	char *g = NULL;

	if (need_cpus(3))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_offpin");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "2"))
		goto cleanup;
	for (i = 0; i < 3; i++) {
		pid[i] = cg_run_nowait(g, busy_forever, NULL);
		if (pid[i] < 0 || wait_in_cgroup(g, pid[i]))
			goto cleanup;
	}
	if (expect_pin(pid[0], c, c, 0, "s1 pinned to the last CPU"))
		goto cleanup;
	/* The pin moves a running task at once. Allow 2 s for the stopper. */
	for (i = 0; i < 200 && stat_cpu(pid[0]) != c; i++)
		usleep(10000);
	if (stat_cpu(pid[0]) != c || procs_stuck(pid, 3, 2000) >= 0) {
		fprintf(stderr, "before the unplug: s1 on CPU %d, not %d, or a spinner stuck\n",
			stat_cpu(pid[0]), c);
		goto cleanup;
	}
	if (write_cpu_online(c, 0)) {
		ret = skip("CPU %d cannot go offline", c);
		goto cleanup;
	}
	offline = 1;
	if (offline_pinned_check(g, pid, c, 0, "after the unplug"))
		goto cleanup;
	if (write_cpu_online(c, 1)) {
		fprintf(stderr, "CPU %d does not come back online\n", c);
		goto cleanup;
	}
	offline = 0;
	if (offline_pinned_check(g, pid, c, 1, "after the replug"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (offline)
		write_cpu_online(c, 1);
	for (i = 0; i < 3; i++) {
		if (pid[i] > 0) {
			kill(pid[i], SIGKILL);
			waitpid(pid[i], NULL, 0);
		}
	}
	if (g) {
		cg_write(g, "cpu.concurrency.max", "max");
		cg_destroy(g);
	}
	free(g);
	return ret;
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

#define BW_TASKS	4
#define BW_THROTTLED	3

/*
 * Wait until the first BW_THROTTLED spinners of
 * test_concurrency_bandwidth_under_limit() are throttled: for 30 ms, all
 * are runnable and none uses CPU time. They use the quota of a period in
 * its first 30 ms, and are throttled for the rest of it. Return 0, or -1
 * if this is not seen in 2 s.
 *
 * nr_throttled of cpu.stat does not tell the moment. It grows at the end
 * of a period, when the quota is refilled and the tasks run again.
 */
static int wait_throttled(const pid_t *pid)
{
	long before[BW_THROTTLED];
	int i, t, ran;

	for (t = 0; t < 2000; t += 30) {
		for (i = 0; i < BW_THROTTLED; i++)
			before[i] = proc_cpu_ms(pid[i]);
		usleep(30000);
		ran = 0;
		for (i = 0; i < BW_THROTTLED; i++)
			ran |= proc_cpu_ms(pid[i]) != before[i] || proc_state(pid[i]) != 'R';
		if (!ran)
			return 0;
	}
	fprintf(stderr, "the spinners under the quota were not all throttled at once in 2 s\n");
	return -1;
}

/*
 * Check the spinners of test_concurrency_bandwidth_under_limit() after
 * the step @when. They must all be on one CPU of @allowed within 2 s, and
 * each must get CPU time within 3 s. A spinner under the quota runs about
 * 7 ms in a period of 100 ms, and the CPU time is read in units of 10 ms.
 * So it shows CPU time after two periods. cpu.concurrency.effective of @g
 * must read "1 max". Return the CPU, or -1.
 */
static int bandwidth_check(const char *g, const pid_t *pid, const cpu_set_t *allowed,
			   const char *when)
{
	int cpu, i;

	cpu = procs_on_one_cpu(pid, BW_TASKS, allowed, 2000, when);
	if (cpu < 0)
		return -1;
	i = procs_stuck(pid, BW_TASKS, 3000);
	if (i >= 0) {
		fprintf(stderr, "%s: spinner %d got no CPU time in 3 s\n", when, i);
		return -1;
	}
	if (cg_read_strcmp(g, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "%s: the effective limit is not 1\n", when);
		return -1;
	}
	return cpu;
}

/*
 * cpu.max below a concurrency limit. A task throttled by cpu.max is off
 * the runqueue until the quota is refilled. A set-move must take it along
 * with the tasks which run.
 *
 * g has the cpuset 1-3 and the limit 2. It has two children:
 *  - q has a quota of 20 ms in a period of 100 ms;
 *  - r has no quota.
 * The steps:
 *  - spin_and_check() runs four threads in q for 500 ms. They use two
 *    CPUs at most, and read no mm_cid of 2 or more.
 *  - Three processes then spin in q, and one in r. Those of q use the
 *    quota in 30 ms at most, so they are throttled for most of each
 *    period. The one of r always runs, so g always uses a CPU.
 *  - The limit 1 is written to g. The four must end on one CPU, u.
 *  - g's cpuset.cpus is written without u. The four must end on one CPU
 *    of the new cpuset, v.
 *  - v goes offline. The four must end on the one CPU left to g.
 *  - "max" is written to cpu.max of q. The four must stay on that CPU,
 *    and each must get CPU time within 2 s.
 * The test makes each of the three changes while the spinners of q are
 * throttled, see wait_throttled(). After each one, bandwidth_check() must
 * hold. A throttled task which a set-move left behind shows on another
 * CPU than the others.
 * Skipped with fewer than 4 CPUs, without cpu.max, or when v cannot go
 * offline.
 */
static int test_concurrency_bandwidth_under_limit(const char *root)
{
	struct spin_param sp = { .nthreads = 4, .limit = 2, .duration_ms = 500 };
	int ret = KSFT_FAIL, i, u, v, w, offline = -1;
	char *g = NULL, *q = NULL, *r = NULL, buf[32];
	pid_t pid[BW_TASKS];
	cpu_set_t allowed;

	for (i = 0; i < BW_TASKS; i++)
		pid[i] = -1;
	if (need_cpus(4))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_bw");
	q = g ? cg_name(g, "q") : NULL;
	r = g ? cg_name(g, "r") : NULL;
	if (!g || !q || !r || cg_create(g) || cg_write(g, "cgroup.subtree_control", "+cpu") ||
	    cg_create(q) || cg_create(r))
		goto cleanup;
	if (cg_write(q, "cpu.max", "20000 100000")) {
		ret = skip("no cpu.max");
		goto cleanup;
	}
	if (cg_write(g, "cpuset.cpus", "1-3") || cg_write(g, "cpu.concurrency.max", "2"))
		goto cleanup;
	i = cg_run(q, spin_and_check, &sp);
	if (i) {
		fprintf(stderr, "four threads under the limit and the quota: check %d failed\n", i);
		goto cleanup;
	}
	for (i = 0; i < BW_TASKS; i++) {
		const char *cg = i < BW_THROTTLED ? q : r;

		pid[i] = cg_run_nowait(cg, busy_forever, NULL);
		if (pid[i] < 0 || wait_in_cgroup(cg, pid[i]))
			goto cleanup;
	}

	parse_cpulist("1-3", &allowed);
	if (wait_throttled(pid) ||
	    expect_write(g, "cpu.concurrency.max", "1", 0, "the limit 1 over throttled tasks"))
		goto cleanup;
	u = bandwidth_check(g, pid, &allowed, "after the limit 1");
	if (u < 0)
		goto cleanup;

	CPU_CLR(u, &allowed);
	snprintf(buf, sizeof(buf), "%d,%d", u == 1 ? 2 : 1, u == 3 ? 2 : 3);
	if (wait_throttled(pid) ||
	    expect_write(g, "cpuset.cpus", buf, 0, "g's cpuset without the CPU in use"))
		goto cleanup;
	v = bandwidth_check(g, pid, &allowed, "after the cpuset write");
	if (v < 0)
		goto cleanup;

	CPU_CLR(v, &allowed);
	if (wait_throttled(pid))
		goto cleanup;
	if (write_cpu_online(v, 0)) {
		ret = skip("CPU %d cannot go offline", v);
		goto cleanup;
	}
	offline = v;
	w = bandwidth_check(g, pid, &allowed, "after the unplug");
	if (w < 0)
		goto cleanup;

	if (expect_write(q, "cpu.max", "max", 0, "the quota removed"))
		goto cleanup;
	i = procs_stuck(pid, BW_TASKS, 2000);
	if (i >= 0) {
		fprintf(stderr, "without the quota: spinner %d got no CPU time in 2 s\n", i);
		goto cleanup;
	}
	if (bandwidth_check(g, pid, &allowed, "without the quota") != w)
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (offline >= 0)
		write_cpu_online(offline, 1);
	for (i = 0; i < BW_TASKS; i++) {
		if (pid[i] > 0) {
			kill(pid[i], SIGKILL);
			waitpid(pid[i], NULL, 0);
		}
	}
	if (g)
		cg_write(g, "cpu.concurrency.max", "max");
	if (q)
		cg_destroy(q);
	if (r)
		cg_destroy(r);
	if (g)
		cg_destroy(g);
	free(q);
	free(r);
	free(g);
	return ret;
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

/*
 * The only CPU of a held task goes offline. held_setup() leaves b held,
 * pinned to {y}, with the placement work stalled. The steps:
 *  - a child of the test takes y offline. The test waits 400 ms for it,
 *    and prints whether the unplug ended while the stall was on;
 *  - the stall ends.
 * b has no CPU left in its mask. The kernel must widen the mask, as it
 * does for a task which runs when its CPU goes. Within 2 s of the end of
 * the stall:
 *  - a and b get CPU time, and are on the same CPU, which is not y;
 *  - Cpus_allowed_list of b is no longer "y" alone;
 *  - cpu.concurrency.effective of g reads "1 max".
 * Skipped with fewer than 4 CPUs, when y cannot go offline, or as
 * held_setup() says.
 *
 * Delay injection: the unplug pushes b off y itself, as it pushes every
 * queued task (balance_push()). conc_try_place() also handles a held task
 * whose mask has no active CPU. It only meets one if the placement work
 * runs after sched_cpu_deactivate() made y inactive and before
 * balance_push() moved b. That needs a delay in sched_cpu_deactivate()
 * after set_cpu_active(), and no stall. This case does not reach that
 * window.
 */
static int test_concurrency_held_offline(const char *root)
{
	int ret, i, st, done = 0, offline = -1;
	struct held_setup hs;
	char buf[128] = "";
	cpu_set_t allowed;
	pid_t pid[2], c;

	if (need_cpus(4))
		return KSFT_SKIP;
	ret = held_setup(&hs, root, "cc_test_held_off");
	if (ret != KSFT_PASS)
		goto cleanup;
	ret = KSFT_FAIL;
	c = fork();
	if (c < 0)
		goto cleanup;
	if (!c)
		_exit(write_cpu_online(hs.y, 0) ? 1 : 0);
	offline = hs.y;
	for (i = 0; i < 40 && !done; i++) {
		done = waitpid(c, &st, WNOHANG) == c;
		if (!done)
			usleep(10000);
	}
	ksft_print_msg("the unplug of the CPU of a held task %s while the placement work was stalled\n",
		       done ? "ended" : "did not end in 400 ms");
	stall_stop(&hs.st);
	hs.stalled = 0;
	if (!done)
		waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) {
		offline = -1;
		ret = skip("CPU %d cannot go offline", hs.y);
		goto cleanup;
	}
	pid[0] = hs.a;
	pid[1] = hs.b;
	i = procs_stuck(pid, 2, 2000);
	if (i >= 0) {
		fprintf(stderr, "%s got no CPU time in 2 s after the unplug and the stall\n",
			i ? "b" : "a");
		goto cleanup;
	}
	stress_all_cpus(&allowed);
	CPU_CLR(hs.y, &allowed);
	if (procs_on_one_cpu(pid, 2, &allowed, 2000, "after the unplug and the stall") < 0)
		goto cleanup;
	if (!proc_status(hs.b, "Cpus_allowed_list", buf, sizeof(buf)) ||
	    parse_cpulist(buf, &allowed) ||
	    (CPU_COUNT(&allowed) == 1 && CPU_ISSET(hs.y, &allowed))) {
		fprintf(stderr, "Cpus_allowed_list of b is \"%s\" after CPU %d went offline\n", buf,
			hs.y);
		goto cleanup;
	}
	if (cg_read_strcmp(hs.g, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "the effective limit is not 1 after the unplug\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	held_cleanup(&hs);
	if (offline >= 0)
		write_cpu_online(offline, 1);
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

/*
 * Run one shape of test_concurrency_rotation_share(): @n spinners under
 * the limit @limit. Return 0 if the checks listed there hold.
 */
static int rotation_share_shape(const char *root, int limit, int n)
{
	int ret = -1, i, stuck, fair = 100 * limit / n, lo = 100, hi = 0, ms;
	struct conc_stat before, after;
	long long sum = 0, dur;
	struct rot *r = NULL;
	char *g, buf[32];
	pid_t pid = -1;

	g = cg_name(root, "cc_test_rot_share");
	snprintf(buf, sizeof(buf), "%d", limit);
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", buf))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	for (i = 0; i < n; i++)
		rot_pinned(r, i, i, i);
	pid = rot_start(r, g, 0, n);
	if (pid < 0 || rot_wait_ready(r, 0, n) || conc_stat_read(g, &before))
		goto cleanup;
	/* Eight turns for each spinner, 3 s at least. */
	ms = MAX(3000, 8 * (n / limit) * rot_period_ms() * 5 / 4);
	rot_go(r, ms);
	stuck = rot_wait_done(r, 0, n);
	if (stuck >= 0) {
		fprintf(stderr, "%d over %d: spinner %d did not run for 3 s at the end\n", n, limit,
			stuck);
		goto cleanup;
	}
	if (conc_stat_read(g, &after) || rot_check_ids(r, 0, n, limit, "rotation_share"))
		goto cleanup;
	dur = r->end_ns - r->t0_ns;
	for (i = 0; i < n; i++) {
		lo = MIN(lo, rot_share(r, i));
		hi = MAX(hi, rot_share(r, i));
		sum += r->t[i].run_ns;
	}
	ksft_print_msg("%d spinners over the limit %d: shares from %d%% to %d%%, fair share %d%%, %lld rotations\n",
		       n, limit, lo, hi, fair, after.nr_rotations - before.nr_rotations);
	if (lo < fair / 2 || hi > fair * 3 / 2) {
		fprintf(stderr, "%d over %d: a share is not within half of the fair share\n", n,
			limit);
		goto cleanup;
	}
	if (sum > dur * limit * 102 / 100) {
		fprintf(stderr, "%d over %d: the spinners ran %lld ms in %lld ms\n", n, limit,
			sum / 1000000, dur / 1000000);
		goto cleanup;
	}
	if (after.nr_rotations == before.nr_rotations) {
		fprintf(stderr, "%d over %d: nr_rotations did not grow\n", n, limit);
		goto cleanup;
	}
	if (cg_read_strcmp(g, "cpu.concurrency.effective", limit == 1 ? "1 max\n" : "2 max\n")) {
		fprintf(stderr, "%d over %d: the effective value is not the limit\n", n, limit);
		goto cleanup;
	}
	ret = 0;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

/*
 * Spinners pinned to more CPUs than the limit share the slots fairly.
 *
 * Cgroup: g, with cpu.concurrency.max L. One process in g has N threads.
 * Thread i pins itself to CPU i and spins. The threads start to spin at
 * the same time and spin for 3 s or more: long enough for eight turns
 * each. Three shapes are run, each in a new g:
 *  - N = 2, L = 1;
 *  - N = 6, L = 2;
 *  - N = the number of CPUs, 16 at most, L = 2.
 *
 * Each thread adds up the time it spends on the CPU: the steps between
 * two clock reads which are shorter than 200 us. Checked for each shape:
 *  - every pin is accepted;
 *  - the share of each thread is between 0.5 and 1.5 times L / N of the
 *    measured time. A thread which never gets a slot has a share of 0.
 *    A thread which keeps its slot has a share of 100%;
 *  - the shares add up to L CPUs at most, with 2% for the hand-overs;
 *  - no thread reads an mm_cid at or above L;
 *  - nr_rotations of cpu.concurrency.stat grows;
 *  - cpu.concurrency.effective reads L.
 * Skipped with fewer than 6 CPUs.
 */
static int test_concurrency_rotation_share(const char *root)
{
	int n = MIN(get_nprocs(), 16);

	if (need_cpus(6))
		return KSFT_SKIP;
	if (rotation_share_shape(root, 1, 2) || rotation_share_shape(root, 2, 6))
		return KSFT_FAIL;
	if (n > 6 && rotation_share_shape(root, 2, n))
		return KSFT_FAIL;
	return KSFT_PASS;
}

/*
 * No task waits for a slot longer than its turn.
 *
 * Cgroup: g, with cpu.concurrency.max L. L is 2 with 8 CPUs or more, else
 * 1. One process in g has 4 * L threads. Thread i pins itself to CPU i and
 * spins. So three threads wait for each slot. They all start at the same
 * time and spin for 4 s.
 *
 * The kernel serves the waiting threads in the order of their holds. A
 * thread keeps a slot for one rotation period, and the kernel looks for
 * an expired period four times in a period. So a thread waits for three
 * turns of 1.25 periods at most.
 *
 * Each thread records its longest time off the CPU, from the common start
 * to the end. Checked: it is below 5 periods for every thread. That is
 * 4 * L threads / L slots + 1. A thread which is skipped once waits for 7
 * turns.
 * Skipped with fewer than 4 CPUs.
 */
static int test_concurrency_rotation_order(const char *root)
{
	int ret = KSFT_FAIL, limit = get_nprocs() >= 8 ? 2 : 1, n = 4 * limit, i, stuck;
	long long bound = 5LL * rot_period_ms() * 1000000, max = 0;
	struct rot *r = NULL;
	pid_t pid = -1;
	char *g;

	if (need_cpus(4))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_rot_order");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", limit == 2 ? "2" : "1"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	for (i = 0; i < n; i++)
		rot_pinned(r, i, i, i);
	pid = rot_start(r, g, 0, n);
	if (pid < 0 || rot_wait_ready(r, 0, n))
		goto cleanup;
	rot_go(r, 4000);
	stuck = rot_wait_done(r, 0, n);
	if (stuck >= 0) {
		fprintf(stderr, "thread %d did not run for 3 s at the end\n", stuck);
		goto cleanup;
	}
	for (i = 0; i < n; i++)
		max = MAX(max, r->t[i].max_gap_ns);
	ksft_print_msg("%d spinners over the limit %d: the longest wait is %lld ms, the bound %lld ms\n",
		       n, limit, max / 1000000, bound / 1000000);
	for (i = 0; i < n; i++) {
		if (r->t[i].max_gap_ns >= bound) {
			fprintf(stderr, "thread %d was off the CPU for %lld ms\n", i,
				r->t[i].max_gap_ns / 1000000);
			goto cleanup;
		}
	}
	if (rot_check_ids(r, 0, n, limit, "rotation_order"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

/*
 * A configuration which fits the limit is served without a rotation.
 *
 * Cgroup: g, with cpu.concurrency.max 2. One process in g has three
 * threads:
 *  - two spin and can run on every CPU;
 *  - w is pinned to the last CPU. It wakes up every 20 ms and spins for
 *    1 ms.
 * They run for 2 s. Two slots serve them all: one on the last CPU and one
 * anywhere. The kernel must move a spinner and its slot to the last CPU
 * when w first finds no slot there. From then on w finds a slot at each
 * wakeup.
 *
 * Checked:
 *  - nr_rotations of cpu.concurrency.stat and the count of
 *    cpu.concurrency.events do not change during the 2 s;
 *  - the median of the wakeup latencies of w is below a quarter of the
 *    rotation period. w measures a latency from the time it asked to wake
 *    up to its first clock read after it. A task which waits for a
 *    rotation is one period late;
 *  - w woke up 50 times or more, of the 99 it asks for;
 *  - no thread reads an mm_cid of 2 or more.
 * Skipped with fewer than 4 CPUs.
 */
static int test_concurrency_rotation_fits(const char *root)
{
	int ret = KSFT_FAIL, stuck, median, quarter_us = rot_period_ms() * 1000 / 4;
	struct conc_stat before, after;
	long long ev0, ev1;
	struct rot *r = NULL;
	pid_t pid = -1;
	char *g;

	if (need_cpus(4))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_rot_fits");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "2"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	r->t[2].kind = ROT_PERIODIC;
	r->t[2].cpu_lo = r->t[2].cpu_hi = get_nprocs() - 1;
	r->t[2].period_us = 20000;
	r->t[2].work_us = 1000;
	pid = rot_start(r, g, 0, 3);
	if (pid < 0 || rot_wait_ready(r, 0, 3))
		goto cleanup;
	ev0 = conc_events_rotation(g);
	if (conc_stat_read(g, &before) || ev0 < 0) {
		fprintf(stderr, "cpu.concurrency.stat or cpu.concurrency.events cannot be read\n");
		goto cleanup;
	}
	rot_go(r, 2000);
	stuck = rot_wait_done(r, 0, 3);
	if (stuck >= 0) {
		fprintf(stderr, "thread %d did not run for 3 s at the end\n", stuck);
		goto cleanup;
	}
	ev1 = conc_events_rotation(g);
	if (conc_stat_read(g, &after))
		goto cleanup;
	median = rot_lat_median_us(r, 2);
	ksft_print_msg("%d wakeups, median latency %d us, %lld holds, %lld rotations\n",
		       r->t[2].nlat, median, after.nr_holds - before.nr_holds,
		       after.nr_rotations - before.nr_rotations);
	if (after.nr_rotations != before.nr_rotations || ev1 != ev0) {
		fprintf(stderr, "nr_rotations went from %lld to %lld, the event count from %lld to %lld\n",
			before.nr_rotations, after.nr_rotations, ev0, ev1);
		goto cleanup;
	}
	if (r->t[2].nlat < 50 || median >= quarter_us) {
		fprintf(stderr, "the pinned thread woke up %d times, median latency %d us\n",
			r->t[2].nlat, median);
		goto cleanup;
	}
	if (rot_check_ids(r, 0, 3, 2, "rotation_fits"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

struct rot_mask {
	int lo, hi;
};

/*
 * Run @n spinners for @ms in a new cgroup @name with the limit @limit, 1
 * or 2. They are the threads of one process. Spinner i pins itself to
 * CPUs m[i].lo to m[i].hi. With @fifo, each then becomes SCHED_FIFO with
 * that priority. Return 0 if all this holds:
 *  - every pin is accepted;
 *  - each spinner runs for @min_share percent of the time or more;
 *  - the spinners run for @limit CPUs at most in all, with 2% for the
 *    hand-overs;
 *  - no spinner reads an mm_cid at or above @limit;
 *  - cpu.concurrency.effective reads @limit.
 */
static int rot_pinned_run(const char *root, const char *name, int limit, const struct rot_mask *m,
			  int n, int ms, int min_share, int fifo)
{
	struct rot *r = NULL;
	int ret = -1, i, stuck;
	long long sum = 0, max_sum;
	pid_t pid = -1;
	char *g;

	g = cg_name(root, name);
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", limit == 1 ? "1" : "2"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	for (i = 0; i < n; i++) {
		rot_pinned(r, i, m[i].lo, m[i].hi);
		r->t[i].fifo = fifo;
	}
	pid = rot_start(r, g, 0, n);
	if (pid < 0 || rot_wait_ready(r, 0, n))
		goto cleanup;
	rot_go(r, ms);
	stuck = rot_wait_done(r, 0, n);
	if (stuck >= 0) {
		fprintf(stderr, "%s: spinner %d did not run for 3 s at the end\n", name, stuck);
		goto cleanup;
	}
	for (i = 0; i < n; i++)
		sum += r->t[i].run_ns;
	max_sum = (r->end_ns - r->t0_ns) * limit * 102 / 100;
	for (i = 0; i < n; i++) {
		if (rot_share(r, i) >= min_share && sum <= max_sum)
			continue;
		fprintf(stderr, "%s: the spinners ran", name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %d%%", rot_share(r, i));
		fprintf(stderr, " of the time; each must run %d%% or more, all %d%% at most\n",
			min_share, limit * 102);
		goto cleanup;
	}
	if (rot_check_ids(r, 0, n, limit, name))
		goto cleanup;
	if (cg_read_strcmp(g, "cpu.concurrency.effective", limit == 1 ? "1 max\n" : "2 max\n")) {
		fprintf(stderr, "%s: the effective value is not the limit %d\n", name, limit);
		goto cleanup;
	}
	ret = 0;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

/* Return true if a task without a limit can become SCHED_FIFO. */
static bool can_sched_fifo(void)
{
	struct sched_param prio = { .sched_priority = 1 };
	int st = 1;
	pid_t c;

	c = fork();
	if (!c)
		_exit(sched_setscheduler(0, SCHED_FIFO, &prio) ? 1 : 0);
	if (c > 0)
		waitpid(c, &st, 0);
	return c > 0 && WIFEXITED(st) && !WEXITSTATUS(st);
}

/*
 * A real-time task takes the slot of fair tasks at once. Fair tasks never
 * take the slot of a real-time task.
 *
 * First part. Cgroup: g, with cpu.concurrency.max 2. One process in g has
 * three threads:
 *  - two fair threads spin, pinned to CPU 1 and to CPU 2. They use both
 *    slots;
 *  - w is SCHED_FIFO and pinned to CPU 3, which g does not use. It wakes
 *    up every 20 ms and spins for 1 ms.
 * They run for 2 s. Checked:
 *  - the median of the wakeup latencies of w is below a quarter of the
 *    rotation period, see test_concurrency_rotation_fits(). A fair task
 *    in the place of w is one period late at each wakeup;
 *  - w woke up 50 times or more, of the 99 it asks for;
 *  - each fair thread runs for half of the 2 s or more. w needs a slot
 *    for 1 ms in 20;
 *  - no thread reads an mm_cid of 2 or more.
 *
 * Second part. Cgroup: h, with cpu.concurrency.max 1. One process in h
 * has two threads, which spin for 1.5 s from the same time:
 *  - s is SCHED_FIFO and pinned to CPU 1;
 *  - f is fair and pinned to CPU 2.
 * Checked:
 *  - s is never off the CPU for three quarters of a rotation period. A
 *    rotation would take its slot for a period. The fair server may take
 *    its CPU for 50 ms in a second;
 *  - s runs for 80% of the time or more, and f for 15% at most;
 *  - nr_rotations of h grows by 1 at most: s may take the slot from f
 *    once, at the start;
 *  - f ends within 3 s of the end of s: it gets the slot which s leaves;
 *  - no thread reads an mm_cid of 1 or more.
 *
 * Third part. Cgroup: k, with cpu.concurrency.max 1. One process in k has
 * two SCHED_FIFO threads of the same priority, pinned to CPU 1 and to
 * CPU 2. They spin for 2 s from the same time, see rot_pinned_run().
 * Checked: each runs for a quarter of the time or more. Two tasks of one
 * class share a slot by rotation, whatever the class. The fair share is
 * half of the time.
 *
 * Skipped with fewer than 4 CPUs, or when a task without a limit cannot
 * become SCHED_FIFO.
 */
static int test_concurrency_rotation_rt(const char *root)
{
	static const struct rot_mask two[] = { { 1, 1 }, { 2, 2 } };
	int ret = KSFT_FAIL, stuck, median, period = rot_period_ms(), i;
	struct conc_stat before, after;
	struct rot *r = NULL, *r2 = NULL;
	pid_t pid = -1, pid2 = -1;
	char *g = NULL, *h = NULL;

	if (need_cpus(4))
		return KSFT_SKIP;
	if (!can_sched_fifo())
		return skip("no SCHED_FIFO task without a limit");
	g = cg_name(root, "cc_test_rot_rt");
	h = cg_name(root, "cc_test_rot_rt2");
	if (!g || !h || cg_create(g) || cg_create(h) || cg_write(g, "cpu.concurrency.max", "2") ||
	    cg_write(h, "cpu.concurrency.max", "1"))
		goto cleanup;
	r = rot_alloc();
	r2 = rot_alloc();
	if (!r || !r2)
		goto cleanup;

	rot_pinned(r, 0, 1, 1);
	rot_pinned(r, 1, 2, 2);
	rot_pinned(r, 2, 3, 3);
	r->t[2].kind = ROT_PERIODIC;
	r->t[2].fifo = 1;
	r->t[2].period_us = 20000;
	r->t[2].work_us = 1000;
	pid = rot_start(r, g, 0, 3);
	if (pid < 0 || rot_wait_ready(r, 0, 3))
		goto cleanup;
	rot_go(r, 2000);
	stuck = rot_wait_done(r, 0, 3);
	if (stuck >= 0) {
		fprintf(stderr, "first part: thread %d did not run for 3 s at the end\n", stuck);
		goto cleanup;
	}
	median = rot_lat_median_us(r, 2);
	ksft_print_msg("SCHED_FIFO waiter: %d wakeups, median latency %d us; the fair spinners ran %d%% and %d%%\n",
		       r->t[2].nlat, median, rot_share(r, 0), rot_share(r, 1));
	if (r->t[2].nlat < 50 || median >= period * 1000 / 4) {
		fprintf(stderr, "the SCHED_FIFO thread woke up %d times, median latency %d us\n",
			r->t[2].nlat, median);
		goto cleanup;
	}
	if (rot_share(r, 0) < 50 || rot_share(r, 1) < 50) {
		fprintf(stderr, "the fair spinners ran %d%% and %d%% of the time\n",
			rot_share(r, 0), rot_share(r, 1));
		goto cleanup;
	}
	if (rot_check_ids(r, 0, 3, 2, "rotation_rt, first part"))
		goto cleanup;
	rot_end(r, &pid, 1);

	rot_pinned(r2, 0, 1, 1);
	r2->t[0].fifo = 1;
	rot_pinned(r2, 1, 2, 2);
	pid2 = rot_start(r2, h, 0, 2);
	if (pid2 < 0 || rot_wait_ready(r2, 0, 2) || conc_stat_read(h, &before))
		goto cleanup;
	rot_go(r2, 1500);
	stuck = rot_wait_done(r2, 0, 2);
	if (stuck >= 0) {
		fprintf(stderr, "second part: thread %d did not run for 3 s at the end\n", stuck);
		goto cleanup;
	}
	if (conc_stat_read(h, &after))
		goto cleanup;
	ksft_print_msg("SCHED_FIFO spinner: ran %d%%, longest time off the CPU %lld ms; the fair waiter ran %d%%; %lld rotations\n",
		       rot_share(r2, 0), r2->t[0].max_gap_ns / 1000000, rot_share(r2, 1),
		       after.nr_rotations - before.nr_rotations);
	if (r2->t[0].max_gap_ns >= period * 750000LL || rot_share(r2, 0) < 80 ||
	    rot_share(r2, 1) > 15 || after.nr_rotations - before.nr_rotations > 1) {
		fprintf(stderr, "a fair waiter took the slot of a SCHED_FIFO spinner\n");
		goto cleanup;
	}
	if (rot_check_ids(r2, 0, 2, 1, "rotation_rt, second part"))
		goto cleanup;
	rot_end(r2, &pid2, 1);

	if (rot_pinned_run(root, "cc_test_rot_rt3", 1, two, 2, 2000, 25, 1))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (r2) {
		rot_end(r2, &pid2, 1);
		munmap(r2, sizeof(*r2));
	}
	for (i = 0; i < 2; i++) {
		char *cg = i ? h : g;

		if (cg)
			cg_destroy(cg);
		free(cg);
	}
	return ret;
}

/*
 * A real-time task of another cgroup does not keep a slot from rotating.
 *
 * Cgroups: g and "out". g has cpu.concurrency.max 1. out has no limit.
 * Tasks, which all spin for 3 s from the same time:
 *  - a and b, two fair threads of one process in g. a is pinned to CPU 1
 *    and b to CPU 2;
 *  - h, a SCHED_FIFO process in out, pinned to CPU 1.
 * h does not belong to g and holds none of its slots. It keeps a off
 * CPU 1 but for the time of the fair server. Nothing else runs on CPU 2.
 *
 * a and b share the one slot of g: each may keep it for a rotation
 * period. So b must run on CPU 2 for about half of the time, whatever a
 * gets on CPU 1 during its turns.
 *
 * Checked:
 *  - b runs for 25% of the time or more;
 *  - b is never off the CPU for 5 rotation periods;
 *  - a and b run for one CPU at most together, with 2% for the
 *    hand-overs;
 *  - neither reads an mm_cid of 1 or more.
 * Skipped with fewer than 3 CPUs, or when a task without a limit cannot
 * become SCHED_FIFO.
 */
static int test_concurrency_rotation_foreign_rt(const char *root)
{
	int ret = KSFT_FAIL, stuck, period = rot_period_ms();
	pid_t pid[2] = { -1, -1 };
	char *g = NULL, *out = NULL;
	struct rot *r = NULL;
	long long dur;

	if (need_cpus(3))
		return KSFT_SKIP;
	if (!can_sched_fifo())
		return skip("no SCHED_FIFO task without a limit");
	g = cg_name(root, "cc_test_rot_foreign");
	out = cg_name(root, "cc_test_rot_foreign_out");
	if (!g || !out || cg_create(g) || cg_create(out) || cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	rot_pinned(r, 0, 1, 1);
	rot_pinned(r, 1, 2, 2);
	rot_pinned(r, 2, 1, 1);
	r->t[2].fifo = 1;
	pid[0] = rot_start(r, g, 0, 2);
	pid[1] = rot_start(r, out, 2, 1);
	if (pid[0] < 0 || pid[1] < 0 || rot_wait_ready(r, 0, 3))
		goto cleanup;
	rot_go(r, 3000);
	stuck = rot_wait_done(r, 0, 3);
	if (stuck >= 0) {
		fprintf(stderr, "task %d did not run for 3 s at the end\n", stuck);
		goto cleanup;
	}
	dur = r->end_ns - r->t0_ns;
	ksft_print_msg("a ran %d%% under the SCHED_FIFO task, b ran %d%%, longest time off the CPU %lld ms\n",
		       rot_share(r, 0), rot_share(r, 1), r->t[1].max_gap_ns / 1000000);
	if (rot_share(r, 1) < 25 || r->t[1].max_gap_ns >= period * 5000000LL) {
		fprintf(stderr, "b did not get the slot of g while a SCHED_FIFO task of another cgroup ran on the CPU of a\n");
		goto cleanup;
	}
	if (r->t[0].run_ns + r->t[1].run_ns > dur * 102 / 100) {
		fprintf(stderr, "a and b ran %lld ms in %lld ms\n",
			(r->t[0].run_ns + r->t[1].run_ns) / 1000000, dur / 1000000);
		goto cleanup;
	}
	if (rot_check_ids(r, 0, 2, 1, "rotation_foreign_rt"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, pid, 2);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	if (out)
		cg_destroy(out);
	free(g);
	free(out);
	return ret;
}

/*
 * Return 1 if the file of @fd raises a notification within @ms, 0 if not,
 * and -1 on an error. The file is read first: only a change after this
 * read counts.
 */
static int file_notified(int fd, int ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLPRI };
	char buf[64];
	int n;

	if (lseek(fd, 0, SEEK_SET) || read(fd, buf, sizeof(buf)) < 0)
		return -1;
	n = poll(&pfd, 1, ms);
	if (n < 0)
		return -1;
	return n && (pfd.revents & POLLPRI);
}

/*
 * Return 0 if no counter of @b is below its value in @a, and nr_held of
 * @b is from 0 to @tasks. Else print both and return -1.
 */
static int stat_grew(const struct conc_stat *a, const struct conc_stat *b, int tasks)
{
	if (b->nr_holds >= a->nr_holds && b->held_usec >= a->held_usec &&
	    b->nr_rotations >= a->nr_rotations && b->rotated_usec >= a->rotated_usec &&
	    b->nr_held >= 0 && b->nr_held <= tasks)
		return 0;
	fprintf(stderr, "cpu.concurrency.stat went from nr_holds %lld held_usec %lld nr_rotations %lld rotated_usec %lld\n"
		"to nr_held %lld nr_holds %lld held_usec %lld nr_rotations %lld rotated_usec %lld\n",
		a->nr_holds, a->held_usec, a->nr_rotations, a->rotated_usec,
		b->nr_held, b->nr_holds, b->held_usec, b->nr_rotations, b->rotated_usec);
	return -1;
}

/*
 * The counters of cpu.concurrency.stat and cpu.concurrency.events.
 *
 * Cgroups: p, and its child c with cpu.concurrency.max 1. Both are new, so
 * their counters start at 0. The test holds cpu.concurrency.events of c
 * open and uses poll() with POLLPRI on it, as for memory.events. The
 * steps:
 *  1. One process with one thread spins in c for 500 ms. It can run on
 *     every CPU. poll() must report nothing during these 500 ms, and
 *     nr_rotations of c must still be 0.
 *  2. One process with two threads runs in c. The threads pin themselves
 *     to CPU 1 and to CPU 2, and spin for 3 s from the same time. One of
 *     them is held at any time. poll() must report a change within 1 s of
 *     their start. The test then reads the file of c every 100 ms. No
 *     counter but nr_held may go down, and nr_held must be 0, 1 or 2.
 *  3. The process is killed and reaped. nr_held of c must be 0 within
 *     2 s. poll() must report nothing for 300 ms.
 *
 * Checked at the end, on the counts of step 2:
 *  - nr_rotations is from 15 to 32 with the default period of 100 ms. A
 *    slot is taken when it is one period old, and the kernel looks four
 *    times in a period. That is one rotation every 100 to 125 ms: 24 to
 *    30 in 3 s. The test accepts half of that, for a busy machine, and
 *    two more;
 *  - nr_holds grew by the number of rotations or more: each rotation
 *    holds the task which loses the slot;
 *  - held_usec grew by 1.5 s or more: one of the two threads is held at
 *    any time;
 *  - rotated_usec is above 0, and not above held_usec;
 *  - "rotation" of cpu.concurrency.events of c equals nr_rotations of c;
 *  - the four counters of p equal those of c, and so does its "rotation"
 *    count: p counts its child;
 *  - the sched_conc_rotate event was recorded with the cgroup ID of c.
 *    This is checked when tracefs can be used.
 * Skipped with fewer than 3 CPUs.
 */
static int test_concurrency_rotation_stat(const char *root)
{
	int ret = KSFT_FAIL, fd = -1, stuck, i, period = rot_period_ms(), expect;
	struct conc_stat st, prev, start, end, top;
	char *p = NULL, *c = NULL, path[PATH_MAX], text[64];
	struct rot *r = NULL, *r2 = NULL;
	pid_t pid = -1, pid2 = -1;
	long long rotations;

	if (need_cpus(3))
		return KSFT_SKIP;
	p = cg_name(root, "cc_test_rot_stat");
	c = p ? cg_name(p, "c") : NULL;
	if (!c || cg_create(p) || cg_write(p, "cgroup.subtree_control", "+cpu") || cg_create(c) ||
	    cg_write(c, "cpu.concurrency.max", "1"))
		goto cleanup;
	snprintf(path, sizeof(path), "%s/cpu.concurrency.events", c);
	fd = open(path, O_RDONLY);
	r = rot_alloc();
	r2 = rot_alloc();
	if (fd < 0 || !r || !r2)
		goto cleanup;
	if (conc_stat_read(c, &st) || st.nr_held || st.nr_holds || st.held_usec ||
	    st.nr_rotations || st.rotated_usec || conc_events_rotation(c)) {
		fprintf(stderr, "the counters of a new cgroup are not 0\n");
		goto cleanup;
	}
	conc_trace_start();
	conc_trace_clear();

	/* Step 1: nothing to rotate. */
	pid = rot_start(r, c, 0, 1);
	if (pid < 0 || rot_wait_ready(r, 0, 1))
		goto cleanup;
	rot_go(r, 500);
	if (file_notified(fd, 500)) {
		fprintf(stderr, "cpu.concurrency.events notified with one spinner under the limit 1\n");
		goto cleanup;
	}
	stuck = rot_wait_done(r, 0, 1);
	if (stuck >= 0 || conc_stat_read(c, &st) || st.nr_rotations) {
		fprintf(stderr, "one spinner under the limit 1: it did not end, or nr_rotations is not 0\n");
		goto cleanup;
	}
	rot_end(r, &pid, 1);

	/* Step 2: two pinned spinners, one slot. */
	rot_pinned(r2, 0, 1, 1);
	rot_pinned(r2, 1, 2, 2);
	pid2 = rot_start(r2, c, 0, 2);
	if (pid2 < 0 || rot_wait_ready(r2, 0, 2) || conc_stat_read(c, &start))
		goto cleanup;
	rot_go(r2, 3000);
	if (file_notified(fd, 1050) != 1) {
		fprintf(stderr, "cpu.concurrency.events did not notify within 1 s of the start\n");
		goto cleanup;
	}
	prev = start;
	while (mono_ns() < r2->end_ns) {
		usleep(100000);
		if (conc_stat_read(c, &st) || stat_grew(&prev, &st, 2))
			goto cleanup;
		prev = st;
	}
	stuck = rot_wait_done(r2, 0, 2);
	if (stuck >= 0) {
		fprintf(stderr, "thread %d did not run for 3 s at the end\n", stuck);
		goto cleanup;
	}
	if (conc_stat_read(c, &end))
		goto cleanup;

	/* Step 3: nothing runs. */
	rot_end(r2, &pid2, 1);
	for (i = 0; i < 200; i++) {
		if (conc_stat_read(c, &st))
			goto cleanup;
		if (!st.nr_held)
			break;
		usleep(10000);
	}
	if (st.nr_held) {
		fprintf(stderr, "nr_held is %lld 2 s after the tasks were killed\n", st.nr_held);
		goto cleanup;
	}
	if (file_notified(fd, 300)) {
		fprintf(stderr, "cpu.concurrency.events notified while nothing runs\n");
		goto cleanup;
	}

	rotations = end.nr_rotations - start.nr_rotations;
	expect = 3000 / period;
	ksft_print_msg("3 s of 2 spinners over 1 slot: %lld rotations, %lld holds, held %lld ms, of which %lld ms ended by a rotation\n",
		       rotations, end.nr_holds - start.nr_holds,
		       (end.held_usec - start.held_usec) / 1000,
		       (end.rotated_usec - start.rotated_usec) / 1000);
	if (rotations < expect / 2 || rotations > expect + 2) {
		fprintf(stderr, "%lld rotations in 3 s, from %d to %d expected\n", rotations,
			expect / 2, expect + 2);
		goto cleanup;
	}
	if (end.nr_holds - start.nr_holds < rotations ||
	    end.held_usec - start.held_usec < 1500000 || !st.rotated_usec ||
	    st.rotated_usec > st.held_usec) {
		fprintf(stderr, "nr_holds, held_usec or rotated_usec does not match the run\n");
		goto cleanup;
	}
	if (conc_events_rotation(c) != st.nr_rotations) {
		fprintf(stderr, "cpu.concurrency.events of c reads %lld, nr_rotations %lld\n",
			conc_events_rotation(c), st.nr_rotations);
		goto cleanup;
	}
	if (conc_stat_read(p, &top) || top.nr_held || top.nr_holds != st.nr_holds ||
	    top.held_usec != st.held_usec || top.nr_rotations != st.nr_rotations ||
	    top.rotated_usec != st.rotated_usec || conc_events_rotation(p) != st.nr_rotations) {
		fprintf(stderr, "the counters of p differ from those of its only child\n");
		stat_grew(&st, &top, 0);
		goto cleanup;
	}
	snprintf(text, sizeof(text), "cgroup=%llu", ns_cid_cgroup_id(c));
	if (conc_trace_find("sched_conc_rotate:", text))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	conc_trace_stop();
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (r2) {
		rot_end(r2, &pid2, 1);
		munmap(r2, sizeof(*r2));
	}
	if (fd >= 0)
		close(fd);
	if (c)
		cg_destroy(c);
	if (p)
		cg_destroy(p);
	free(c);
	free(p);
	return ret;
}

/*
 * Run two pinned spinners over one slot for 2 s with the rotation period
 * @ms. Store the mean wait of a spinner in @wait_ms and the rotations in
 * @rotations. Return 0, or -1 on a failure.
 */
static int rotation_period_run(const char *root, int ms, long *wait_ms, long long *rotations)
{
	struct conc_stat before, after;
	struct rot *r = NULL;
	int ret = -1, stuck;
	pid_t pid = -1;
	char *g, buf[16];

	g = cg_name(root, "cc_test_rot_period");
	snprintf(buf, sizeof(buf), "%d\n", ms);
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "1") ||
	    file_write(ROT_PERIOD_FILE, buf))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	rot_pinned(r, 0, 1, 1);
	rot_pinned(r, 1, 2, 2);
	r->t[0].wait_min_ns = r->t[1].wait_min_ns = ms * 500000LL;
	pid = rot_start(r, g, 0, 2);
	if (pid < 0 || rot_wait_ready(r, 0, 2) || conc_stat_read(g, &before))
		goto cleanup;
	rot_go(r, 2000);
	stuck = rot_wait_done(r, 0, 2);
	if (stuck >= 0 || conc_stat_read(g, &after)) {
		fprintf(stderr, "period %d ms: a spinner did not run for 3 s at the end\n", ms);
		goto cleanup;
	}
	if (!r->t[0].nwaits || !r->t[1].nwaits) {
		fprintf(stderr, "period %d ms: a spinner never waited\n", ms);
		goto cleanup;
	}
	*wait_ms = (r->t[0].wait_ns + r->t[1].wait_ns) / (r->t[0].nwaits + r->t[1].nwaits) /
		   1000000;
	*rotations = after.nr_rotations - before.nr_rotations;
	ret = 0;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

/*
 * The rotation period of debugfs sets how long a held fair task waits.
 *
 * Cgroup: g, with cpu.concurrency.max 1. One process in g has two threads,
 * pinned to CPU 1 and to CPU 2. They spin for 2 s from the same time, so
 * one runs while the other waits for the slot. This is run twice, in a
 * new g: with 100 written to sched/conc_rotation_ms, then with 20.
 *
 * A thread counts a time off the CPU as a wait when it lasts half a
 * period or more. Checked:
 *  - with 100 ms, the mean wait is 80 ms or more. A waiter gets a slot
 *    which ran its own class after one period at the earliest;
 *  - with 20 ms, the mean wait is below 60 ms. A kernel which ignores the
 *    file makes the waiter wait 100 ms;
 *  - nr_rotations grows at least twice as much with 20 ms as with 100 ms.
 * The value read from the file at the start is written back at the end.
 * Skipped with fewer than 3 CPUs, or without the debugfs file.
 */
static int test_concurrency_rotation_period(const char *root)
{
	long long rot_long = 0, rot_short = 0;
	long wait_long = 0, wait_short = 0;
	int ret = KSFT_FAIL;
	char saved[32];

	if (need_cpus(3))
		return KSFT_SKIP;
	if (file_read(ROT_PERIOD_FILE, saved, sizeof(saved)))
		return skip("no %s", ROT_PERIOD_FILE);
	if (rotation_period_run(root, 100, &wait_long, &rot_long) ||
	    rotation_period_run(root, 20, &wait_short, &rot_short))
		goto cleanup;
	ksft_print_msg("period 100 ms: mean wait %ld ms, %lld rotations; period 20 ms: mean wait %ld ms, %lld rotations\n",
		       wait_long, rot_long, wait_short, rot_short);
	if (wait_long < 80 || wait_short >= 60 || rot_short < 2 * rot_long) {
		fprintf(stderr, "the waits do not follow the rotation period\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (file_write(ROT_PERIOD_FILE, saved)) {
		fprintf(stderr, "%s not restored to %s", ROT_PERIOD_FILE, saved);
		ret = KSFT_FAIL;
	}
	return ret;
}

/*
 * Masks which overlap without nesting are accepted under a limit.
 *
 * Two shapes are run, each for 2 s in a new cgroup, see rot_pinned_run():
 *  - limit 1: two spinners pinned to {1,2} and to {2,3};
 *  - limit 2: four spinners pinned to {1,2}, {2,3}, {3,4} and {1,2}.
 * The fair share of a spinner is half a CPU in both shapes. Checked: each
 * runs for a quarter of the time or more. The other checks are those of
 * rot_pinned_run().
 * Skipped with fewer than 5 CPUs.
 */
static int test_concurrency_rotation_overlap(const char *root)
{
	static const struct rot_mask two[] = { { 1, 2 }, { 2, 3 } };
	static const struct rot_mask four[] = { { 1, 2 }, { 2, 3 }, { 3, 4 }, { 1, 2 } };

	if (need_cpus(5))
		return KSFT_SKIP;
	if (rot_pinned_run(root, "cc_test_rot_overlap1", 1, two, 2, 2000, 25, 0) ||
	    rot_pinned_run(root, "cc_test_rot_overlap2", 2, four, 4, 2000, 25, 0))
		return KSFT_FAIL;
	return KSFT_PASS;
}

/*
 * A limited cgroup below a limited cgroup, both with too many pinned
 * tasks.
 *
 * Cgroups: p, and its threaded children c and d. The test writes 1 to
 * cpu.concurrency.max of c, then 2 to that of p. d has no limit of its
 * own.
 *
 * One process has four threads, which spin for 4 s from the same time:
 *  - c1 and c2 stay in c, pinned to CPU 1 and to CPU 2;
 *  - d1 and d2 move to d, pinned to CPU 3 and to CPU 4.
 * c may use one CPU at a time and p two. So the four threads ask for
 * three CPUs under p, and c1 and c2 ask for two under c.
 *
 * Checked:
 *  - every move and every pin is accepted;
 *  - cpu.concurrency.effective reads 2 for p and d, and 1 for c, within
 *    2 s of the moves. A larger value shows while the kernel still holds
 *    the threads which arrived over a limit;
 *  - c1 and c2 run for one CPU at most together, and the four threads for
 *    two CPUs at most, each with 2% for the hand-overs;
 *  - each thread runs for 10% of the time or more, and is never off the
 *    CPU for 10 rotation periods. With two slots for four threads the
 *    fair share is 50%;
 *  - no thread reads an mm_cid of 2 or more.
 * Skipped with fewer than 5 CPUs, or without threaded cgroups.
 */
static int test_concurrency_rotation_nested(const char *root)
{
	char *p = NULL, *c = NULL, *d = NULL;
	int ret = KSFT_FAIL, i, stuck;
	long long dur, sum = 0;
	struct rot *r = NULL;
	pid_t pid = -1;

	if (need_cpus(5))
		return KSFT_SKIP;
	p = cg_name(root, "cc_test_rot_nested");
	c = p ? cg_name(p, "c") : NULL;
	d = p ? cg_name(p, "d") : NULL;
	if (!c || !d || cg_create(p) || cg_write(p, "cgroup.subtree_control", "+cpu") ||
	    cg_create(c) || cg_create(d))
		goto cleanup;
	if (cg_write(c, "cgroup.type", "threaded") || cg_write(d, "cgroup.type", "threaded")) {
		ret = skip("no threaded cgroups");
		goto cleanup;
	}
	if (cg_write(c, "cpu.concurrency.max", "1") || cg_write(p, "cpu.concurrency.max", "2"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	for (i = 0; i < 4; i++)
		rot_pinned(r, i, i + 1, i + 1);
	r->t[2].cg = d;
	r->t[3].cg = d;
	pid = rot_start(r, c, 0, 4);
	if (pid < 0 || rot_wait_ready(r, 0, 4))
		goto cleanup;
	if (wait_effective(p, "2 max\n", 2000) || wait_effective(d, "2 max\n", 2000) ||
	    wait_effective(c, "1 max\n", 2000)) {
		char val[3][32] = { "?", "?", "?" };

		cg_read(p, "cpu.concurrency.effective", val[0], sizeof(val[0]));
		cg_read(d, "cpu.concurrency.effective", val[1], sizeof(val[1]));
		cg_read(c, "cpu.concurrency.effective", val[2], sizeof(val[2]));
		fprintf(stderr, "the effective values of p, d and c are \"%.*s\", \"%.*s\" and \"%.*s\"\n",
			(int)strcspn(val[0], "\n"), val[0], (int)strcspn(val[1], "\n"), val[1],
			(int)strcspn(val[2], "\n"), val[2]);
		goto cleanup;
	}
	rot_go(r, 4000);
	stuck = rot_wait_done(r, 0, 4);
	if (stuck >= 0) {
		fprintf(stderr, "thread %d did not run for 3 s at the end\n", stuck);
		goto cleanup;
	}
	dur = r->end_ns - r->t0_ns;
	ksft_print_msg("shares: c1 %d%%, c2 %d%%, d1 %d%%, d2 %d%%; longest times off the CPU: %lld, %lld, %lld, %lld ms\n",
		       rot_share(r, 0), rot_share(r, 1), rot_share(r, 2), rot_share(r, 3),
		       r->t[0].max_gap_ns / 1000000, r->t[1].max_gap_ns / 1000000,
		       r->t[2].max_gap_ns / 1000000, r->t[3].max_gap_ns / 1000000);
	for (i = 0; i < 4; i++) {
		sum += r->t[i].run_ns;
		if (rot_share(r, i) < 10 || r->t[i].max_gap_ns >= 10000000LL * rot_period_ms()) {
			fprintf(stderr, "thread %d ran %d%% of the time, and was off the CPU for %lld ms\n",
				i, rot_share(r, i), r->t[i].max_gap_ns / 1000000);
			goto cleanup;
		}
	}
	if (r->t[0].run_ns + r->t[1].run_ns > dur * 102 / 100 || sum > dur * 204 / 100) {
		fprintf(stderr, "the threads of c ran %lld ms, all four %lld ms, in %lld ms\n",
			(r->t[0].run_ns + r->t[1].run_ns) / 1000000, sum / 1000000, dur / 1000000);
		goto cleanup;
	}
	if (rot_check_ids(r, 0, 4, 2, "rotation_nested"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (c)
		cg_destroy(c);
	if (d)
		cg_destroy(d);
	if (p)
		cg_destroy(p);
	free(c);
	free(d);
	free(p);
	return ret;
}

/*
 * A cpuset write may leave a cgroup fewer CPUs than its limit.
 *
 * Cgroup: g, with cpu.concurrency.max 2 and cpuset.cpus 1-3. One process
 * in g has three threads, pinned to CPU 1, to CPU 2 and to CPU 3. They
 * spin until the end of the test. The steps, each followed by 1.5 s of
 * measurement:
 *  1. Nothing: three pinned threads share two slots.
 *  2. "1" is written to cpuset.cpus of g. The three threads share CPU 1.
 *  3. "1-3" is written to cpuset.cpus of g.
 *
 * Checked:
 *  - both writes are accepted;
 *  - cpu.concurrency.effective reads 2 within 2 s of each write;
 *  - in each measurement, each thread runs for 10% of the time or more.
 *    The fair share is 66% in steps 1 and 3, and 33% in step 2;
 *  - together the threads run for two CPUs at most in steps 1 and 3.
 *    They run for one CPU at most in step 2. rot_window() allows 4%
 *    more;
 *  - no thread reads an mm_cid of 2 or more.
 * Skipped with fewer than 4 CPUs.
 */
static int test_concurrency_rotation_cpuset_narrow(const char *root)
{
	struct rot *r = NULL;
	int ret = KSFT_FAIL, i;
	pid_t pid = -1;
	char *g;

	if (need_cpus(4))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_rot_cpuset");
	if (!g || cg_create(g) || cg_write(g, "cpuset.cpus", "1-3") ||
	    cg_write(g, "cpu.concurrency.max", "2"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	for (i = 0; i < 3; i++)
		rot_pinned(r, i, i + 1, i + 1);
	pid = rot_start(r, g, 0, 3);
	if (pid < 0 || rot_wait_ready(r, 0, 3))
		goto cleanup;
	rot_go(r, 60000);
	usleep(100000);
	if (rot_window(r, 0, 3, 1500, 10, 0, 208, "three pinned threads, cpuset 1-3"))
		goto cleanup;
	if (cg_write(g, "cpuset.cpus", "1")) {
		fprintf(stderr, "the cpuset of g narrowed to CPU 1: refused\n");
		goto cleanup;
	}
	if (wait_effective(g, "2 max\n", 2000)) {
		fprintf(stderr, "the effective value is not 2 with the cpuset 1\n");
		goto cleanup;
	}
	usleep(100000);
	if (rot_window(r, 0, 3, 1500, 10, 0, 104, "cpuset narrowed to CPU 1"))
		goto cleanup;
	if (cg_write(g, "cpuset.cpus", "1-3")) {
		fprintf(stderr, "the cpuset of g widened to 1-3: refused\n");
		goto cleanup;
	}
	if (wait_effective(g, "2 max\n", 2000)) {
		fprintf(stderr, "the effective value is not 2 with the cpuset 1-3 again\n");
		goto cleanup;
	}
	usleep(100000);
	if (rot_window(r, 0, 3, 1500, 10, 0, 208, "cpuset widened to 1-3 again"))
		goto cleanup;
	if (rot_check_ids(r, 0, 3, 2, "rotation_cpuset_narrow"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

/*
 * A pinned task may move into a cgroup whose slots are all in use.
 *
 * Cgroups: g and "out". g has cpu.concurrency.max 1. out has no limit.
 * Two processes of one thread each spin until the end of the test:
 *  - a in g, pinned to CPU 1;
 *  - b in out, pinned to CPU 2.
 * The steps, each followed by a measurement:
 *  1. Nothing, 500 ms: each runs on its own CPU.
 *  2. The test moves b into g, 2 s: a and b share one slot.
 *  3. The test moves b back to out, 500 ms.
 *
 * Checked:
 *  - both moves are accepted;
 *  - in steps 1 and 3 each task runs for 60% of the time or more;
 *  - in step 2 each task runs for 20% of the time or more. The fair
 *    share is 50%;
 *  - in step 2 both tasks together run for one CPU at most. rot_window()
 *    allows 4% more;
 *  - nr_rotations of g grows during step 2;
 *  - nr_held of g is 0 within 2 s of step 3.
 * Skipped with fewer than 3 CPUs.
 */
static int test_concurrency_rotation_migrate(const char *root)
{
	pid_t pid[2] = { -1, -1 };
	struct conc_stat before, after;
	char *g = NULL, *out = NULL;
	struct rot *r = NULL;
	int ret = KSFT_FAIL, i;

	if (need_cpus(3))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_rot_migrate");
	out = cg_name(root, "cc_test_rot_migrate_out");
	if (!g || !out || cg_create(g) || cg_create(out) || cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	rot_pinned(r, 0, 1, 1);
	rot_pinned(r, 1, 2, 2);
	pid[0] = rot_start(r, g, 0, 1);
	pid[1] = rot_start(r, out, 1, 1);
	if (pid[0] < 0 || pid[1] < 0 || rot_wait_ready(r, 0, 2))
		goto cleanup;
	rot_go(r, 60000);
	usleep(100000);
	if (rot_window(r, 0, 2, 500, 60, 0, 0, "b outside g"))
		goto cleanup;
	if (conc_stat_read(g, &before))
		goto cleanup;
	if (cg_enter(g, pid[1])) {
		fprintf(stderr, "b, pinned to CPU 2, cannot move into g, which runs a on CPU 1\n");
		goto cleanup;
	}
	if (rot_window(r, 0, 2, 2000, 20, 0, 104, "b inside g"))
		goto cleanup;
	if (conc_stat_read(g, &after) || after.nr_rotations == before.nr_rotations) {
		fprintf(stderr, "nr_rotations of g did not grow while a and b shared its slot\n");
		goto cleanup;
	}
	if (cg_enter(out, pid[1])) {
		fprintf(stderr, "b cannot move back out of g\n");
		goto cleanup;
	}
	usleep(100000);
	if (rot_window(r, 0, 2, 500, 60, 0, 0, "b outside g again"))
		goto cleanup;
	for (i = 0; i < 200; i++) {
		if (conc_stat_read(g, &after))
			goto cleanup;
		if (!after.nr_held)
			break;
		usleep(10000);
	}
	if (after.nr_held) {
		fprintf(stderr, "nr_held of g is %lld 2 s after b left\n", after.nr_held);
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, pid, 2);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	if (out)
		cg_destroy(out);
	free(g);
	free(out);
	return ret;
}

/* Return true if task @i of @r runs for half of the next 20 ms or more. */
static bool rot_runs(struct rot *r, int i)
{
	long before = rot_run_ms(r, i);

	usleep(20000);
	return rot_run_ms(r, i) - before >= 10;
}

/*
 * A CPU goes offline while its slot is in rotation.
 *
 * Cgroup: g, with cpu.concurrency.max 1. One process in g has three
 * threads. They pin themselves to CPU 1, to CPU 2 and to the last CPU,
 * c. They spin until the end of the test and share the one slot. The
 * steps:
 *  1. The test measures for 1.5 s.
 *  2. The test waits until the thread pinned to c runs, and takes c
 *     offline at once. That thread has no CPU left in its mask. The
 *     kernel must give it a wider mask, as it does without a limit. The
 *     test measures for 2 s.
 *  3. The test brings c back online, and measures for 2 s.
 *
 * Checked in each measurement:
 *  - each thread runs for 10% of the time or more. The fair share is
 *    33%;
 *  - together the threads run for one CPU at most. rot_window() allows
 *    4% more.
 * Checked at the end: no thread read an mm_cid of 1 or more, and
 * cpu.concurrency.effective reads 1.
 * Skipped with fewer than 4 CPUs, or when c cannot go offline.
 */
static int test_concurrency_rotation_hotplug(const char *root)
{
	int ret = KSFT_FAIL, c = get_nprocs() - 1, offline = 0, i;
	struct rot *r = NULL;
	pid_t pid = -1;
	char *g;

	if (need_cpus(4))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_rot_hotplug");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	rot_pinned(r, 0, 1, 1);
	rot_pinned(r, 1, 2, 2);
	rot_pinned(r, 2, c, c);
	pid = rot_start(r, g, 0, 3);
	if (pid < 0 || rot_wait_ready(r, 0, 3))
		goto cleanup;
	rot_go(r, 60000);
	usleep(100000);
	if (rot_window(r, 0, 3, 1500, 10, 0, 104, "before the unplug"))
		goto cleanup;
	/* Its turn comes every three rotation periods. */
	for (i = 0; i < 100 && !rot_runs(r, 2); i++)
		;
	if (i == 100) {
		fprintf(stderr, "the thread pinned to CPU %d did not run within 2 s\n", c);
		goto cleanup;
	}
	if (write_cpu_online(c, 0)) {
		ret = skip("CPU %d cannot go offline", c);
		goto cleanup;
	}
	offline = 1;
	if (rot_window(r, 0, 3, 2000, 10, 0, 104, "after the unplug"))
		goto cleanup;
	if (write_cpu_online(c, 1)) {
		fprintf(stderr, "CPU %d does not come back online\n", c);
		goto cleanup;
	}
	offline = 0;
	if (rot_window(r, 0, 3, 2000, 10, 0, 104, "after the replug"))
		goto cleanup;
	if (rot_check_ids(r, 0, 3, 1, "rotation_hotplug"))
		goto cleanup;
	if (cg_read_strcmp(g, "cpu.concurrency.effective", "1 max\n")) {
		fprintf(stderr, "the effective value is not 1 after the hotplug\n");
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (offline)
		write_cpu_online(c, 1);
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

/*
 * A limit goes up and down while its slots are in rotation.
 *
 * Cgroup: g, with cpu.concurrency.max 1. One process in g has four
 * threads, pinned to CPUs 1 to 4. They spin until the end of the test.
 * The steps, each followed by 1.5 s of measurement:
 *  1. Nothing: four threads share one slot.
 *  2. The test writes 2 to cpu.concurrency.max.
 *     cpu.concurrency.effective must read 2 when the write returns.
 *  3. The test writes 1. cpu.concurrency.effective must read 1 within
 *     2 s: a decrease shows once the tasks over the limit are held. The
 *     test then waits 100 ms and forgets the IDs read so far.
 *
 * Checked in each measurement, with the limit L of the step:
 *  - each thread runs for 5% of the time or more. The fair share is 25%
 *    with one slot and 50% with two;
 *  - together the threads run for L CPUs at most. rot_window() allows
 *    4% more;
 *  - in step 2 they run for 1.3 CPUs or more together: the second slot
 *    is used.
 * Checked on the IDs: no mm_cid of 2 or more until step 3, and none of 1
 * or more after it.
 * Skipped with fewer than 5 CPUs.
 */
static int test_concurrency_rotation_limit_change(const char *root)
{
	struct rot *r = NULL;
	int ret = KSFT_FAIL, i;
	pid_t pid = -1;
	char *g;

	if (need_cpus(5))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_rot_limit");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	for (i = 0; i < 4; i++)
		rot_pinned(r, i, i + 1, i + 1);
	pid = rot_start(r, g, 0, 4);
	if (pid < 0 || rot_wait_ready(r, 0, 4))
		goto cleanup;
	rot_go(r, 60000);
	usleep(100000);
	if (rot_window(r, 0, 4, 1500, 5, 0, 104, "under the limit 1"))
		goto cleanup;
	if (cg_write(g, "cpu.concurrency.max", "2") ||
	    cg_read_strcmp(g, "cpu.concurrency.effective", "2 max\n")) {
		fprintf(stderr, "the limit raised to 2: refused, or not effective at once\n");
		goto cleanup;
	}
	if (rot_window(r, 0, 4, 1500, 5, 130, 208, "under the limit 2"))
		goto cleanup;
	if (rot_check_ids(r, 0, 4, 2, "rotation_limit_change, limit 2"))
		goto cleanup;
	if (cg_write(g, "cpu.concurrency.max", "1") || wait_effective(g, "1 max\n", 2000)) {
		fprintf(stderr, "the limit lowered to 1: refused, or not effective within 2 s\n");
		goto cleanup;
	}
	usleep(100000);
	for (i = 0; i < 4; i++)
		__atomic_store_n(&r->t[i].max_cid, 0, __ATOMIC_RELAXED);
	if (rot_window(r, 0, 4, 1500, 5, 0, 104, "under the limit 1 again"))
		goto cleanup;
	if (rot_check_ids(r, 0, 4, 1, "rotation_limit_change, limit 1 again"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

#define ROT_LOWER_ROUNDS	20

/*
 * Forget the IDs which tasks 0 to @n - 1 of @r read so far and sleep for
 * 10 ms. Return true if a task read an mm_cid at or above @limit meanwhile.
 */
static bool rot_reads_over(struct rot *r, int n, unsigned int limit)
{
	bool over = false;
	int i;

	if (!rseq_has_mm_cid())
		return false;
	for (i = 0; i < n; i++)
		__atomic_store_n(&r->t[i].max_cid, 0, __ATOMIC_RELAXED);
	usleep(10000);
	for (i = 0; i < n; i++)
		over |= __atomic_load_n(&r->t[i].max_cid, __ATOMIC_RELAXED) >= limit;
	return over;
}

/*
 * A limit goes down under tasks which keep running in user space.
 *
 * Cgroup: g, with cpu.concurrency.max 2. One process in g has two
 * threads, pinned to CPU 1 and to CPU 2. They spin in user space until
 * the end of the test and never block.
 *
 * ROT_LOWER_ROUNDS rounds. In each one the test:
 *  - writes 2 and sleeps for 100 ms or more: both threads run;
 *  - writes 1 and waits until cpu.concurrency.effective reads 1;
 *  - forgets the IDs read so far and sleeps for 10 ms, twice.
 *
 * Checked in each round:
 *  - cpu.concurrency.effective reads 1 within 2 s. One thread has to be
 *    held, and it must not take its slot back while the kernel empties
 *    it;
 *  - no thread reads an mm_cid of 1 or more in both 10 ms sleeps. The
 *    thread which keeps its slot does not schedule: the kernel has to
 *    make it take a new ID. A thread can store once an ID which it read
 *    before the decrease. So one sleep with such an ID is accepted.
 *
 * The sleeps are shorter than a rotation period. A rotation would make
 * the running thread schedule and take a new ID.
 * Skipped with fewer than 3 CPUs.
 */
static int test_concurrency_rotation_lower_running(const char *root)
{
	struct rot *r = NULL;
	int ret = KSFT_FAIL, round, i;
	pid_t pid = -1;
	char *g;

	if (need_cpus(3))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_rot_lower");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "2"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	for (i = 0; i < 2; i++)
		rot_pinned(r, i, i + 1, i + 1);
	pid = rot_start(r, g, 0, 2);
	if (pid < 0 || rot_wait_ready(r, 0, 2))
		goto cleanup;
	rot_go(r, 60000);
	for (round = 0; round < ROT_LOWER_ROUNDS; round++) {
		if (cg_write(g, "cpu.concurrency.max", "2"))
			goto cleanup;
		usleep(100000 + round * 3000);
		if (cg_write(g, "cpu.concurrency.max", "1") || wait_effective(g, "1 max\n", 2000)) {
			fprintf(stderr, "round %d: the limit lowered to 1: refused, or not effective within 2 s\n",
				round);
			goto cleanup;
		}
		if (rot_reads_over(r, 2, 1) && rot_reads_over(r, 2, 1)) {
			fprintf(stderr, "round %d: a thread reads an mm_cid of 1 or more under the limit 1: %u and %u\n",
				round, r->t[0].max_cid, r->t[1].max_cid);
			goto cleanup;
		}
	}
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

#define ROT_KILL_ROUNDS	10

/*
 * SIGKILL ends a task which waits for a slot, and a task which runs in
 * one, also near the time its slot is taken.
 *
 * Cgroup: g, with cpu.concurrency.max 1. Three processes of one thread
 * each run in g, pinned to CPU 1, to CPU 2 and to CPU 3. They spin until
 * they are killed, and share the one slot. The test holds
 * cpu.concurrency.events of g open.
 *
 * First part. The test picks a process which did not run for 20 ms: it
 * waits for the slot. It sends it SIGKILL.
 *
 * Second part, ROT_KILL_ROUNDS rounds. In each one the test:
 *  - starts a new process in the place of the one killed before;
 *  - waits for a notification of cpu.concurrency.events: a rotation has
 *    just happened;
 *  - picks the process which runs now;
 *  - sends it SIGKILL one rotation period and round * 2.5 ms after the
 *    notification. The next rotation comes 1 to 1.25 periods after the
 *    last one.
 *
 * Checked after each SIGKILL:
 *  - the process is reaped within 1 s. The kernel lets a held task go as
 *    soon as its placement work runs, and nothing keeps it from running
 *    here;
 *  - during the next 300 ms, each of the two other processes runs for
 *    10% of the time or more;
 *  - together they run for one CPU at most during that time, with 4%
 *    more allowed as in rot_window().
 * Checked at the end: nr_held of g is 0 within 2 s of the last kill, and
 * main() finds no warning of the kernel.
 * Skipped with fewer than 4 CPUs.
 *
 * Delay injection: conc_hold_cpu() asks for the hold of the task which
 * runs in the slot. It then waits until that task is held at its return
 * to user space. SIGKILL has to arrive between the two. The window lasts
 * well under a millisecond, so a round reaches it only by chance. A delay
 * in conc_hold_cpu() after conc_hold_task_on() would open it.
 */
static int test_concurrency_rotation_kill(const char *root)
{
	int ret = KSFT_FAIL, fd = -1, i, round, victim, period = rot_period_ms(), next = 3;
	int live[3] = { 0, 1, 2 }, slot;
	pid_t pid[ROT_TASKS];
	struct conc_stat st;
	struct rot *r = NULL;
	char *g, path[PATH_MAX];

	for (i = 0; i < ROT_TASKS; i++)
		pid[i] = -1;
	if (need_cpus(4))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_rot_kill");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	snprintf(path, sizeof(path), "%s/cpu.concurrency.events", g);
	fd = open(path, O_RDONLY);
	r = rot_alloc();
	if (fd < 0 || !r)
		goto cleanup;
	for (i = 0; i < 3; i++) {
		rot_pinned(r, i, i + 1, i + 1);
		pid[i] = rot_start(r, g, i, 1);
		if (pid[i] < 0)
			goto cleanup;
	}
	if (rot_wait_ready(r, 0, 3))
		goto cleanup;
	rot_go(r, 120000);
	usleep(300000);

	for (round = -1; round < ROT_KILL_ROUNDS; round++) {
		long before[3], elapsed;
		int other[2], n = 0;
		long long t0;

		if (round >= 0 && file_notified(fd, 2000) != 1) {
			fprintf(stderr, "round %d: no rotation within 2 s\n", round);
			goto cleanup;
		}
		t0 = mono_ns();
		for (i = 0; i < 3; i++)
			before[i] = rot_run_ms(r, live[i]);
		usleep(20000);
		slot = -1;
		for (i = 0; i < 3; i++) {
			bool ran = rot_run_ms(r, live[i]) - before[i] >= 10;

			/* The first part kills a waiter, the rounds the runner. */
			if (ran == (round >= 0))
				slot = i;
		}
		if (slot < 0) {
			fprintf(stderr, "round %d: no task %s\n", round,
				round < 0 ? "waits" : "runs");
			goto cleanup;
		}
		victim = live[slot];
		if (round >= 0)
			sleep_until_ns(t0 + period * 1000000LL + round * 2500000LL);
		if (kill_within(pid[victim], 1000)) {
			fprintf(stderr, "round %d: a task which %s is not dead 1 s after SIGKILL\n",
				round, round < 0 ? "waited for the slot" : "ran in the slot");
			goto cleanup;
		}
		pid[victim] = -1;
		for (i = 0; i < 3; i++) {
			if (i != slot)
				other[n++] = live[i];
		}
		/* The two tasks left are not a range, as rot_window() wants. */
		t0 = mono_ns();
		for (i = 0; i < 2; i++)
			before[i] = rot_run_ms(r, other[i]);
		usleep(300000);
		elapsed = (mono_ns() - t0) / 1000000;
		for (i = 0; i < 2; i++)
			before[i] = rot_run_ms(r, other[i]) - before[i];
		if (before[0] * 10 < elapsed || before[1] * 10 < elapsed ||
		    (before[0] + before[1]) * 100 > elapsed * 104) {
			fprintf(stderr, "round %d: in %ld ms after the kill the two other tasks ran %ld and %ld ms\n",
				round, elapsed, before[0], before[1]);
			goto cleanup;
		}
		if (round == ROT_KILL_ROUNDS - 1)
			break;
		/* A new process on the CPU of the one just killed. */
		rot_pinned(r, next, slot + 1, slot + 1);
		pid[next] = rot_start(r, g, next, 1);
		if (pid[next] < 0 || rot_wait_ready(r, next, 1))
			goto cleanup;
		live[slot] = next++;
	}
	rot_end(r, pid, ROT_TASKS);
	for (i = 0; i < 200; i++) {
		if (conc_stat_read(g, &st))
			goto cleanup;
		if (!st.nr_held)
			break;
		usleep(10000);
	}
	if (st.nr_held) {
		fprintf(stderr, "nr_held of g is %lld 2 s after the last kill\n", st.nr_held);
		goto cleanup;
	}
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, pid, ROT_TASKS);
		munmap(r, sizeof(*r));
	}
	if (fd >= 0)
		close(fd);
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

/*
 * A task which gets a slot by rotation and then blocks gives the slot
 * back at once.
 *
 * Cgroup: g, with cpu.concurrency.max 1. One process in g has two threads,
 * which run for 3 s:
 *  - s spins, pinned to CPU 1;
 *  - w is pinned to CPU 2. It wakes up every 30 ms, spins for 5 ms and
 *    sleeps again.
 * w finds no slot at its wakeup and waits for a rotation. It then runs
 * for 5 ms and blocks. Its slot is idle from then on, and s is held.
 *
 * Checked:
 *  - s is never off the CPU for three quarters of a rotation period. A
 *    kernel which keeps the idle slot of w makes s wait for a period;
 *  - s runs for 70% of the time or more;
 *  - w wakes up 5 times or more, and never runs more than 2.5 periods
 *    late. It waits for one period, and the kernel looks for an expired
 *    period four times in a period;
 *  - nr_rotations of g grows;
 *  - no thread reads an mm_cid of 1 or more.
 * Skipped with fewer than 3 CPUs.
 */
static int test_concurrency_rotation_block(const char *root)
{
	int ret = KSFT_FAIL, stuck, i, late = 0, period = rot_period_ms();
	struct conc_stat before, after;
	struct rot *r = NULL;
	pid_t pid = -1;
	char *g;

	if (need_cpus(3))
		return KSFT_SKIP;
	g = cg_name(root, "cc_test_rot_block");
	if (!g || cg_create(g) || cg_write(g, "cpu.concurrency.max", "1"))
		goto cleanup;
	r = rot_alloc();
	if (!r)
		goto cleanup;
	rot_pinned(r, 0, 1, 1);
	rot_pinned(r, 1, 2, 2);
	r->t[1].kind = ROT_PERIODIC;
	r->t[1].period_us = 30000;
	r->t[1].work_us = 5000;
	pid = rot_start(r, g, 0, 2);
	if (pid < 0 || rot_wait_ready(r, 0, 2) || conc_stat_read(g, &before))
		goto cleanup;
	rot_go(r, 3000);
	stuck = rot_wait_done(r, 0, 2);
	if (stuck >= 0) {
		fprintf(stderr, "thread %d did not run for 3 s at the end\n", stuck);
		goto cleanup;
	}
	if (conc_stat_read(g, &after))
		goto cleanup;
	for (i = 0; i < r->t[1].nlat; i++)
		late = MAX(late, r->t[1].lat_us[i]);
	ksft_print_msg("the spinner ran %d%%, longest time off the CPU %lld ms; the waiter woke up %d times, %d ms late at most; %lld rotations\n",
		       rot_share(r, 0), r->t[0].max_gap_ns / 1000000, r->t[1].nlat, late / 1000,
		       after.nr_rotations - before.nr_rotations);
	if (r->t[0].max_gap_ns >= period * 750000LL || rot_share(r, 0) < 70) {
		fprintf(stderr, "the spinner did not get the slot back when the waiter blocked\n");
		goto cleanup;
	}
	if (r->t[1].nlat < 5 || late >= period * 2500) {
		fprintf(stderr, "the waiter woke up %d times, %d ms late at most\n", r->t[1].nlat,
			late / 1000);
		goto cleanup;
	}
	if (after.nr_rotations == before.nr_rotations) {
		fprintf(stderr, "nr_rotations did not grow\n");
		goto cleanup;
	}
	if (rot_check_ids(r, 0, 2, 1, "rotation_block"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (g)
		cg_destroy(g);
	free(g);
	return ret;
}

#define DL_JOBS		20

/* The deadline task of test_concurrency_rotation_deadline(). */
struct dl_job {
	int go;		/* the test: start the jobs */
	int jobs;	/* jobs ended */
	int late;	/* jobs which took more than a period */
};

static long long thread_cpu_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * Wait until told. Then run DL_JOBS jobs, one in each period of the
 * SCHED_DEADLINE policy which the test gave this task. A job uses 15 ms
 * of CPU time. sched_yield() then sleeps until the next period. Count a
 * job which took more than 100 ms.
 */
static int dl_job_fn(const char *cgroup, void *arg)
{
	struct dl_job *j = arg;
	long long start, cpu;
	int k;

	while (!__atomic_load_n(&j->go, __ATOMIC_ACQUIRE))
		usleep(1000);
	for (k = 0; k < DL_JOBS; k++) {
		start = mono_ns();
		cpu = thread_cpu_ns();
		while (thread_cpu_ns() - cpu < 15000000LL)
			;
		if (mono_ns() - start > 100000000LL)
			j->late++;
		__atomic_store_n(&j->jobs, k + 1, __ATOMIC_RELEASE);
		sched_yield();
	}
	for (;;)
		pause();
	return 0;
}

/*
 * A deadline task keeps its runtime beside pinned fair tasks which ask
 * for more CPUs than the limit.
 *
 * Cgroup: P, a partition root on the last three CPUs, with
 * cpu.concurrency.max 2. Tasks in P:
 *  - d, one process. The test makes it SCHED_DEADLINE with a runtime of
 *    20 ms in a period of 100 ms. It can run on the three CPUs;
 *  - one process with three fair threads, each pinned to one CPU of P.
 *    They spin for 3 s and share the two slots.
 * When the fair threads start to spin, d starts DL_JOBS jobs, see
 * dl_job_fn(). A job needs 15 ms of CPU time in its period.
 *
 * Checked:
 *  - d is admitted, and the three pins are accepted;
 *  - d ends its DL_JOBS jobs within 5 s, and 2 at most take more than a
 *    period of 100 ms. A deadline task which waits for the slot of a
 *    fair task for a rotation period ends each job late;
 *  - each fair thread runs for 10% of the time or more. The fair share
 *    is about 60%;
 *  - no fair thread reads an mm_cid of 2 or more.
 * Skipped:
 *  - with fewer than 4 CPUs;
 *  - with deadline admission off;
 *  - without a partition root;
 *  - when P takes no deadline task before it has a limit.
 */
static int test_concurrency_rotation_deadline(const char *root)
{
	struct sched_param other = { .sched_priority = 0 };
	int ret = KSFT_FAIL, L = get_nprocs() - 1, i, err, stuck;
	char *P = NULL, rt[32], cpus[32];
	struct rot *r = NULL;
	struct dl_job *j;
	pid_t d = -1, pid = -1;

	if (need_cpus(4))
		return KSFT_SKIP;
	if (rt_runtime_read(rt, sizeof(rt)) || atol(rt) < 0)
		return skip("deadline admission control is off");
	j = mmap(NULL, sizeof(*j), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (j == MAP_FAILED)
		return KSFT_FAIL;
	P = cg_name(root, "cc_test_rot_dl");
	if (!P || cg_create(P))
		goto cleanup;
	snprintf(cpus, sizeof(cpus), "%d-%d", L - 2, L);
	if (cg_write(P, "cpuset.cpus", cpus) || cg_write(P, "cpuset.cpus.partition", "root") ||
	    cg_read_strcmp(P, "cpuset.cpus.partition", "root\n")) {
		ret = skip("no partition root on CPUs %s", cpus);
		goto cleanup;
	}
	err = dl_admitted_in(P);
	if (err) {
		ret = skip("no SCHED_DEADLINE task in a partition root without a limit: %s",
			   strerror(-err));
		goto cleanup;
	}
	if (cg_write(P, "cpu.concurrency.max", "2"))
		goto cleanup;
	d = cg_run_nowait(P, dl_job_fn, j);
	if (d < 0 || wait_in_cgroup(P, d))
		goto cleanup;
	err = set_deadline(d, 20000, 100000);
	if (err) {
		fprintf(stderr, "a deadline task of 0.2 CPU refused in a deadline domain: %s\n",
			strerror(-err));
		goto cleanup;
	}
	r = rot_alloc();
	if (!r)
		goto cleanup;
	for (i = 0; i < 3; i++)
		rot_pinned(r, i, L - 2 + i, L - 2 + i);
	pid = rot_start(r, P, 0, 3);
	if (pid < 0 || rot_wait_ready(r, 0, 3))
		goto cleanup;
	rot_go(r, 3000);
	usleep(50000);
	__atomic_store_n(&j->go, 1, __ATOMIC_RELEASE);
	for (i = 0; i < 500 && __atomic_load_n(&j->jobs, __ATOMIC_ACQUIRE) < DL_JOBS; i++)
		usleep(10000);
	stuck = rot_wait_done(r, 0, 3);
	ksft_print_msg("the deadline task ended %d jobs, %d late; the fair threads ran %d%%, %d%% and %d%%\n",
		       j->jobs, j->late, rot_share(r, 0), rot_share(r, 1), rot_share(r, 2));
	if (j->jobs < DL_JOBS || j->late > 2) {
		fprintf(stderr, "the deadline task did not get its runtime in each period\n");
		goto cleanup;
	}
	if (stuck >= 0 || rot_share(r, 0) < 10 || rot_share(r, 1) < 10 || rot_share(r, 2) < 10) {
		fprintf(stderr, "a fair thread got less than 10%% of the time, or did not end\n");
		goto cleanup;
	}
	if (rot_check_ids(r, 0, 3, 2, "rotation_deadline"))
		goto cleanup;
	ret = KSFT_PASS;
cleanup:
	if (r) {
		rot_end(r, &pid, 1);
		munmap(r, sizeof(*r));
	}
	if (d > 0) {
		kill(d, SIGKILL);
		sched_setscheduler(d, SCHED_OTHER, &other);
		waitpid(d, NULL, 0);
	}
	if (P) {
		cg_write(P, "cpu.concurrency.max", "max");
		cg_write(P, "cpuset.cpus.partition", "member");
		cg_destroy(P);
	}
	free(P);
	munmap(j, sizeof(*j));
	return ret;
}

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
	T(test_concurrency_hotplug),
	T(test_concurrency_split),
	T(test_concurrency_hard),
	T(test_concurrency_sleepers),
	T(test_concurrency_cpuset_class),
	T(test_concurrency_ns_cid),
	T(test_concurrency_ns_cid_moves),
	T(test_concurrency_mm_cid_moves),
	T(test_concurrency_ns_home_unshare),
	T(test_concurrency_ns_home_clone),
	T(test_concurrency_ns_no_home),
	T(test_concurrency_ns_ceiling_only),
	T(test_concurrency_ns_keeps_home),
	T(test_concurrency_ns_fd_keeps_home),
	T(test_concurrency_dormant),
	T(test_concurrency_deadline),
	T(test_concurrency_deadline_unadmitted),
	T(test_concurrency_deadline_domain),
	T(test_concurrency_isolated),
	T(test_concurrency_late_child),
	T(test_concurrency_child_slot),
	T(test_concurrency_partition_rmdir),
	T(test_concurrency_child_hard_enable),
	T(test_concurrency_fork_affinity),
	T(test_concurrency_offline_affinity),
	T(test_concurrency_setmove_mask),
	T(test_concurrency_headroom_setmove),
	T(test_concurrency_cid_decrease),
	T(test_concurrency_effective_ids),
	T(test_concurrency_ids_unique),
	T(test_concurrency_limit_raise_ids),
	T(test_concurrency_ids_complete_after_rounds),
	T(test_concurrency_ns_join_widen),
	T(test_concurrency_clone_into),
	T(test_concurrency_self_move),
	T(test_concurrency_vhost),
	T(test_concurrency_toggle),
	T(test_concurrency_smt_spread),
	T(test_concurrency_dl_setaffinity_in_domain),
	T(test_concurrency_offline_pinned_running),
	T(test_concurrency_bandwidth_under_limit),
	T(test_concurrency_rt_setmove),
	T(test_concurrency_held_kill),
	T(test_concurrency_held_offline),
	T(test_concurrency_rotation_share),
	T(test_concurrency_rotation_order),
	T(test_concurrency_rotation_fits),
	T(test_concurrency_rotation_rt),
	T(test_concurrency_rotation_foreign_rt),
	T(test_concurrency_rotation_stat),
	T(test_concurrency_rotation_period),
	T(test_concurrency_rotation_overlap),
	T(test_concurrency_rotation_nested),
	T(test_concurrency_rotation_cpuset_narrow),
	T(test_concurrency_rotation_migrate),
	T(test_concurrency_rotation_hotplug),
	T(test_concurrency_rotation_limit_change),
	T(test_concurrency_rotation_lower_running),
	T(test_concurrency_rotation_kill),
	T(test_concurrency_rotation_block),
	T(test_concurrency_rotation_deadline),
};

#undef T

/*
 * Run the case @name alone and return its result: this is the execution
 * which own_rseq_run() started. The first execution has set up the root
 * cgroup. It also checks the kernel log and the online CPUs once the case
 * ends.
 */
static int own_rseq_main(const char *name)
{
	char root[PATH_MAX];
	int i;

	own_rseq_child = 1;
	if (cg_find_unified_root(root, sizeof(root), NULL))
		return KSFT_FAIL;
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		if (!strcmp(tests[i].name, name))
			return tests[i].fn(root);
	}
	return KSFT_FAIL;
}

int main(int argc, char *argv[])
{
	struct kernel_state ks;
	char root[PATH_MAX];
	int i, r, plan = 0;
	char *probe;

	if (argc == 3 && !strcmp(argv[1], OWN_RSEQ_ARG))
		return own_rseq_main(argv[2]);
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
