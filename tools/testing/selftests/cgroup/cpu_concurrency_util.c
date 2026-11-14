// SPDX-License-Identifier: GPL-2.0
/*
 * Helpers of the tests of cpu.concurrency.max, see
 * Documentation/scheduler/sched-max-concurrency.rst
 */

#include "cpu_concurrency_util.h"

struct rseq *thread_rseq(void)
{
	return (struct rseq *)((char *)__builtin_thread_pointer() + __rseq_offset);
}

int rseq_has_mm_cid(void)
{
	return __rseq_size >= RSEQ_MM_CID_OFFSET + sizeof(__u32);
}

__u32 rseq_field(const struct rseq *r, size_t offset)
{
	return *(volatile const __u32 *)((const char *)r + offset);
}

/* Print the reason why a test does not apply. Return KSFT_SKIP. */
int skip(const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	ksft_print_msg("%s\n", msg);
	return KSFT_SKIP;
}

/*
 * Return 0 when at least @n CPUs are online. Else print the reason and
 * return KSFT_SKIP.
 */
int need_cpus(int n)
{
	return get_nprocs() < n ? skip("%d CPUs needed, %d online", n, get_nprocs()) : 0;
}

long read_sysctl(const char *name)
{
	char path[PATH_MAX], buf[32];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "/proc/sys/kernel/%s", name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return atol(buf);
}

int ts_before(const struct timespec *a, const struct timespec *b)
{
	return a->tv_sec < b->tv_sec ||
	       (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

void ts_add_ms(struct timespec *t, long ms)
{
	t->tv_sec += ms / 1000;
	t->tv_nsec += (ms % 1000) * 1000000L;
	if (t->tv_nsec >= 1000000000L) {
		t->tv_sec++;
		t->tv_nsec -= 1000000000L;
	}
}

long long ns_since(const struct timespec *from)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - from->tv_sec) * 1000000000LL + (now.tv_nsec - from->tv_nsec);
}

/* Spin for @ns nanoseconds. */
void spin_ns(long long ns)
{
	struct timespec t0;

	if (ns <= 0)
		return;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (ns_since(&t0) < ns)
		;
}

/*
 * Spin until the deadline, recording every CPU the thread runs on and the
 * largest mm_cid it is given.
 */
