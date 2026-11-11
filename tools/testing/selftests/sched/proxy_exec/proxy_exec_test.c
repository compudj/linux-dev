// SPDX-License-Identifier: GPL-2.0
/*
 * Tests of proxy execution (CONFIG_SCHED_PROXY_EXEC).
 *
 * With proxy execution, a task blocked on a mutex stays on its runqueue
 * and lends its scheduling context to the mutex owner. These cases block
 * tasks on the mutexes of the test_proxy_exec module, and check what the
 * waiter and the owner get.
 *
 * Every case must also pass without proxy execution (sched_proxy_exec=0).
 *
 * The deadlock_cycle case makes two tasks deadlock on two kernel mutexes,
 * then kills them. A kernel without the cycle check locks up. So the case
 * runs only with PROXY_EXEC_TEST_DEADLOCK=1.
 *
 * Needs root, CONFIG_TEST_PROXY_EXEC and debugfs. The cases with a
 * deadline task also need cgroup v2 with the cpuset controller.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../kselftest.h"

#define OP_FILE		"/sys/kernel/debug/test_proxy_exec/op"
#define SCHED_DEBUG	"/sys/kernel/debug/sched/debug"
#define CGROUP		"/sys/fs/cgroup"
#define CG_A		CGROUP "/proxy_exec_test_a"
#define CG_B		CGROUP "/proxy_exec_test_b"
#define AUTOGROUP	"/proc/sys/kernel/sched_autogroup_enabled"
#define MS		1000000ULL
/*
 * A waiter that runs this long after it could have is late. In testing,
 * a waiter ran less than 1ms late with the fixes, and 28ms or more
 * without them.
 */
#define LATE_US		10000

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE	6
#endif

/* Exit codes of a case, and of its children. */
#define CASE_PASS	0
#define CASE_FAIL	1
#define CASE_SKIP	KSFT_SKIP

struct dl_attr {
	uint32_t size;
	uint32_t sched_policy;
	uint64_t sched_flags;
	int32_t sched_nice;
	uint32_t sched_priority;
	uint64_t sched_runtime;
	uint64_t sched_deadline;
	uint64_t sched_period;
};

/* Shared between the processes of a case. */
struct shared {
	uint64_t t_unlock;	/* owner: after its hold returned */
	uint64_t t_got;	/* waiter: after its take returned */
	uint64_t beat;		/* heartbeat counter */
	int ret;
};

static struct shared *sh;

/* The children of the running case, and what must be undone after it. */
static pid_t kids[8];
static int nr_kids;
static int in_child;
static int child_failed;
static int autogroup_old = -1;

static int write_file(const char *path, const char *val);

/* Kill and reap every child of the case that is still there. */
static void stop_children(void)
{
	int i;

	for (i = 0; i < nr_kids; i++) {
		if (!kids[i])
			continue;
		kill(kids[i], SIGKILL);
		waitpid(kids[i], NULL, 0);
		kids[i] = 0;
	}
}

/* Undo what the case changed in the system. Run by the case process. */
static void cleanup(void)
{
	stop_children();
	if (autogroup_old >= 0)
		write_file(AUTOGROUP, autogroup_old ? "1" : "0");
	autogroup_old = -1;
}

static uint64_t now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Skip the case: it cannot do the setup step @what on this system. */
static void skip(const char *what)
{
	ksft_print_msg("cannot %s: %s\n", what, strerror(errno));
	if (!in_child)
		cleanup();
	exit(CASE_SKIP);
}

/* Sleep until @ms milliseconds after @t0. */
static void sleep_until(uint64_t t0, unsigned int ms)
{
	uint64_t t = t0 + ms * MS;
	struct timespec ts = { t / 1000000000ULL, t % 1000000000ULL };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

static void spin_until(uint64_t t)
{
	while (now() < t)
		;
}

/* Pin @pid on @cpu0, and on @cpu1 too unless it is negative. */
static void pin(pid_t pid, int cpu0, int cpu1)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu0, &set);
	if (cpu1 >= 0)
		CPU_SET(cpu1, &set);
	if (sched_setaffinity(pid, sizeof(set), &set))
		skip("sched_setaffinity");
}

