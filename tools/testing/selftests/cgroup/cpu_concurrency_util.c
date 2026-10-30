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

/*
 * The ns_cid field lies past the 32 bytes glibc registers for a thread:
 * unregister glibc's area and register a larger one, sized and aligned
 * as the auxiliary vector says, unless glibc's is large enough already.
 * Returns the area holding ns_cid and its registered length, or NULL
 * when the kernel has no such field. glibc falls back to the getcpu
 * syscall for sched_getcpu() once its area is unregistered.
 */
struct rseq *rseq_ext_register(size_t *lenp)
{
	unsigned long feat = getauxval(AT_RSEQ_FEATURE_SIZE);
	unsigned long align = getauxval(AT_RSEQ_ALIGN);
	size_t len, try[3];
	struct rseq *area;
	int i;

	if (feat < RSEQ_NS_CID_OFFSET + sizeof(__u32) || !align)
		return NULL;
	if (__rseq_size >= RSEQ_NS_CID_OFFSET + sizeof(__u32)) {
		*lenp = 0;
		return thread_rseq();
	}
	len = (feat + align - 1) & ~(align - 1);
	/* glibc registered with one of these lengths, if at all. */
	try[0] = __rseq_size;
	try[1] = 32;
	try[2] = len;
	for (i = 0; __rseq_size && i < 3; i++) {
		if (!syscall(__NR_rseq, thread_rseq(), (unsigned int)try[i],
			     RSEQ_FLAG_UNREGISTER, RSEQ_SIG))
			break;
	}
	if (__rseq_size && i == 3)
		return NULL;
	area = aligned_alloc(align, len);
	if (!area)
		return NULL;
	memset(area, 0, len);
	if (syscall(__NR_rseq, area, (unsigned int)len, 0, RSEQ_SIG)) {
		free(area);
		return NULL;
	}
	*lenp = len;
	return area;
}

void rseq_ext_unregister(struct rseq *area, size_t len)
{
	if (!len)
		return;
	syscall(__NR_rseq, area, (unsigned int)len, RSEQ_FLAG_UNREGISTER, RSEQ_SIG);
	free(area);
}

/* A test that does not apply says why, as kselftest asks. */
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

/* KSFT_SKIP, with the reason, when fewer than @n CPUs are online; else 0. */
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

long us_since(const struct timespec *from)
{
	return ns_since(from) / 1000;
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
 * Every CPU the mask can name, so that "all" restricts nothing whatever
 * the number of online CPUs is at the time, which moves under hotplug.
 */
void stress_all_cpus(cpu_set_t *set)
{
	int i;

	CPU_ZERO(set);
	for (i = 0; i < CPU_SETSIZE; i++)
		CPU_SET(i, set);
}

/* Pin @pid to the CPUs of @cpus, a list ended by -1, or to every CPU: 0 or an errno. */
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
 * Runs inside the cgroup: spin on nthreads threads and check that the
 * process never used more than limit CPUs and never saw an mm_cid at or
 * above the limit.
 */
int spin_and_check(const char *cgroup, void *arg)
{
	struct spin_param *p = arg;
	struct spinner s[MAX_THREADS] = { };
	struct timespec until;
	int i, seen, go = 0;
	long long max_cid;

	/*
	 * The CPUs seen are counted over the whole run, which holds the
	 * limit only while no thread leaves: a slot left idle is drained
	 * and the next arrival may take a new CPU. This thread shares the
	 * group's CPUs with the spinners and, with many CPUs, creates the
	 * last ones after the first ones have reached a deadline set
	 * before: start the clock once they all spin.
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

/* Take CPU @cpu online or offline through sysfs: 0, or -1 without it. */
int write_cpu_online(int cpu, int online)
{
	char path[PATH_MAX];
	char buf[2] = { online ? '1' : '0', '\n' };

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu);
	return write_text(path, buf, 2) == 2 ? 0 : -1;
}