void *spin(void *arg)
{
	struct spinner *s = arg;
	struct timespec now;
	int has_cid = rseq_has_mm_cid();

	__atomic_store_n(&s->tid, gettid(), __ATOMIC_RELEASE);
	/* Spinning, not sleeping: the group's slots stay busy meanwhile. */
	if (s->go)
		while (!__atomic_load_n(s->go, __ATOMIC_ACQUIRE))
			;
	if (s->first_nap_us)
		usleep(s->first_nap_us);
	for (;;) {
		int cpu = sched_getcpu();

		if (cpu >= 0 && cpu < CPU_SETSIZE)
			s->cpus_seen[cpu / 64] |= 1UL << (cpu % 64);
		if (cpu + 1 == __atomic_load_n(&s->avoid, __ATOMIC_RELAXED))
			__atomic_fetch_add(&s->avoid_hits, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&s->samples, 1, __ATOMIC_RELAXED);
		if (has_cid) {
			/* Unsigned: an ID left unset reads 0x80000000. */
			__u32 cid = *(volatile __u32 *)((char *)thread_rseq() +
							RSEQ_MM_CID_OFFSET);

			if (cid > s->max_mm_cid)
				s->max_mm_cid = cid;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (!ts_before(&now, &s->until))
			break;
		if (s->nap_us)
			usleep(s->nap_us);
	}
	return NULL;
}

/*
 * Set every CPU which a cpu_set_t can name in @set. The mask then
 * restricts nothing, whatever CPUs hotplug brings online later.
 */
void stress_all_cpus(cpu_set_t *set)
{
	int i;

	CPU_ZERO(set);
	for (i = 0; i < CPU_SETSIZE; i++)
		CPU_SET(i, set);
}

/*
 * Pin @pid to the CPUs listed in @cpus. The list ends with -1. Pin it to
 * every CPU when @cpus is NULL. Return 0 or an errno.
 */
int pin_cpus(pid_t pid, const int *cpus)
{
	cpu_set_t set;

	if (!cpus) {
		stress_all_cpus(&set);
	} else {
		CPU_ZERO(&set);
		for (; *cpus >= 0; cpus++)
			CPU_SET(*cpus, &set);
	}
	return sched_setaffinity(pid, sizeof(set), &set) ? errno : 0;
}

int count_cpus_seen(struct spinner *s, int n)
{
	unsigned long all[(CPU_SETSIZE + 63) / 64] = { };
	int i, j, count = 0;

	for (i = 0; i < n; i++)
		for (j = 0; j < (CPU_SETSIZE + 63) / 64; j++)
			all[j] |= s[i].cpus_seen[j];
	for (j = 0; j < (CPU_SETSIZE + 63) / 64; j++)
		count += __builtin_popcountl(all[j]);
	return count;
}

/* Return true if every sample of @s was taken on @cpu. */
int spinner_only_on(const struct spinner *s, int cpu)
{
	int j;

	for (j = 0; j < (CPU_SETSIZE + 63) / 64; j++) {
		if (s->cpus_seen[j] != (j == cpu / 64 ? 1UL << (cpu % 64) : 0))
			return 0;
	}
	return 1;
}

long long max_mm_cid_seen(struct spinner *s, int n)
{
	long long max = -1;
	int i;

	for (i = 0; i < n; i++)
		if (s[i].max_mm_cid > max)
			max = s[i].max_mm_cid;
	return max;
}

/*
 * Run inside the cgroup. Start nthreads spinners for duration_ms. Return
 * non-zero if the process used more than limit CPUs, or if a thread read
 * an mm_cid at or above limit. @arg is a struct spin_param.
 */
int spin_and_check(const char *cgroup, void *arg)
{
	struct spin_param *p = arg;
	struct spinner s[MAX_THREADS] = { };
	struct timespec until;
	int i, seen, go = 0;
	long long max_cid;

	/*
	 * The check counts the CPUs used over the whole run. This count
	 * stays within the limit only while every spinner runs. When one
	 * ends, its slot can be removed, and the cgroup can then use
	 * another CPU. This thread shares the CPUs of the cgroup with the
	 * spinners. With many CPUs, a deadline set before the first
	 * pthread_create() would end the first spinners before the last
	 * ones are created. So set the deadline once they all spin.
	 */
	for (i = 0; i < p->nthreads; i++) {
		s[i].max_mm_cid = -1;
		s[i].go = &go;
		if (pthread_create(&s[i].thread, NULL, spin, &s[i]))
			return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &until);
	ts_add_ms(&until, p->duration_ms);
	for (i = 0; i < p->nthreads; i++)
		s[i].until = until;
	__atomic_store_n(&go, 1, __ATOMIC_RELEASE);
	for (i = 0; i < p->nthreads; i++)
		pthread_join(s[i].thread, NULL);

	seen = count_cpus_seen(s, p->nthreads);
	max_cid = max_mm_cid_seen(s, p->nthreads);
	if (seen > p->limit) {
		fprintf(stderr, "%d threads ran on %d CPUs, limit %d\n",
			p->nthreads, seen, p->limit);
		return 2;
	}
	if (rseq_has_mm_cid() && max_cid >= p->limit) {
		fprintf(stderr, "mm_cid %lld seen, limit %d\n", max_cid, p->limit);
		return 3;
	}
	return 0;
}

/*
 * Set CPU @cpu online or offline through sysfs. Return 0, or -1 when the
 * write fails.
 */
int write_cpu_online(int cpu, int online)
{
	char path[PATH_MAX];
	char buf[2] = { online ? '1' : '0', '\n' };

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu);
	return write_text(path, buf, 2) == 2 ? 0 : -1;
}

/*
 * cpus_record_online() records the CPUs online when the tests start.
 * cpus_restore() brings them back online. It runs after each test and
 * at exit. It also runs on SIGTERM, SIGINT and SIGHUP. A test which
 * fails or is killed then does not leave a CPU offline for the tests
 * after it. Only
 * the process which recorded the CPUs restores them. A forked child
 * inherits the handlers, and its exit must not undo the hotplug of its
 * parent.
 */
static cpu_set_t cpus_online_at_start;
static int cpus_nconf;
static pid_t cpus_owner;

/*
 * Write the sysfs path of @cpu's online file into @path. Use no stdio:
 * the signal handler calls this too.
 */
static void cpu_online_path(char *path, int cpu)
{
	static const char pre[] = "/sys/devices/system/cpu/cpu", post[] = "/online";
	char digits[12];
	int n = 0, i = sizeof(pre) - 1;

	do {
		digits[n++] = '0' + cpu % 10;
		cpu /= 10;
	} while (cpu);
	memcpy(path, pre, sizeof(pre) - 1);
	while (n)
		path[i++] = digits[--n];
	memcpy(path + i, post, sizeof(post));
}