static void set_fifo(int prio)
{
	struct sched_param sp = { .sched_priority = prio };

	if (sched_setscheduler(0, SCHED_FIFO, &sp))
		skip("SCHED_FIFO");
}

static int set_deadline(pid_t pid, unsigned int runtime_ms, unsigned int period_ms)
{
	struct dl_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_DEADLINE,
		.sched_runtime = runtime_ms * MS,
		.sched_deadline = period_ms * MS,
		.sched_period = period_ms * MS,
	};

	return syscall(SYS_sched_setattr, pid, &attr, 0);
}

/* Send @cmd to the test module. Return 0, or a negative errno. */
static int op(const char *cmd)
{
	int fd = open(OP_FILE, O_WRONLY), ret;

	if (fd < 0)
		skip("open " OP_FILE);
	ret = write(fd, cmd, strlen(cmd));
	ret = ret < 0 ? -errno : 0;
	close(fd);
	return ret;
}

/* Send @cmd to the test module. A child fails if the command does. */
static void op_ok(const char *cmd)
{
	int ret = op(cmd);

	if (ret) {
		ksft_print_msg("%s: %s\n", cmd, strerror(-ret));
		exit(CASE_FAIL);
	}
}

static int write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY), ret;

	if (fd < 0)
		return -1;
	ret = write(fd, val, strlen(val));
	close(fd);
	return ret < 0 ? -1 : 0;
}

/* Move the calling process into cgroup @cg. */
static void join(const char *cg)
{
	char path[128], pid[32];

	snprintf(path, sizeof(path), "%s/cgroup.procs", cg);
	snprintf(pid, sizeof(pid), "%d", getpid());
	if (write_file(path, pid))
		skip("move into the cgroup");
}

/* Make cgroup @cg a cpuset partition root on @cpu. Skip if it cannot be. */
static void partition(const char *cg, const char *cpu)
{
	char path[128];

	write_file(CGROUP "/cgroup.subtree_control", "+cpuset");
	if (mkdir(cg, 0755) && errno != EEXIST)
		skip("create the cgroup");
	snprintf(path, sizeof(path), "%s/cpuset.cpus", cg);
	if (write_file(path, cpu))
		skip("set the CPUs of the cgroup");
	snprintf(path, sizeof(path), "%s/cpuset.cpus.partition", cg);
	if (write_file(path, "root"))
		skip("make the cgroup a partition root");
	/* Let the root domains be rebuilt. */
	usleep(200000);
}

/*
 * Fork a child that dies with the case. Return its pid to the parent and
 * 0 to the child. The case fails if it cannot fork.
 */
static pid_t child(void)
{
	pid_t pid;

	fflush(stdout);
	pid = fork();
	if (pid < 0 || nr_kids == ARRAY_SIZE(kids)) {
		ksft_print_msg("fork: %s\n", strerror(errno));
		cleanup();
		exit(CASE_FAIL);
	}
	if (!pid) {
		in_child = 1;
		prctl(PR_SET_PDEATHSIG, SIGKILL);
	} else {
		kids[nr_kids++] = pid;
	}
	return pid;
}

/*
 * Wait for @pid to exit. Skip the case if the child could not do its
 * setup. Note a child that failed or was killed: the case then fails.
 */
static void reap(pid_t pid)
{
	int status, i;

	if (waitpid(pid, &status, 0) != pid)
		return;
	for (i = 0; i < nr_kids; i++)
		if (kids[i] == pid)
			kids[i] = 0;
	if (WIFEXITED(status) && WEXITSTATUS(status) == CASE_SKIP) {
		cleanup();
		exit(CASE_SKIP);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		ksft_print_msg("a child of the case failed (status 0x%x)\n", status);
		child_failed = 1;
	}
}

/*
 * Return CASE_PASS if no child failed and the waiter ran less than
 * LATE_US after @t_ref. The waiter can record its time a little before
 * the other task records @t_ref.
 */
static int waiter_on_time(uint64_t t_ref, const char *what)
{
	long long late = (long long)(sh->t_got - t_ref) / 1000;

	if (child_failed || !sh->t_got || !t_ref) {
		ksft_print_msg("the case did not complete\n");
		return CASE_FAIL;
	}
	ksft_print_msg("the waiter ran %lld us after %s (limit %d)\n",
		       late, what, LATE_US);
	return late > -LATE_US && late < LATE_US ? CASE_PASS : CASE_FAIL;
}

/*
 * One CPU. The owner (SCHED_OTHER) holds the mutex 300ms, then spins 1s
 * in user space. The waiter (FIFO 50, or deadline 400ms/1s) blocks on
 * the mutex at 100ms. It must run when the owner unlocks.
 *
 * The owner runs on the waiter's reservation while it holds the mutex.
 * So the deadline waiter has a runtime above the 200ms it waits: it must
 * not be throttled when the owner unlocks.
 *
 * Without "sched/proxy: Reschedule when a blocked task is woken under a
 * proxy", the owner keeps the waiter's scheduling context after the
 * unlock. A FIFO waiter then waits for the owner to block. A deadline
 * waiter waits for its next period.
 */
static int handoff(int deadline)
{
	uint64_t t0;
	pid_t o, w;

	/* A deadline task cannot be pinned: use a one-CPU partition. */
	if (deadline)
		partition(CG_A, "0");
	t0 = now();
	o = child();
	if (!o) {
		if (deadline)
			join(CG_A);
		else
			pin(0, 0, -1);
		op_ok("hold 0 300");
		sh->t_unlock = now();
		spin_until(sh->t_unlock + 1000 * MS);
		exit(0);
	}
	w = child();
	if (!w) {
		if (deadline) {
			join(CG_A);
			if (set_deadline(0, 400, 1000))
				skip("SCHED_DEADLINE");
		} else {
			pin(0, 0, -1);
			set_fifo(50);
		}
		sleep_until(t0, 100);
		op_ok("take 0");
		sh->t_got = now();
		exit(0);
	}
	reap(w);
	reap(o);
	return waiter_on_time(sh->t_unlock, "the unlock");
}

static int handoff_fifo(void)
{
	return handoff(0);
}

static int handoff_deadline(void)
{
	return handoff(1);
}

static void sig_handler(int sig)
{
}

/*
 * Two CPUs. The owner is SCHED_OTHER, the waiter is FIFO 50. Both are on
 * CPU 0.
 *
 *   start  the owner takes the mutex, and holds it 1500ms
 *   100ms  the waiter blocks on it, interruptibly
 *   300ms  the parent, on CPU 1, signals the waiter
 *
 * The signal must make the waiter return.
 *
 * Same fix as the hand-off cases. The busy-wait of the owner has
 * scheduling points, so without the fix the waiter runs at the next one.
 * In testing that was 28ms to 85ms after the signal.
 */
static int signal_fifo(void)
{
	uint64_t t0 = now(), t_signal;
	pid_t o, w;

	o = child();
	if (!o) {
		pin(0, 0, -1);
		op_ok("hold 0 1500");
		exit(0);
	}
	w = child();
	if (!w) {
		struct sigaction sa = { .sa_handler = sig_handler };

		sigaction(SIGUSR1, &sa, NULL);
		pin(0, 0, -1);
		set_fifo(50);
		sleep_until(t0, 100);
		sh->ret = op("take_intr 0");
		sh->t_got = now();
		exit(0);
	}
	pin(0, 1, -1);
	set_fifo(99);
	sleep_until(t0, 300);
	t_signal = now();
	kill(w, SIGUSR1);
	reap(w);
	reap(o);
	ksft_print_msg("the waiter returned %d (expect %d)\n", sh->ret, -EINTR);
	if (sh->ret != -EINTR)
		return CASE_FAIL;
	return waiter_on_time(t_signal, "the signal");
}