/*
 * Return true when @cpu is online. A CPU without the sysfs file, often
 * the boot CPU, cannot go offline: it counts as online.
 */
static bool cpu_is_online(int cpu)
{
	char path[64], c = '1';
	int fd;

	cpu_online_path(path, cpu);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return true;
	if (read(fd, &c, 1) != 1)
		c = '1';
	close(fd);
	return c == '1';
}

/*
 * Bring back online the CPUs which were online at the start. Return the
 * number of CPUs which could not be brought back.
 */
int cpus_restore(void)
{
	int cpu, failed = 0;

	if (getpid() != cpus_owner)
		return 0;
	for (cpu = 0; cpu < cpus_nconf; cpu++) {
		char path[64];
		int fd;

		if (!CPU_ISSET(cpu, &cpus_online_at_start) || cpu_is_online(cpu))
			continue;
		cpu_online_path(path, cpu);
		fd = open(path, O_WRONLY);
		if (fd < 0 || write(fd, "1\n", 2) != 2)
			failed++;
		if (fd >= 0)
			close(fd);
	}
	return failed;
}

static void cpus_restore_at_exit(void)
{
	cpus_restore();
}

static void cpus_restore_on_signal(int sig)
{
	cpus_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

void cpus_record_online(void)
{
	int cpu;

	cpus_owner = getpid();
	cpus_nconf = MIN(get_nprocs_conf(), CPU_SETSIZE);
	CPU_ZERO(&cpus_online_at_start);
	for (cpu = 0; cpu < cpus_nconf; cpu++) {
		if (cpu_is_online(cpu))
			CPU_SET(cpu, &cpus_online_at_start);
	}
	atexit(cpus_restore_at_exit);
	signal(SIGTERM, cpus_restore_on_signal);
	signal(SIGINT, cpus_restore_on_signal);
	signal(SIGHUP, cpus_restore_on_signal);
}

/*
 * Return debug_locks from /proc/lockdep_stats, or -1 on a kernel without
 * lockdep. Lockdep sets it to 0 with its first report and then checks
 * nothing more. A lockdep report does not taint the kernel.
 */
static int lockdep_checking(void)
{
	char line[128];
	int v = -1;
	FILE *f;

	f = fopen("/proc/lockdep_stats", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, " debug_locks: %d", &v) == 1)
			break;
	}
	fclose(f);
	return v;
}

/* Record the state of the kernel before a case. */
void kernel_state_record(struct kernel_state *ks)
{
	ks->taint = read_sysctl("tainted");
	ks->lockdep = lockdep_checking();
	ks->kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (ks->kmsg >= 0)
		lseek(ks->kmsg, 0, SEEK_END);
}

/*
 * Return the number of problems which the kernel reported since
 * kernel_state_record(), and print each one:
 *  - a new taint flag;
 *  - a lockdep report;
 *  - a line of the kernel log with "WARNING:" or "BUG:". The taint flag W
 *    is set by the first warning only. A WARN_ONCE() after it shows in the
 *    log alone.
 */
int kernel_state_check(struct kernel_state *ks, const char *name)
{
	long taint = read_sysctl("tainted");
	int lockdep = lockdep_checking(), bad = 0;
	char rec[1024];
	ssize_t n;

	if (ks->taint >= 0 && taint >= 0 && (taint & ~ks->taint)) {
		ksft_print_msg("%s: the kernel got the taint flags 0x%lx\n", name,
			       taint & ~ks->taint);
		bad++;
	}
	if (ks->lockdep == 1 && lockdep == 0) {
		ksft_print_msg("%s: lockdep reported a problem\n", name);
		bad++;
	}
	if (ks->kmsg < 0)
		return bad;
	for (;;) {
		char *text;

		n = read(ks->kmsg, rec, sizeof(rec) - 1);
		/* EPIPE: records were overwritten, the next read continues. */
		if (n < 0 && errno == EPIPE)
			continue;
		if (n <= 0)
			break;
		rec[n] = 0;
		text = strchr(rec, ';');
		if (!text || (!strstr(text, "WARNING:") && !strstr(text, "BUG:")))
			continue;
		text[strcspn(text, "\n")] = 0;
		ksft_print_msg("%s: kernel log: %s\n", name, text + 1);
		bad++;
	}
	close(ks->kmsg);
	return bad;
}