/*
 * Two CPUs. The owner is SCHED_OTHER. The waiter is FIFO 50, allowed on
 * CPUs 0-1.
 *
 *   start  the owner takes the mutex on CPU 0
 *   100ms  the waiter blocks on it on CPU 0
 *   200ms  a FIFO 90 task starts to spin on CPU 0, until 1000ms
 *   250ms  the parent moves the owner to CPU 1, so that it unlocks there
 *   500ms  the owner unlocks and exits: CPU 1 is idle
 *
 * The waiter must run on CPU 1 then.
 *
 * Without "sched/proxy: Wake blocked RT and deadline tasks through the
 * full path", the waiter is woken in place on CPU 0. It is not pushable,
 * and waits for the FIFO 90 task.
 */
static int pushable_fifo(void)
{
	uint64_t t0 = now();
	pid_t o, w, h;

	o = child();
	if (!o) {
		pin(0, 0, -1);
		op_ok("hold 0 500");
		sh->t_unlock = now();
		exit(0);
	}
	w = child();
	if (!w) {
		pin(0, 0, -1);
		set_fifo(50);
		sleep_until(t0, 100);
		pin(0, 0, 1);
		op_ok("take 0");
		sh->t_got = now();
		exit(0);
	}
	h = child();
	if (!h) {
		pin(0, 0, -1);
		set_fifo(90);
		sleep_until(t0, 200);
		spin_until(t0 + 1000 * MS);
		exit(0);
	}
	pin(0, 1, -1);
	set_fifo(99);
	sleep_until(t0, 250);
	pin(o, 1, -1);
	reap(w);
	reap(o);
	reap(h);
	return waiter_on_time(sh->t_unlock, "the unlock");
}

/*
 * Read @field of the root cfs_rq of @cpu from the sched debug file. Its
 * header is "cfs_rq[N]:/" with CONFIG_FAIR_GROUP_SCHED, "cfs_rq[N]:"
 * without. Return -1 if the field is not there.
 */
static long long cfs_rq_field(int cpu, const char *field)
{
	char line[256], hdr[32], hdr_grp[32];
	long long v = -1;
	int in = 0;
	FILE *f = fopen(SCHED_DEBUG, "r");

	if (!f)
		skip("open " SCHED_DEBUG);
	snprintf(hdr, sizeof(hdr), "cfs_rq[%d]:\n", cpu);
	snprintf(hdr_grp, sizeof(hdr_grp), "cfs_rq[%d]:/\n", cpu);
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "cfs_rq[", 7) || !strncmp(line, "rt_rq[", 6)) {
			in = !strcmp(line, hdr) || !strcmp(line, hdr_grp);
		} else if (in) {
			char *s = strstr(line, field);

			/* ".util_avg" must not match ".util_est". */
			if (s && s[-1] == '.' && s[strlen(field)] == ' ') {
				v = atoll(strchr(line, ':') + 1);
				break;
			}
		}
	}
	fclose(f);
	return v;
}

/*
 * Three CPUs. The owner (SCHED_OTHER, CPU 1) never sleeps. It holds the
 * mutex 15ms and spins 5ms in user space, in a loop. A second task spins
 * on CPU 1, so the owner is preempted about half the time. A waiter spins
 * instead of blocking while the owner runs, so this is needed. The waiter
 * (nice -10, CPU 0) spins 5ms and takes the mutex, in a loop. Proxy
 * execution moves it to CPU 1 and back each time it blocks. With two
 * tasks that never sleep, util_avg of the root cfs_rq of CPU 1 stays
 * near 1024.
 *
 * Without "sched/proxy: Tell the class about a proxy migration", each
 * return of the waiter removes load that CPU 1 never got.
 */
static int pelt_migration(void)
{
	long long u, umin = 1 << 30, usum = 0;
	uint64_t t0 = now();
	int i, n = 40;
	pid_t o, w, x;
	FILE *f;

	/* The tasks must be in the root cfs_rq: no autogroup. */
	f = fopen(AUTOGROUP, "r");
	if (f) {
		if (fscanf(f, "%d", &autogroup_old) != 1)
			autogroup_old = -1;
		fclose(f);
		write_file(AUTOGROUP, "0");
	}
	if (cfs_rq_field(1, "util_avg") < 0) {
		errno = ENOENT;
		skip("read util_avg of CPU 1 in " SCHED_DEBUG);
	}
	o = child();
	if (!o) {
		pin(0, 1, -1);
		for (;;) {
			op_ok("hold 0 15");
			spin_until(now() + 5 * MS);
		}
	}
	x = child();
	if (!x) {
		pin(0, 1, -1);
		for (;;)
			;
	}
	w = child();
	if (!w) {
		pin(0, 0, -1);
		errno = 0;
		if (nice(-10) == -1 && errno)
			skip("nice");
		for (;;) {
			spin_until(now() + 5 * MS);
			op_ok("take 0");
		}
	}
	pin(0, 2, -1);
	set_fifo(50);
	sleep_until(t0, 1000);
	for (i = 0; i < n; i++) {
		u = cfs_rq_field(1, "util_avg");
		umin = u < umin ? u : umin;
		usum += u;
		sleep_until(t0, 1000 + (i + 1) * 25);
	}
	cleanup();
	ksft_print_msg("util_avg of CPU 1 over %d samples: mean %lld, min %lld (mean limit 850)\n",
		       n, usum / n, umin);
	return usum / n >= 850 ? CASE_PASS : CASE_FAIL;
}

/* Read .dl_bw->total_bw of @cpu's root domain from the sched debug file. */
static long long total_bw(int cpu)
{
	char line[256], hdr[32];
	long long v = -1;
	int in = 0;
	FILE *f = fopen(SCHED_DEBUG, "r");

	if (!f)
		skip("open " SCHED_DEBUG);
	snprintf(hdr, sizeof(hdr), "dl_rq[%d]:", cpu);
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "dl_rq[", 6)) {
			in = !strncmp(line, hdr, strlen(hdr));
		} else if (in && strstr(line, "dl_bw->total_bw")) {
			v = atoll(strchr(line, ':') + 1);
			break;
		}
	}
	fclose(f);
	return v;
}

/* Return the CPU that /proc/<pid>/stat reports for @pid. */
static int task_cpu_of(pid_t pid)
{
	char path[64], buf[1024], *s;
	int cpu = -1, i;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fgets(buf, sizeof(buf), f)) {
		/* Field 39, counted after the command name. */
		s = strrchr(buf, ')');
		for (i = 2; s && i < 39; i++)
			s = strchr(s + 1, ' ');
		if (s)
			cpu = atoi(s + 1);
	}
	fclose(f);
	return cpu;
}

/*
 * Three CPUs. Cpusets A (CPU 0) and B (CPU 1) are partition roots. The
 * owner is SCHED_OTHER, in B. The waiter is in A.
 *
 *   start  the owner takes the mutex, and holds it 600ms
 *    50ms  a FIFO 90 task of B starts to spin, until 450ms. The owner is
 *          then preempted when the waiter comes.
 *   100ms  the waiter blocks on the mutex. Proxy execution queues it on
 *          CPU 1.
 *   300ms  the parent calls sched_setattr() on the waiter
 *
 * With @admit, the waiter is SCHED_OTHER and becomes a deadline task of
 * 10ms/100ms. Root domain A must admit it. Without @admit, the waiter is
 * that deadline task already. Its runtime changes to 20ms, then it leaves
 * the class. In both cases the total_bw of B must not move, and both
 * root domains must end where they started.
 *
 * Without "sched/deadline: Use the root domain a proxy-migrated task runs
 * in", sched_setattr() fails with EPERM.
 */