/*
 * Read cpu.concurrency.stat of @cg into @st. Return 0. Return -1 if the
 * file cannot be read or lacks one of the five keys. Other lines are
 * ignored.
 */
int conc_stat_read(const char *cg, struct conc_stat *st)
{
	static const char * const keys[] = { "nr_held", "nr_holds", "held_usec", "nr_rotations",
					     "rotated_usec" };
	long long *val[] = { &st->nr_held, &st->nr_holds, &st->held_usec, &st->nr_rotations,
			     &st->rotated_usec };
	char buf[1024], *line, *save;
	unsigned int i, found = 0;

	if (cg_read(cg, "cpu.concurrency.stat", buf, sizeof(buf)))
		return -1;
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		for (i = 0; i < ARRAY_SIZE(keys); i++) {
			size_t n = strlen(keys[i]);

			if (strncmp(line, keys[i], n) || line[n] != ' ')
				continue;
			*val[i] = atoll(line + n + 1);
			found |= 1U << i;
		}
	}
	return found == (1U << ARRAY_SIZE(keys)) - 1 ? 0 : -1;
}

/*
 * Return the count of the key "rotation" of cpu.concurrency.events of
 * @cg, or -1 if it cannot be read.
 */
long long conc_events_rotation(const char *cg)
{
	char buf[128];

	if (cg_read(cg, "cpu.concurrency.events", buf, sizeof(buf)) ||
	    strncmp(buf, "rotation ", 9))
		return -1;
	return atoll(buf + 9);
}

/*
 * The directory of tracefs while the events of the limit are enabled, or
 * an empty string. See conc_trace_start().
 */
static char conc_trace_dir[64];

static int conc_trace_write(const char *file, int flags, const char *val)
{
	char path[128];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", conc_trace_dir, file);
	fd = open(path, O_WRONLY | flags);
	if (fd < 0)
		return -1;
	n = write(fd, val, strlen(val));
	close(fd);
	return n == (ssize_t)strlen(val) ? 0 : -1;
}

/*
 * Enable the events sched_conc_refuse and sched_conc_rotate. Mount tracefs
 * at /sys/kernel/tracing if it is not mounted. Without tracefs or without
 * the events, print a note: conc_trace_find() then checks nothing.
 */
void conc_trace_start(void)
{
	static const char * const dirs[] = { "/sys/kernel/tracing", "/sys/kernel/debug/tracing" };
	int i;

	mount("tracefs", dirs[0], "tracefs", 0, NULL);
	for (i = 0; i < 2; i++) {
		snprintf(conc_trace_dir, sizeof(conc_trace_dir), "%s", dirs[i]);
		if (!conc_trace_write("events/sched/sched_conc_refuse/enable", 0, "1") &&
		    !conc_trace_write("events/sched/sched_conc_rotate/enable", 0, "1") &&
		    !conc_trace_write("tracing_on", 0, "1"))
			return;
	}
	conc_trace_dir[0] = 0;
	ksft_print_msg("no tracefs: the events of the limit are not checked\n");
}

void conc_trace_stop(void)
{
	if (!conc_trace_dir[0])
		return;
	conc_trace_write("events/sched/sched_conc_refuse/enable", 0, "0");
	conc_trace_write("events/sched/sched_conc_rotate/enable", 0, "0");
	conc_trace_dir[0] = 0;
}

/* Empty the trace buffer. */
void conc_trace_clear(void)
{
	if (conc_trace_dir[0])
		conc_trace_write("trace", O_TRUNC, "\n");
}

/*
 * Return 0 if a line of the trace buffer holds @text and @text2, or if the
 * events are not enabled. Else print the buffer and return -1.
 */
int conc_trace_find(const char *text, const char *text2)
{
	char path[128], line[512];
	int found = 0;
	FILE *f;

	if (!conc_trace_dir[0])
		return 0;
	snprintf(path, sizeof(path), "%s/trace", conc_trace_dir);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (!found && fgets(line, sizeof(line), f))
		found = strstr(line, text) && strstr(line, text2);
	if (!found) {
		fprintf(stderr, "no event with \"%s\" and \"%s\". The trace buffer:\n", text,
			text2);
		rewind(f);
		while (fgets(line, sizeof(line), f)) {
			if (line[0] != '#')
				fprintf(stderr, "  %s", line);
		}
	}
	fclose(f);
	return found ? 0 : -1;
}