static int dl_moved(int admit)
{
	long long a0, b0, a1, b1, a2, b2, bw = (10LL << 20) / 100;
	struct sched_param sp = { 0 };
	int r, e, cpu, ok;
	pid_t o, w, h;
	uint64_t t0;

	partition(CG_A, "0");
	partition(CG_B, "1");
	pin(0, 2, -1);
	a0 = total_bw(0);
	b0 = total_bw(1);
	t0 = now();
	o = child();
	if (!o) {
		join(CG_B);
		op_ok("hold 0 600");
		exit(0);
	}
	h = child();
	if (!h) {
		join(CG_B);
		set_fifo(90);
		sleep_until(t0, 50);
		spin_until(t0 + 450 * MS);
		exit(0);
	}
	w = child();
	if (!w) {
		join(CG_A);
		if (!admit && set_deadline(0, 10, 100))
			skip("SCHED_DEADLINE");
		sleep_until(t0, 100);
		op_ok("take 0");
		exit(0);
	}
	sleep_until(t0, 300);
	cpu = task_cpu_of(w);
	r = set_deadline(w, admit ? 10 : 20, 100);
	e = errno;
	a1 = total_bw(0);
	b1 = total_bw(1);
	if (!admit && sched_setscheduler(w, SCHED_OTHER, &sp))
		ksft_print_msg("sched_setscheduler(SCHED_OTHER): %s\n", strerror(errno));
	reap(w);
	reap(o);
	reap(h);
	sleep(1);
	a2 = total_bw(0);
	b2 = total_bw(1);
	ksft_print_msg("the waiter of cpuset A (CPU 0) was queued on CPU %d\n", cpu);
	ksft_print_msg("sched_setattr(): %s\n", r ? strerror(e) : "ok");
	ksft_print_msg("total_bw A: before %lld, at 300ms %lld, after %lld\n", a0, a1, a2);
	ksft_print_msg("total_bw B: before %lld, at 300ms %lld, after %lld\n", b0, b1, b2);
	ok = !child_failed && !r && b1 == b0 && a2 == a0 && b2 == b0;
	if (admit)
		ok = ok && a1 - a0 >= bw - 2 && a1 - a0 <= bw + 2;
	return ok ? CASE_PASS : CASE_FAIL;
}

static int dl_setattr_moved(void)
{
	return dl_moved(0);
}

static int dl_admit_moved(void)
{
	return dl_moved(1);
}

/*
 * Two CPUs. Two tasks (SCHED_OTHER, CPU 0) take two mutexes in opposite
 * orders and deadlock. A FIFO 98 heartbeat on CPU 0 ticks every 10ms. It
 * must still tick 3s later. The inner lock is killable, so the case kills
 * the two tasks at the end.
 *
 * Without "sched/proxy: Stop the chain walk when the blocked_on chain is
 * a cycle", CPU 0 walks the cycle forever with its rq lock held.
 */
static int deadlock_cycle(void)
{
	uint64_t t0 = now(), b0, b1;
	pid_t t1, t2, hb;

	t1 = child();
	if (!t1) {
		pin(0, 0, -1);
		op("pair 0 1 200");
		exit(0);
	}
	t2 = child();
	if (!t2) {
		pin(0, 0, -1);
		sleep_until(t0, 50);
		op("pair 1 0 200");
		exit(0);
	}
	hb = child();
	if (!hb) {
		pin(0, 0, -1);
		set_fifo(98);
		for (;;) {
			sh->beat++;
			usleep(10000);
		}
	}
	pin(0, 1, -1);
	set_fifo(99);
	sleep_until(t0, 1000);
	b0 = sh->beat;
	sleep_until(t0, 4000);
	b1 = sh->beat;
	ksft_print_msg("the heartbeat of CPU 0 ticked %lld times in 3s (expect about 300)\n",
		       (long long)(b1 - b0));
	cleanup();
	return b1 - b0 > 100 ? CASE_PASS : CASE_FAIL;
}

static const struct {
	const char *name;
	int (*fn)(void);
	int cpus;	/* online CPUs 0..cpus-1 needed */
	int cpuset;	/* needs a cpuset partition */
	int deadlock;	/* needs PROXY_EXEC_TEST_DEADLOCK=1 */
} cases[] = {
	{ "handoff_fifo",	handoff_fifo,		1, 0, 0 },
	{ "handoff_deadline",	handoff_deadline,	2, 1, 0 },
	{ "signal_fifo",	signal_fifo,		2, 0, 0 },
	{ "pushable_fifo",	pushable_fifo,		2, 0, 0 },
	{ "pelt_migration",	pelt_migration,		3, 0, 0 },
	{ "dl_setattr_moved",	dl_setattr_moved,	3, 1, 0 },
	{ "dl_admit_moved",	dl_admit_moved,		3, 1, 0 },
	{ "deadlock_cycle",	deadlock_cycle,		2, 0, 1 },
};

/* Return the number of CPUs 0..n-1 that the test may run on. */
static int usable_cpus(void)
{
	cpu_set_t set;
	int n = 0;

	if (sched_getaffinity(0, sizeof(set), &set))
		return 0;
	while (n < CPU_SETSIZE && CPU_ISSET(n, &set))
		n++;
	return n;
}

/*
 * Remove cgroup @cg. The children of a case that was killed can still be
 * in it for a moment, so retry for a second.
 */
static void remove_cgroup(const char *cg)
{
	int i;

	for (i = 0; i < 20; i++) {
		if (!rmdir(cg) || errno == ENOENT)
			return;
		usleep(50000);
	}
	ksft_print_msg("cannot remove %s: %s\n", cg, strerror(errno));
}

/* Run case @i in its own process. Return its exit code. */
static int run_case(unsigned int i)
{
	int status;
	pid_t pid;

	memset(sh, 0, sizeof(*sh));
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		return CASE_SKIP;
	if (!pid) {
		alarm(60);
		exit(cases[i].fn());
	}
	if (waitpid(pid, &status, 0) != pid)
		status = -1;
	if (cases[i].cpuset) {
		remove_cgroup(CG_A);
		remove_cgroup(CG_B);
	}
	if (status == -1)
		return CASE_FAIL;
	if (!WIFEXITED(status)) {
		ksft_print_msg("the case was killed by signal %d\n", WTERMSIG(status));
		return CASE_FAIL;
	}
	return WEXITSTATUS(status);
}

int main(void)
{
	int cpus = usable_cpus(), deadlock, cpuset, had_cpuset = 1, ret;
	const char *env = getenv("PROXY_EXEC_TEST_DEADLOCK");
	char buf[256] = "";
	unsigned int i;
	FILE *f;

	ksft_print_header();
	if (geteuid())
		ksft_exit_skip("needs root\n");
	if (access(OP_FILE, W_OK)) {
		if (system("modprobe test_proxy_exec 2>/dev/null") || access(OP_FILE, W_OK))
			ksft_exit_skip("needs CONFIG_TEST_PROXY_EXEC and debugfs (%s)\n", OP_FILE);
	}
	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		ksft_exit_skip("mmap: %s\n", strerror(errno));
	deadlock = env && !strcmp(env, "1");
	cpuset = !access(CGROUP "/cgroup.subtree_control", W_OK);
	/* partition() enables the cpuset controller: note if it was off. */
	f = fopen(CGROUP "/cgroup.subtree_control", "r");
	if (f) {
		if (fgets(buf, sizeof(buf), f))
			had_cpuset = !!strstr(buf, "cpuset");
		else
			had_cpuset = 0;
		fclose(f);
	}

	ksft_set_plan(ARRAY_SIZE(cases));
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		if (cases[i].cpus > cpus) {
			ksft_test_result_skip("%s: needs CPUs 0-%d\n", cases[i].name,
					      cases[i].cpus - 1);
			continue;
		}
		if (cases[i].cpuset && !cpuset) {
			ksft_test_result_skip("%s: needs cgroup v2 on %s\n", cases[i].name,
					      CGROUP);
			continue;
		}
		if (cases[i].deadlock && !deadlock) {
			ksft_test_result_skip("%s: needs PROXY_EXEC_TEST_DEADLOCK=1\n",
					      cases[i].name);
			continue;
		}
		ret = run_case(i);
		if (ret == CASE_SKIP)
			ksft_test_result_skip("%s: see the message above\n", cases[i].name);
		else
			ksft_test_result(ret == CASE_PASS, "%s\n", cases[i].name);
	}
	if (cpuset && !had_cpuset)
		write_file(CGROUP "/cgroup.subtree_control", "-cpuset");
	ksft_finished();
}
