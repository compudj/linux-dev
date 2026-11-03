// SPDX-License-Identifier: GPL-2.0
/*
 * SCHED_DEADLINE bandwidth through priority inheritance, parameter changes,
 * cpuset moves, deaths, yields, runtime overruns, bandwidth reclaiming,
 * changes to the fair servers and CPU hotplug.
 *
 * Each case measures the free bandwidth of a root domain before and after
 * it runs: the number of tasks of 10ms every 100ms that root domain admits
 * (probe()). Once every task of the case is gone and its 0-lag time is
 * past, the two must be equal: fewer is bandwidth leaked, more is
 * bandwidth released twice, which lets the root domain admit deadline
 * tasks beyond its capacity. A deadline task a case keeps running must
 * also make progress.
 *
 * The reclaiming cases measure instead the share of a CPU a deadline task
 * with SCHED_FLAG_RECLAIM gets, which tells of the free bandwidth its CPU
 * keeps of the root domain (extra_bw), where probe() tells of the root
 * domain's own (total_bw).
 *
 * Every case but the hotplug ones uses P, a partition root of one CPU,
 * the last one, made anew for each case: the rebuild of the accounting
 * that makes it clears what an earlier case left. The hotplug cases run in
 * the root domain of all CPUs. The server cases change the fair server of a
 * CPU through debugfs, and the test puts it back as it found it. Needs
 * root, deadline admission control, and for the cases that use them, the
 * cgroup v2 cpuset controller, debugfs and CPU hotplug.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>

#include "../kselftest.h"

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE	6
#endif
#ifndef SCHED_FLAG_RECLAIM
#define SCHED_FLAG_RECLAIM	0x02
#endif
#ifndef SCHED_FLAG_DL_OVERRUN
#define SCHED_FLAG_DL_OVERRUN	0x04
#endif
#ifndef SCHED_FLAG_KEEP_PARAMS
#define SCHED_FLAG_KEEP_PARAMS	0x10
#endif

#define NSEC_PER_USEC	1000ULL
#define NSEC_PER_MSEC	1000000ULL
#define NSEC_PER_SEC	1000000000ULL

/* The threads of probe() and hold() do nothing: a small stack will do. */
#define THREAD_STACK	(64 * 1024 > PTHREAD_STACK_MIN ? 64 * 1024 : PTHREAD_STACK_MIN)

#define PROBE_RUNTIME	(10 * NSEC_PER_MSEC)
#define PROBE_PERIOD	(100 * NSEC_PER_MSEC)
#define PERIOD		(100 * NSEC_PER_MSEC)

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

static char root[PATH_MAX], cg_p[PATH_MAX], cg_q[PATH_MAX];
static int ncpus;

/* Both clocks are always supported and the pointer is valid: no error. */
static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static uint64_t thread_time_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void sleep_ns(uint64_t ns)
{
	struct timespec ts = { .tv_sec = ns / NSEC_PER_SEC, .tv_nsec = ns % NSEC_PER_SEC };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static void spin_ns(uint64_t ns)
{
	uint64_t end = thread_time_ns() + ns;

	while (thread_time_ns() < end)
		;
}

/*
 * Polls @cond every @step, at most @ms: 0 once it holds, -1 on a timeout.
 * Every wait here is bounded, so that a child that never started, or was
 * refused SCHED_DEADLINE, fails its case instead of hanging it.
 */
#define wait_for_step(ms, step, cond)					\
({									\
	uint64_t wait_end = now_ns() + (uint64_t)(ms) * NSEC_PER_MSEC;	\
	int wait_ret = -1;						\
									\
	while (1) {							\
		if (cond) {						\
			wait_ret = 0;					\
			break;						\
		}							\
		if (now_ns() >= wait_end)				\
			break;						\
		sleep_ns(step);						\
	}								\
	wait_ret;							\
})

#define wait_for(ms, cond)	wait_for_step(ms, NSEC_PER_MSEC, cond)

/* sem_wait(), retrying the signals that interrupt it. */
static void sem_wait_sig(sem_t *s)
{
	while (sem_wait(s)) {
		if (errno == EINTR)
			continue;
		ksft_print_msg("sem_wait: %s\n", strerror(errno));
		return;
	}
}

/* The same, at most @ms: 0, or -1. For what a thread or a child must post. */
static int sem_wait_for(sem_t *s, int ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	ts.tv_sec += ms / 1000 + ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;
	while (sem_timedwait(s, &ts)) {
		if (errno == EINTR)
			continue;
		if (errno != ETIMEDOUT)
			ksft_print_msg("sem_timedwait: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

/* fork(), which tells of its own failure: the caller handles -1. */
static pid_t fork_child(const char *what)
{
	pid_t pid = fork();

	if (pid < 0)
		ksft_print_msg("fork for %s: %s\n", what, strerror(errno));
	return pid;
}

static void reap(pid_t pid)
{
	int status;

	if (pid <= 0)
		return;
	if (kill(pid, SIGKILL) && errno != ESRCH)
		ksft_print_msg("kill %d: %s\n", pid, strerror(errno));
	while (waitpid(pid, &status, 0) != pid) {
		if (errno == EINTR)
			continue;
		ksft_print_msg("waitpid %d: %s\n", pid, strerror(errno));
		break;
	}
}

static void join(pthread_t t)
{
	int err = pthread_join(t, NULL);

	if (err)
		ksft_print_msg("pthread_join: %s\n", strerror(err));
}

static pid_t gettid_(void)
{
	return syscall(SYS_gettid);
}

static int set_deadline_flags(pid_t tid, uint64_t runtime, uint64_t period,
			      uint64_t flags)
{
	struct dl_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_DEADLINE,
		.sched_flags = flags,
		.sched_runtime = runtime,
		.sched_deadline = period,
		.sched_period = period,
	};

	return syscall(SYS_sched_setattr, tid, &attr, 0) ? -errno : 0;
}

static int set_deadline(pid_t tid, uint64_t runtime, uint64_t period)
{
	return set_deadline_flags(tid, runtime, period, 0);
}

static int set_other(pid_t tid)
{
	struct dl_attr attr = { .size = sizeof(attr), .sched_policy = SCHED_OTHER };

	return syscall(SYS_sched_setattr, tid, &attr, 0) ? -errno : 0;
}

static int write_file(const char *dir, const char *name, const char *val)
{
	char path[PATH_MAX];
	int fd, err = 0;
	ssize_t n;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;
	n = write(fd, val, strlen(val));
	if (n < 0)
		err = -errno;
	else if (n != (ssize_t)strlen(val))
		err = -EIO;
	if (close(fd) && !err)
		err = -errno;
	return err;
}

static int read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	do {
		n = read(fd, buf, len - 1);
	} while (n < 0 && errno == EINTR);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return 0;
}

static int enter(const char *cg, pid_t pid)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%d", pid);
	return write_file(cg, "cgroup.procs", buf);
}

/* @cg, a partition root of the CPUs @cpus names: 0, or -1. */
static int make_partition_cpus(const char *cg, const char *cpus)
{
	char path[PATH_MAX + 32], buf[32];

	if (mkdir(cg, 0755) && errno != EEXIST)
		return -1;
	if (write_file(cg, "cpuset.cpus", cpus) ||
	    write_file(cg, "cpuset.cpus.partition", "root"))
		return -1;
	snprintf(path, sizeof(path), "%s/cpuset.cpus.partition", cg);
	if (read_file(path, buf, sizeof(buf)) || strcmp(buf, "root\n"))
		return -1;
	return 0;
}

/* @cg, a partition root of CPU @cpu: 0, or -1. */
static int make_partition(const char *cg, int cpu)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%d", cpu);
	return make_partition_cpus(cg, buf);
}

static void remove_partition(const char *cg)
{
	int err = write_file(cg, "cpuset.cpus.partition", "member");

	if (err && err != -ENOENT)
		ksft_print_msg("%s back to a member: %s\n", cg, strerror(-err));
	/* A cgroup whose last task just died takes a moment to empty. */
	if (wait_for(1000, !rmdir(cg) || errno == ENOENT))
		ksft_print_msg("rmdir %s: %s\n", cg, strerror(errno));
}

/* P, a partition root of the last CPU: 0, or -1. */
static int make_p(void)
{
	return make_partition(cg_p, ncpus - 1);
}

static void remove_p(void)
{
	remove_partition(cg_p);
}

/* The CPU @pid last ran on, from /proc. */
static int task_cpu_of(pid_t pid)
{
	char path[64], buf[1024], *p;
	int i;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	if (read_file(path, buf, sizeof(buf)))
		return -1;
	p = strrchr(buf, ')');
	for (i = 0; p && i < 37; i++)
		p = strchr(p + 1, ' ');
	return p ? atoi(p + 1) : -1;
}

/* The CPU time @tid has used. */
static uint64_t cpu_time_of(pthread_t t)
{
	static int told;
	struct timespec ts;
	clockid_t c;

	if (pthread_getcpuclockid(t, &c) || clock_gettime(c, &ts)) {
		if (!told++)
			ksft_print_msg("the CPU time of a thread cannot be read: %s\n",
				       strerror(errno));
		return 0;
	}
	return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

/* The share of a CPU, in percent, that @t uses over @ns, a period later. */
static int share_of(pthread_t t, uint64_t ns)
{
	uint64_t cpu, wall;

	sleep_ns(PERIOD);		/* a period of its own to settle in */
	cpu = cpu_time_of(t);
	wall = now_ns();
	sleep_ns(ns);
	cpu = cpu_time_of(t) - cpu;
	wall = now_ns() - wall;
	return wall ? (int)(100 * cpu / wall) : -1;
}

/*
 * The probe: from a process that joins @cg (the root domain of its CPUs),
 * threads each make themselves a deadline task of PROBE_RUNTIME every
 * PROBE_PERIOD, one at a time, until one is refused; then they all exit.
 * Returns how many were admitted, or -1. The threads never run past their
 * first instants, so their bandwidth goes at their death; probe() waits for
 * their 0-lag times all the same.
 */
struct probe_slot {
	pthread_t t;
	int err;
};

static sem_t probe_done;
static int probe_hold[2];

static void *probe_thread(void *arg)
{
	struct probe_slot *s = arg;
	char c;

	s->err = set_deadline(0, PROBE_RUNTIME, PROBE_PERIOD);
	if (sem_post(&probe_done))
		ksft_print_msg("sem_post: %s\n", strerror(errno));
	/* Asleep until the probe is over: the write end closes. */
	while (!s->err) {
		ssize_t r = read(probe_hold[0], &c, 1);

		if (r >= 0)
			break;
		if (errno != EINTR) {
			s->err = -errno;
			break;
		}
	}
	return NULL;
}

static int probe(const char *cg)
{
	int fd[2], n = -1, max = 16 * ncpus + 16;
	pid_t pid;

	if (pipe(fd)) {
		ksft_print_msg("pipe: %s\n", strerror(errno));
		return -1;
	}
	pid = fork_child("the admission check");
	if (!pid) {
		struct probe_slot *s = calloc(max, sizeof(*s));
		pthread_attr_t a;
		int i, k = 0;

		close(fd[0]);
		if (!s || sem_init(&probe_done, 0, 0) || pipe(probe_hold) ||
		    (cg && enter(cg, getpid())))
			_exit(1);
		if (pthread_attr_init(&a) || pthread_attr_setstacksize(&a, THREAD_STACK))
			_exit(1);
		for (i = 0; i < max; i++) {
			if (pthread_create(&s[i].t, &a, probe_thread, &s[i])) {
				k = -1;	/* not an admission refused */
				break;
			}
			if (sem_wait_for(&probe_done, 10000)) {
				k = -1;
				i++;	/* that thread is to be joined as well */
				break;
			}
			if (s[i].err) {
				if (s[i].err != -EBUSY)
					k = -1;
				join(s[i].t);
				break;
			}
			if (k >= 0)
				k++;
		}
		if (i == max)
			k = -1;
		close(probe_hold[1]);
		while (--i >= 0)
			join(s[i].t);
		if (write(fd[1], &k, sizeof(k)) != sizeof(k))
			_exit(1);
		_exit(0);
	}
	close(fd[1]);
	if (pid < 0 || read(fd[0], &n, sizeof(n)) != sizeof(n))
		n = -1;
	close(fd[0]);
	reap(pid);
	sleep_ns(3 * PROBE_PERIOD);
	return n;
}

/*
 * A case runs in a child process, in @cg, so that all its threads go with
 * it: @fn returns KSFT_PASS or KSFT_FAIL, or KSFT_SKIP.
 */
static int run_in(const char *cg, int (*fn)(void))
{
	pid_t pid = fork_child("a case");
	int status;

	if (!pid) {
		if (cg && enter(cg, getpid()))
			_exit(KSFT_SKIP);
		_exit(fn());
	}
	if (pid < 0)
		return KSFT_FAIL;
	while (waitpid(pid, &status, 0) != pid) {
		if (errno == EINTR)
			continue;
		ksft_print_msg("waitpid %d: %s\n", pid, strerror(errno));
		return KSFT_FAIL;
	}
	if (!WIFEXITED(status)) {
		ksft_print_msg("the case died of signal %d\n",
			       WIFSIGNALED(status) ? WTERMSIG(status) : 0);
		return KSFT_FAIL;
	}
	return WEXITSTATUS(status);
}

/* @fn in @cg, with the free bandwidth of @cg's root domain the same after. */
static int measured(const char *name, const char *cg, int (*fn)(void))
{
	int before, after, ret;

	before = probe(cg);
	if (before <= 0) {
		ksft_print_msg("%s: no room to probe (%d)\n", name, before);
		return KSFT_SKIP;
	}
	ret = run_in(cg, fn);
	sleep_ns(3 * PERIOD);
	after = probe(cg);
	if (ret == KSFT_PASS && after != before) {
		ksft_print_msg("%s: %d probe tasks admitted before, %d after: %s\n",
			       name, before, after,
			       after < before ? "bandwidth leaked" : "bandwidth released twice");
		ret = KSFT_FAIL;
	}
	return ret;
}

/* Priority inheritance */

static pthread_mutex_t pi_lock;
static sem_t owner_ready, owner_go;
static atomic_int owner_holds, owner_tid;

/* 0, or -1 where a priority inheritance mutex is not to be had. */
static int pi_init(void)
{
	pthread_mutexattr_t a;
	int err;

	err = pthread_mutexattr_init(&a);
	if (!err)
		err = pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
	if (!err)
		err = pthread_mutex_init(&pi_lock, &a);
	if (err) {
		ksft_print_msg("no priority inheritance mutex: %s\n", strerror(err));
		return -1;
	}
	if (sem_init(&owner_ready, 0, 0) || sem_init(&owner_go, 0, 0)) {
		ksft_print_msg("sem_init: %s\n", strerror(errno));
		return -1;
	}
	owner_holds = 0;
	return 0;
}

/*
 * The calling thread, a deadline task meanwhile, waits for the lock up to
 * @ms: 0 when it times out, or gets the lock, as @timeout expects.
 */
static int wait_lock_for(int ms, int timeout)
{
	struct timespec ts;
	int err;

	err = set_deadline(0, 5 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
	if (err) {
		ksft_print_msg("a waiter refused as a deadline task: %s\n", strerror(-err));
		return -1;
	}
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)ms * 1000000L;
	ts.tv_sec += ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;
	err = pthread_mutex_timedlock(&pi_lock, &ts);
	if (!err) {
		int e = pthread_mutex_unlock(&pi_lock);

		if (e)
			ksft_print_msg("a waiter cannot unlock: %s\n", strerror(e));
	}
	if (set_other(0)) {
		ksft_print_msg("a waiter cannot leave SCHED_DEADLINE\n");
		return -1;
	}
	if (err != (timeout ? ETIMEDOUT : 0)) {
		ksft_print_msg("a waiter of the lock: %s, not %s\n", err ? strerror(err) : "got it",
			       timeout ? "a timeout" : "the lock");
		return -1;
	}
	return 0;
}

/* The lock of an owner: -1 told of, and owner_holds says so. */
static int take_pi_lock(void)
{
	int err = pthread_mutex_lock(&pi_lock);

	if (err) {
		ksft_print_msg("an owner cannot take the lock: %s\n", strerror(err));
		owner_holds = -1;
	}
	return err ? -1 : 0;
}

static void drop_pi_lock(void)
{
	int err = pthread_mutex_unlock(&pi_lock);

	if (err)
		ksft_print_msg("an owner cannot unlock: %s\n", strerror(err));
}

/* Owner: was a deadline task (@was_dl), takes the lock and sleeps with it. */
static void *sleeping_owner(void *arg)
{
	int was_dl = (long)arg;

	owner_tid = gettid_();
	if (was_dl) {
		if (set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD)) {
			owner_holds = -1;
			sem_post(&owner_ready);
			return (void *)1;
		}
		spin_ns(NSEC_PER_MSEC);
		/* Asleep past its 0-lag time, switched out by the main thread. */
		sem_post(&owner_ready);
		sem_wait_sig(&owner_go);
	}
	if (take_pi_lock())
		return (void *)1;
	owner_holds = 2;
	sleep_ns(400 * NSEC_PER_MSEC);
	drop_pi_lock();
	return NULL;
}

static int pi_asleep(int was_dl, int rounds)
{
	pthread_t o;
	int r, ret = KSFT_PASS;

	if (pi_init())
		return KSFT_SKIP;
	if (pthread_create(&o, NULL, sleeping_owner, (void *)(long)was_dl)) {
		ksft_print_msg("the owner thread could not start\n");
		return KSFT_FAIL;
	}
	if (was_dl) {
		if (sem_wait_for(&owner_ready, 5000)) {
			ksft_print_msg("the owner did not become a deadline task\n");
			ret = KSFT_FAIL;
		} else {
			sleep_ns(200 * NSEC_PER_MSEC);
			if (set_other(owner_tid)) {
				ksft_print_msg("the owner cannot leave SCHED_DEADLINE\n");
				ret = KSFT_FAIL;
			}
		}
		sem_post(&owner_go);
	}
	if (ret == KSFT_PASS && (wait_for(5000, owner_holds) || owner_holds != 2)) {
		ksft_print_msg("the owner never took the lock\n");
		ret = KSFT_FAIL;
	}
	if (ret == KSFT_PASS)
		sleep_ns(20 * NSEC_PER_MSEC);
	/* Each attempt boosts the sleeping owner, its timeout deboosts it asleep. */
	for (r = 0; r < rounds && ret == KSFT_PASS; r++) {
		if (wait_lock_for(20, 1)) {
			ksft_print_msg("round %d of %d failed\n", r + 1, rounds);
			ret = KSFT_FAIL;
		}
	}
	join(o);
	return ret;
}

static int pi_other_owner_asleep(void)
{
	return pi_asleep(0, 5);
}

static int pi_former_owner_asleep(void)
{
	return pi_asleep(1, 5);
}

/* A deadline owner boosted by a deadline waiter of an earlier deadline. */
static void *dl_owner(void *arg)
{
	uint64_t end;

	if (set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD)) {
		owner_holds = -1;
		return (void *)1;
	}
	if (take_pi_lock())
		return (void *)1;
	owner_holds = 2;
	end = now_ns() + 300 * NSEC_PER_MSEC;
	while (now_ns() < end) {
		spin_ns(2 * NSEC_PER_MSEC);
		sleep_ns(5 * NSEC_PER_MSEC);
	}
	drop_pi_lock();
	return NULL;
}

static int pi_deadline_owner(void)
{
	int r, ret = KSFT_PASS, err;
	pthread_t o;
	void *res;

	if (pi_init())
		return KSFT_SKIP;
	if (pthread_create(&o, NULL, dl_owner, NULL)) {
		ksft_print_msg("the owner thread could not start\n");
		return KSFT_FAIL;
	}
	if (wait_for(5000, owner_holds) || owner_holds != 2) {
		ksft_print_msg("the deadline owner never took the lock\n");
		ret = KSFT_FAIL;
	}
	for (r = 0; r < 5 && ret == KSFT_PASS; r++)
		if (wait_lock_for(20, 1))
			ret = KSFT_FAIL;
	err = pthread_join(o, &res);
	if (err)
		ksft_print_msg("pthread_join: %s\n", strerror(err));
	return res || err ? KSFT_FAIL : ret;
}

/* The owner runs when boosted, and its unlock deboosts it. */
static void *running_owner(void *arg)
{
	if (take_pi_lock())
		return (void *)1;
	owner_holds = 2;
	spin_ns(50 * NSEC_PER_MSEC);
	drop_pi_lock();
	return NULL;
}

static int pi_unlock_running(void)
{
	pthread_t o;
	int r, ret = KSFT_PASS;

	for (r = 0; r < 5 && ret == KSFT_PASS; r++) {
		if (pi_init())
			return KSFT_SKIP;
		if (pthread_create(&o, NULL, running_owner, NULL)) {
			ksft_print_msg("the owner thread could not start\n");
			return KSFT_FAIL;
		}
		if (wait_for_step(5000, 100 * NSEC_PER_USEC, owner_holds) ||
		    owner_holds != 2) {
			ksft_print_msg("the owner never took the lock\n");
			ret = KSFT_FAIL;
		} else if (wait_lock_for(1000, 0)) {
			ret = KSFT_FAIL;
		}
		join(o);
	}
	return ret;
}

/* Parameter changes */

static atomic_int t_state;	/* 1: about to sleep, or spinning */
static _Atomic pid_t t_tid;
static sem_t t_exit;

/* A deadline task that runs @run_ns of its runtime, then sleeps. */
static void *runs_then_sleeps(void *arg)
{
	uint64_t run = (uint64_t)(long)arg;

	t_tid = gettid_();
	if (set_deadline(0, 20 * NSEC_PER_MSEC, PERIOD)) {
		t_state = -1;
		return (void *)1;
	}
	sleep_ns(PERIOD);	/* a fresh period */
	spin_ns(run);
	t_state = 1;
	sem_wait_sig(&t_exit);
	return NULL;
}

/* Starts runs_then_sleeps(): 0 once it is about to sleep, or -1. */
static int start_sleeper(pthread_t *t, uint64_t run)
{
	t_state = 0;
	if (sem_init(&t_exit, 0, 0)) {
		ksft_print_msg("sem_init: %s\n", strerror(errno));
		return -1;
	}
	if (pthread_create(t, NULL, runs_then_sleeps, (void *)(long)run)) {
		ksft_print_msg("the sleeper thread could not start\n");
		return -1;
	}
	if (wait_for_step(5000, 100 * NSEC_PER_USEC, t_state) || t_state != 1) {
		ksft_print_msg("the sleeper was refused SCHED_DEADLINE\n");
		join(*t);
		return -1;
	}
	return 0;
}

static void stop_sleeper(pthread_t t)
{
	if (sem_post(&t_exit))
		ksft_print_msg("sem_post: %s\n", strerror(errno));
	join(t);
}

/* The root domain of @cg admits @want probe tasks now. */
static int admits_in(const char *cg, int want, const char *what)
{
	int n = probe(cg);

	if (n != want) {
		ksft_print_msg("%s: %d probe tasks admitted, %d expected\n", what, n, want);
		return -1;
	}
	return 0;
}

/* The root domain of the caller's cgroup admits @want probe tasks now. */
static int admits(int want, const char *what)
{
	return admits_in(NULL, want, what);
}

static int params_asleep(void)
{
	static const uint64_t rt[] = { 40, 10, 30, 20 };
	int base, i, ret = KSFT_PASS;
	pthread_t t;

	if (start_sleeper(&t, NSEC_PER_MSEC))
		return KSFT_FAIL;
	sleep_ns(2 * PERIOD);
	/* Probed from here, in P: this process is P's. */
	base = probe(NULL) + 2;		/* the task's 20% back */
	for (i = 0; i < 4 && ret == KSFT_PASS; i++) {
		char what[64];

		if (set_deadline(t_tid, rt[i] * NSEC_PER_MSEC, PERIOD)) {
			ret = KSFT_FAIL;
			break;
		}
		snprintf(what, sizeof(what), "asleep, changed to %llums every 100ms",
			 (unsigned long long)rt[i]);
		if (admits(base - (int)(rt[i] / 10), what))
			ret = KSFT_FAIL;
	}
	stop_sleeper(t);
	return ret;
}

static int params_non_contending(void)
{
	pthread_t t;
	int ret = KSFT_PASS;

	/* 5ms of 20ms run: its 0-lag time some 20ms after it sleeps. */
	if (start_sleeper(&t, 5 * NSEC_PER_MSEC))
		return KSFT_FAIL;
	sleep_ns(NSEC_PER_MSEC);
	if (set_deadline(t_tid, 40 * NSEC_PER_MSEC, PERIOD) ||
	    set_deadline(t_tid, 10 * NSEC_PER_MSEC, PERIOD))
		ret = KSFT_FAIL;
	stop_sleeper(t);
	return ret;
}

static atomic_int spin_stop;
static uint64_t spin_runtime, spin_flags;	/* set before the thread starts */

static void *dl_spinner(void *arg)
{
	t_tid = gettid_();
	if (set_deadline_flags(0, spin_runtime, PERIOD, spin_flags)) {
		t_state = -1;
		return (void *)1;
	}
	t_state = 1;
	while (!spin_stop)
		;
	return NULL;
}

/* Starts dl_spinner() of @runtime every 100ms with @flags: 0, or -1. */
static int start_spinner(pthread_t *t, uint64_t runtime, uint64_t flags)
{
	t_state = 0;
	spin_stop = 0;
	spin_runtime = runtime;
	spin_flags = flags;
	if (pthread_create(t, NULL, dl_spinner, NULL)) {
		ksft_print_msg("the spinner thread could not start\n");
		return -1;
	}
	if (!wait_for(5000, t_state) && t_state == 1)
		return 0;
	join(*t);
	return -1;
}

static void stop_spinner(pthread_t t)
{
	spin_stop = 1;
	set_other(t_tid);	/* out of the class already: no matter */
	join(t);
}

/* Waits until @t stops using CPU time: throttled. 0, or -1 after 1s. */
static int wait_throttled(pthread_t t)
{
	uint64_t end = now_ns() + NSEC_PER_SEC, last = cpu_time_of(t);

	while (now_ns() < end) {
		uint64_t cur;

		sleep_ns(3 * NSEC_PER_MSEC);
		cur = cpu_time_of(t);
		if (cur == last)
			return 0;
		last = cur;
	}
	return -1;
}

/* @t runs again within @ns. */
static int progresses(pthread_t t, uint64_t ns)
{
	uint64_t start = cpu_time_of(t), end = now_ns() + ns;

	while (now_ns() < end) {
		if (cpu_time_of(t) > start + NSEC_PER_MSEC)
			return 0;
		sleep_ns(2 * NSEC_PER_MSEC);
	}
	return -1;
}

static int params_throttled(void)
{
	int r, ret = KSFT_PASS;
	pthread_t t;

	if (start_spinner(&t, 10 * NSEC_PER_MSEC, 0))
		return KSFT_FAIL;
	for (r = 0; r < 5 && ret == KSFT_PASS; r++) {
		if (wait_throttled(t) ||
		    set_deadline(t_tid, (r & 1 ? 10 : 20) * NSEC_PER_MSEC, PERIOD)) {
			ret = KSFT_FAIL;
			break;
		}
		if (progresses(t, 3 * PERIOD)) {
			ksft_print_msg("throttled, parameters changed: never ran again\n");
			ret = KSFT_FAIL;
		}
	}
	stop_spinner(t);
	return ret;
}

static int params_refused(void)
{
	pthread_t t;
	int ret = KSFT_PASS, base, err;

	if (start_sleeper(&t, NSEC_PER_MSEC))
		return KSFT_FAIL;
	sleep_ns(2 * PERIOD);
	base = probe(NULL);
	/*
	 * P's free bandwidth is at least @base probe tasks of 10%, and less
	 * than one more: growing the task by one more than that is refused,
	 * whatever the admission limit.
	 */
	if (20 + 10 * (base + 1) > 100) {
		stop_sleeper(t);
		ksft_print_msg("P has more room than one of its tasks can take\n");
		return KSFT_SKIP;
	}
	err = set_deadline(t_tid, (20 + 10 * (uint64_t)(base + 1)) * NSEC_PER_MSEC, PERIOD);
	if (err != -EBUSY) {
		ksft_print_msg("an increase beyond P's room: %s, not EBUSY\n",
			       err ? strerror(-err) : "accepted");
		ret = KSFT_FAIL;
	} else if (admits(base, "after a refused increase")) {
		ret = KSFT_FAIL;
	}
	stop_sleeper(t);
	return ret;
}

/*
 * SCHED_FLAG_KEEP_PARAMS with a change of class is refused, with nothing
 * reserved for the deadline parameters it would keep: the task's own,
 * its slice the runtime.
 */
static int params_keep_class_change(void)
{
	struct dl_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_OTHER,
		.sched_runtime = 40 * NSEC_PER_MSEC,
	};
	int err;

	if (syscall(SYS_sched_setattr, 0, &attr, 0)) {
		ksft_print_msg("a slice of its own is refused: %s\n", strerror(errno));
		return KSFT_SKIP;
	}
	attr.sched_policy = SCHED_DEADLINE;
	attr.sched_flags = SCHED_FLAG_KEEP_PARAMS;
	attr.sched_deadline = PERIOD;
	attr.sched_period = PERIOD;
	err = syscall(SYS_sched_setattr, 0, &attr, 0) ? -errno : 0;
	if (err != -EINVAL) {
		ksft_print_msg("to SCHED_DEADLINE with SCHED_FLAG_KEEP_PARAMS: %s, not EINVAL\n",
			       err ? strerror(-err) : "accepted");
		return KSFT_FAIL;
	}
	return KSFT_PASS;
}

/* A deadline task that blocks on the lock: it boosts its owner. */
static void *boosting_waiter(void *arg)
{
	return (void *)(long)wait_lock_for(300, 1);
}

/*
 * SCHED_FLAG_KEEP_PARAMS with SCHED_DEADLINE on an owner that priority
 * inheritance has boosted into the class: the class of the request is the
 * one the owner already has, so a class change is not what it asks for,
 * and the policy it asks for is one the kept parameters would not give
 * it. It is refused with EINVAL; admitted, it reserves the bandwidth of
 * the owner's slice and nothing ever releases it.
 */
static int params_keep_boosted(void)
{
	struct dl_attr slice = {
		.size = sizeof(slice),
		.sched_policy = SCHED_OTHER,
		.sched_runtime = 40 * NSEC_PER_MSEC,	/* what the request would keep */
	};
	struct dl_attr keep = {
		.size = sizeof(keep),
		.sched_policy = SCHED_DEADLINE,
		.sched_flags = SCHED_FLAG_KEEP_PARAMS,
		.sched_deadline = PERIOD,
		.sched_period = PERIOD,
	};
	int ret = KSFT_PASS, err;
	pthread_t o, w;

	if (pi_init())
		return KSFT_SKIP;
	if (pthread_create(&o, NULL, sleeping_owner, (void *)0L)) {
		ksft_print_msg("the owner thread could not start\n");
		return KSFT_FAIL;
	}
	if (wait_for(5000, owner_holds) || owner_holds != 2) {
		ksft_print_msg("the owner never took the lock\n");
		join(o);
		return KSFT_FAIL;
	}
	if (syscall(SYS_sched_setattr, owner_tid, &slice, 0)) {
		ksft_print_msg("the owner cannot take a slice of its own: %s\n",
			       strerror(errno));
		ret = KSFT_SKIP;
		goto out;
	}
	if (pthread_create(&w, NULL, boosting_waiter, NULL)) {
		ksft_print_msg("the waiter thread could not start\n");
		ret = KSFT_FAIL;
		goto out;
	}
	sleep_ns(50 * NSEC_PER_MSEC);		/* it blocks, the owner is boosted */
	err = syscall(SYS_sched_setattr, owner_tid, &keep, 0) ? -errno : 0;
	if (err != -EINVAL) {
		ksft_print_msg("KEEP_PARAMS to SCHED_DEADLINE on a boosted owner: %s, not EINVAL\n",
			       err ? strerror(-err) : "accepted");
		ret = KSFT_FAIL;
	}
	join(w);
out:
	join(o);
	return ret;
}

/* cpuset moves, from the main process, outside P */

/* Shared with the processes of a case: atomic in shared memory. */
static atomic_int *mv_state;

/* mv_state[0] says a child is up (1) or was refused SCHED_DEADLINE (-1). */
static int child_up(const char *what, int fast)
{
	if (wait_for_step(5000, fast ? 100 * NSEC_PER_USEC : NSEC_PER_MSEC, mv_state[0])) {
		ksft_print_msg("%s: the task never started\n", what);
		return -1;
	}
	if (mv_state[0] != 1) {
		ksft_print_msg("%s: the task was refused SCHED_DEADLINE\n", what);
		return -1;
	}
	return 0;
}

/* A process whose one deadline thread does what @mode says, in the root cgroup. */
enum mv_mode { MV_ASLEEP, MV_THROTTLED, MV_YIELDED, MV_NON_CONTENDING };

static pid_t mover(enum mv_mode mode, uint64_t runtime)
{
	pid_t pid = fork_child("a deadline task");

	if (!pid) {
		if (set_deadline(0, runtime, PERIOD)) {
			mv_state[0] = -1;
			_exit(1);
		}
		if (mode == MV_ASLEEP) {
			spin_ns(NSEC_PER_MSEC);
			*mv_state = 1;
			for (;;)
				pause();
		}
		if (mode == MV_NON_CONTENDING) {
			sleep_ns(PERIOD);
			spin_ns(runtime / 4);
			*mv_state = 1;
			for (;;)
				pause();
		}
		if (mode == MV_YIELDED) {
			*mv_state = 1;
			for (;;) {
				spin_ns(NSEC_PER_MSEC / 2);
				mv_state[1]++;
				sched_yield();
			}
		}
		*mv_state = 1;
		for (;;)
			mv_state[1]++;
	}
	return pid;
}

static int moves(enum mv_mode mode, const char *name)
{
	int base_p, base_r, after_p, after_r, ret = KSFT_PASS;
	pid_t pid;

	base_p = probe(cg_p);
	base_r = probe(root);
	mv_state[0] = 0;
	mv_state[1] = 0;
	pid = mover(mode, 30 * NSEC_PER_MSEC);
	if (pid < 0 || child_up(name, mode == MV_NON_CONTENDING)) {
		reap(pid);
		return KSFT_FAIL;
	}
	if (mode == MV_NON_CONTENDING)
		sleep_ns(NSEC_PER_MSEC);
	else
		sleep_ns(2 * PERIOD);
	if (enter(cg_p, pid) || enter(root, pid)) {
		ksft_print_msg("%s: a move refused\n", name);
		ret = KSFT_FAIL;
	}
	if (mode == MV_THROTTLED || mode == MV_YIELDED) {
		int last = mv_state[1];

		sleep_ns(3 * PERIOD);
		if (mv_state[1] == last) {
			ksft_print_msg("%s: moved, never ran again\n", name);
			ret = KSFT_FAIL;
		}
	}
	reap(pid);
	sleep_ns(3 * PERIOD);
	after_p = probe(cg_p);
	after_r = probe(root);
	if (ret == KSFT_PASS && (after_p != base_p || after_r != base_r)) {
		ksft_print_msg("%s: probe tasks admitted, P %d -> %d, the other CPUs %d -> %d\n",
			       name, base_p, after_p, base_r, after_r);
		ret = KSFT_FAIL;
	}
	return ret;
}

static int move_refused(void)
{
	int base_p, base_r, ret = KSFT_PASS, err;
	pid_t in_p, pid;

	base_p = probe(cg_p);
	base_r = probe(root);
	mv_state[0] = 0;
	/* 60% in P, then a task of 60% moved in: refused. */
	in_p = fork_child("a deadline task of P");
	if (!in_p) {
		if (enter(cg_p, getpid()) || set_deadline(0, 60 * NSEC_PER_MSEC, PERIOD))
			_exit(1);
		mv_state[0] = 1;
		for (;;)
			pause();
	}
	if (in_p < 0 || child_up("a task of 60% in P", 0)) {
		reap(in_p);
		return KSFT_FAIL;
	}
	mv_state[0] = 0;
	pid = mover(MV_ASLEEP, 60 * NSEC_PER_MSEC);
	if (pid < 0 || child_up("a task of 60% to move into P", 0)) {
		reap(pid);
		reap(in_p);
		return KSFT_FAIL;
	}
	err = enter(cg_p, pid);
	if (err != -EBUSY) {
		ksft_print_msg("a move beyond P's room: %s, not EBUSY\n",
			       err ? strerror(-err) : "accepted");
		ret = KSFT_FAIL;
	}
	reap(pid);
	reap(in_p);
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS && (probe(cg_p) != base_p || probe(root) != base_r)) {
		ksft_print_msg("a refused move changed the free bandwidth\n");
		ret = KSFT_FAIL;
	}
	return ret;
}

static int move_asleep(void)
{
	return moves(MV_ASLEEP, "move_asleep");
}

static int move_throttled(void)
{
	return moves(MV_THROTTLED, "move_throttled");
}

static int move_non_contending(void)
{
	return moves(MV_NON_CONTENDING, "move_non_contending");
}

/* Deaths, in P */

enum die_mode { DIE_RUNNING, DIE_SWITCHED_OUT, DIE_SHRUNK };

/*
 * A deadline task of 30% that runs 5ms of a fresh period, its 0-lag time
 * ahead, and dies there: as it is, or switched to SCHED_OTHER, its
 * bandwidth then going at that time; or shrunk to 10%, which puts that
 * time behind it, and the bandwidth goes at its death.
 */
static int die_case(enum die_mode mode, const char *what)
{
	pid_t pid = fork_child("a task that dies");
	int status;

	if (!pid) {
		if (set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD))
			_exit(1);
		sleep_ns(PERIOD);
		spin_ns(5 * NSEC_PER_MSEC);
		if (mode == DIE_SWITCHED_OUT && set_other(0))
			_exit(2);
		if (mode == DIE_SHRUNK && set_deadline(0, 10 * NSEC_PER_MSEC, PERIOD))
			_exit(3);
		_exit(0);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status)) {
		ksft_print_msg("a task that dies %s failed\n", what);
		return KSFT_FAIL;
	}
	return KSFT_PASS;
}

static int death_running(void)
{
	return die_case(DIE_RUNNING, "running");
}

static int death_switched_out(void)
{
	return die_case(DIE_SWITCHED_OUT, "switched out");
}

static int death_shrunk(void)
{
	return die_case(DIE_SHRUNK, "shrunk");
}

/*
 * Waits until the spinner's mv_state[1] moves (@moving): it runs; or stops
 * for 5ms: it is throttled. 0, or -1 after 1s.
 */
static int wait_spinner(int moving)
{
	uint64_t end = now_ns() + NSEC_PER_SEC;
	int last = mv_state[1];

	while (now_ns() < end) {
		sleep_ns(moving ? NSEC_PER_MSEC : 5 * NSEC_PER_MSEC);
		if (moving ? mv_state[1] != last : mv_state[1] == last)
			return 0;
		last = mv_state[1];
	}
	return -1;
}

enum kill_when { KILL_AFTER, KILL_THROTTLED, KILL_RUNNING };

/*
 * A deadline task of 30% (mover()) killed in the state @mode leaves it in,
 * @after that, or once it is throttled, or while it runs. The killer
 * leaves P first, to watch and kill from another CPU without taking the
 * task's.
 */
static int kill_case(enum mv_mode mode, enum kill_when when, uint64_t after, const char *what)
{
	int ret = KSFT_PASS;
	pid_t pid;

	mv_state[0] = 0;
	mv_state[1] = 0;
	pid = mover(mode, 30 * NSEC_PER_MSEC);
	if (pid < 0) {
		ret = KSFT_FAIL;
		goto out;
	}
	if (enter(root, getpid())) {
		ksft_print_msg("killed %s: the killer cannot leave P\n", what);
		ret = KSFT_FAIL;
		goto out;
	}
	if (child_up(what, 1)) {
		ret = KSFT_FAIL;
	} else if (when != KILL_AFTER && wait_spinner(when == KILL_RUNNING)) {
		ksft_print_msg("killed %s: never %s\n", what,
			       when == KILL_RUNNING ? "ran" : "throttled");
		ret = KSFT_FAIL;
	} else {
		sleep_ns(after);
	}
out:
	reap(pid);
	return ret;
}

static int death_killed_asleep(void)
{
	return kill_case(MV_ASLEEP, KILL_AFTER, 2 * PERIOD, "asleep");
}

static int death_killed_non_contending(void)
{
	return kill_case(MV_NON_CONTENDING, KILL_AFTER, 0, "before its 0-lag time");
}

static int death_killed_throttled(void)
{
	return kill_case(MV_THROTTLED, KILL_THROTTLED, 0, "throttled");
}

static int death_killed_running(void)
{
	return kill_case(MV_THROTTLED, KILL_RUNNING, 0, "running");
}

/*
 * A robust priority inheritance lock that processes share: mv_state[0]
 * tells its owner's state, mv_state[2] its waiter's.
 */
static pthread_mutex_t *shm_lock;

/* 0, or -1 where such a mutex is not to be had. */
static int shm_lock_init(void)
{
	pthread_mutexattr_t a;
	int err;

	err = pthread_mutexattr_init(&a);
	if (!err)
		err = pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
	if (!err)
		err = pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
	if (!err)
		err = pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
	if (!err)
		err = pthread_mutex_init(shm_lock, &a);
	if (err) {
		ksft_print_msg("no shared priority inheritance mutex: %s\n", strerror(err));
		return -1;
	}
	mv_state[0] = 0;
	mv_state[2] = 0;
	return 0;
}

/* Takes the lock, as a deadline task of 30% if @dl, and sleeps with it. */
static pid_t lock_owner(int dl)
{
	pid_t pid = fork_child("the owner of the lock");

	if (!pid) {
		if ((dl && set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD)) ||
		    pthread_mutex_lock(shm_lock)) {
			mv_state[0] = -1;
			_exit(1);
		}
		mv_state[0] = 1;
		for (;;)
			pause();
	}
	return pid;
}

/* A deadline task of 5ms every 50ms that waits for the lock: 2 once it got it. */
static pid_t lock_waiter(void)
{
	pid_t pid = fork_child("a waiter of the lock");

	if (!pid) {
		int err;

		if (set_deadline(0, 5 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC)) {
			mv_state[2] = -1;
			_exit(1);
		}
		mv_state[2] = 1;
		err = pthread_mutex_lock(shm_lock);
		if (err == EOWNERDEAD)
			err = pthread_mutex_consistent(shm_lock);
		if (!err)
			pthread_mutex_unlock(shm_lock);
		mv_state[2] = err ? -2 : 2;
		for (;;)
			pause();
	}
	return pid;
}

/* @var reaches @want within 1s: 0, or -1. */
static int wait_state(atomic_int *var, int want)
{
	uint64_t end = now_ns() + NSEC_PER_SEC;

	while (now_ns() < end) {
		if (*var == want)
			return 0;
		sleep_ns(NSEC_PER_MSEC);
	}
	return -1;
}

/*
 * The owner sleeps with the lock, a deadline task if @dl_owner, and the
 * waiter boosts it; then @kill_owner, or the waiter, dies. A killed owner
 * does not die boosted: its exit releases its priority inheritance
 * futexes first (futex_exit_release()), which deboosts it.
 */
static int pi_death(int dl_owner, int kill_owner, const char *what)
{
	int ret = KSFT_PASS;
	pid_t owner, waiter = -1;

	if (shm_lock_init())
		return KSFT_SKIP;
	owner = lock_owner(dl_owner);
	if (owner < 0)
		return KSFT_FAIL;
	if (wait_state(&mv_state[0], 1)) {
		ksft_print_msg("%s: no owner\n", what);
		ret = KSFT_FAIL;
		goto out;
	}
	waiter = lock_waiter();
	if (waiter < 0) {
		ret = KSFT_FAIL;
		goto out;
	}
	if (wait_state(&mv_state[2], 1)) {
		ksft_print_msg("%s: no waiter\n", what);
		ret = KSFT_FAIL;
		goto out;
	}
	sleep_ns(50 * NSEC_PER_MSEC);	/* blocked, boosting the owner */
	if (kill_owner) {
		reap(owner);
		owner = -1;
		if (wait_state(&mv_state[2], 2)) {
			ksft_print_msg("%s: the waiter did not get the lock\n", what);
			ret = KSFT_FAIL;
		}
	} else {
		reap(waiter);
		waiter = -1;
		sleep_ns(50 * NSEC_PER_MSEC);	/* the owner deboosted asleep */
	}
out:
	reap(waiter);
	reap(owner);
	return ret;
}

static int death_pi_waiter(void)
{
	return pi_death(0, 0, "a waiter killed");
}

static int death_pi_owner(void)
{
	return pi_death(0, 1, "a boosted owner killed");
}

static int death_pi_deadline_owner(void)
{
	return pi_death(1, 1, "a boosted deadline owner killed");
}

/* Deadline threads of one process, asleep, throttled and running, killed at once. */
static void *group_thread(void *arg)
{
	long kind = (long)arg;

	if (set_deadline(0, 10 * NSEC_PER_MSEC, PERIOD)) {
		mv_state[0] = -1000;
		return NULL;
	}
	spin_ns(NSEC_PER_MSEC);
	mv_state[0]++;
	if (!kind)
		for (;;)
			pause();
	for (;;)
		;
	return NULL;
}

static int death_group(void)
{
	pid_t pid;

	mv_state[0] = 0;
	pid = fork_child("a group of deadline threads");
	if (pid < 0)
		return KSFT_FAIL;
	if (!pid) {
		pthread_t t;
		long i;

		for (i = 0; i < 3; i++)
			if (pthread_create(&t, NULL, group_thread, (void *)i))
				mv_state[0] = -1000;
		for (;;)
			pause();
	}
	if (wait_for(5000, mv_state[0] < 0 || mv_state[0] == 3))
		ksft_print_msg("only %d threads of the group started\n", (int)mv_state[0]);
	sleep_ns(PERIOD / 2);
	reap(pid);
	if (mv_state[0] != 3) {
		ksft_print_msg("a thread of the group refused SCHED_DEADLINE\n");
		return KSFT_FAIL;
	}
	return KSFT_PASS;
}

/* sched_yield() */

static atomic_int y_rounds;	/* the yields of the yielder */

/*
 * A deadline task of 30% that yields half a millisecond into every
 * period: it spends nearly all of its time throttled by sched_yield(),
 * with runtime left, so its 0-lag time is a few milliseconds away and the
 * inactive timer that releases its bandwidth is armed for that long.
 * @t_state is 2 while it is in the yield.
 */
static void *yielder(void *arg)
{
	t_tid = gettid_();
	if (set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD)) {
		t_state = -1;
		return (void *)1;
	}
	t_state = 1;
	while (!spin_stop) {
		spin_ns(NSEC_PER_MSEC / 2);
		y_rounds++;
		t_state = 2;
		sched_yield();
		t_state = 1;
	}
	return NULL;
}

static int start_yielder(pthread_t *t)
{
	t_state = 0;
	spin_stop = 0;
	y_rounds = 0;
	if (pthread_create(t, NULL, yielder, NULL)) {
		ksft_print_msg("the yielder thread could not start\n");
		return -1;
	}
	if (!wait_for(5000, t_state) && t_state > 0)
		return 0;
	join(*t);
	return -1;
}

/*
 * A task switched to SCHED_OTHER while it is throttled by sched_yield():
 * its bandwidth comes back, and it runs again.
 */
static int yield_switched_out(void)
{
	int base, ret = KSFT_PASS;
	pthread_t t;

	base = probe(NULL);
	if (base < 3) {
		ksft_print_msg("no room for a task of 30%%\n");
		return KSFT_SKIP;
	}
	if (start_yielder(&t))
		return KSFT_FAIL;
	if (wait_throttled(t) || t_state != 2) {
		ksft_print_msg("the task never yielded\n");
		ret = KSFT_FAIL;
	} else if (set_other(t_tid)) {
		ret = KSFT_FAIL;
	} else {
		int last = y_rounds;

		sleep_ns(2 * PERIOD);
		if (y_rounds == last) {
			ksft_print_msg("yielded, then switched out: never ran again\n");
			ret = KSFT_FAIL;
		}
	}
	stop_spinner(t);
	sleep_ns(3 * PERIOD);		/* its 0-lag time */
	if (ret == KSFT_PASS && admits(base, "yielded, then switched out"))
		ret = KSFT_FAIL;
	return ret;
}

static int yield_moved(void)
{
	return moves(MV_YIELDED, "yield_moved");
}

/* A task killed while it is throttled by sched_yield(). */
static int yield_killed(void)
{
	pid_t pid;

	mv_state[0] = 0;
	pid = fork_child("a task that yields");
	if (!pid) {
		if (set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD)) {
			mv_state[0] = -1;
			_exit(1);
		}
		mv_state[0] = 1;
		for (;;) {
			spin_ns(NSEC_PER_MSEC / 2);
			sched_yield();
		}
	}
	if (pid < 0)
		return KSFT_FAIL;
	if (child_up("a task that yields", 0)) {
		reap(pid);
		return KSFT_FAIL;
	}
	sleep_ns(2 * PERIOD);
	reap(pid);
	return KSFT_PASS;
}

/* SCHED_FLAG_DL_OVERRUN */

static atomic_int got_xcpu;

static void catch_xcpu(int sig)
{
	got_xcpu = 1;
}

/* A deadline task of 10% with SCHED_FLAG_DL_OVERRUN that runs past it. */
static void *overruns(void *arg)
{
	t_tid = gettid_();
	if (set_deadline_flags(0, 10 * NSEC_PER_MSEC, PERIOD, SCHED_FLAG_DL_OVERRUN)) {
		t_state = -1;
		return (void *)1;
	}
	t_state = 1;
	spin_ns(15 * NSEC_PER_MSEC);
	while (!spin_stop)
		sleep_ns(10 * NSEC_PER_MSEC);
	return NULL;
}

/*
 * The thread group of a task with SCHED_FLAG_DL_OVERRUN that overruns its
 * runtime gets SIGXCPU. Caught here: the overrun leaves the task's
 * bandwidth reserved, and it comes back when the task leaves
 * SCHED_DEADLINE.
 */
static int overrun_caught(void)
{
	int base, ret = KSFT_PASS;
	uint64_t end;
	pthread_t t;

	if (signal(SIGXCPU, catch_xcpu) == SIG_ERR)
		return KSFT_FAIL;
	base = probe(NULL);
	if (base < 1) {
		ksft_print_msg("no room for a task of 10%%\n");
		return KSFT_SKIP;
	}
	got_xcpu = 0;
	t_state = 0;
	spin_stop = 0;
	if (pthread_create(&t, NULL, overruns, NULL))
		return KSFT_FAIL;
	while (!t_state)
		sleep_ns(NSEC_PER_MSEC);
	end = now_ns() + 2 * NSEC_PER_SEC;
	while (!got_xcpu && now_ns() < end)
		sleep_ns(5 * NSEC_PER_MSEC);
	if (t_state != 1) {
		ret = KSFT_FAIL;
	} else if (!got_xcpu) {
		ksft_print_msg("it overran its runtime, no SIGXCPU in 2s\n");
		ret = KSFT_FAIL;
	} else if (admits(base - 1, "an overrun told")) {
		ret = KSFT_FAIL;	/* the overrun gave its 10% back */
	}
	stop_spinner(t);
	sleep_ns(3 * PERIOD);		/* its 0-lag time */
	if (ret == KSFT_PASS && admits(base, "after an overrun was told"))
		ret = KSFT_FAIL;
	return ret;
}

/* The deadline threads of overrun_group_death(). */
static void *og_thread(void *arg)
{
	switch ((int)(long)arg) {
	case 0:		/* overruns its runtime: SIGXCPU to the group */
		if (set_deadline_flags(0, 10 * NSEC_PER_MSEC, PERIOD,
				       SCHED_FLAG_DL_OVERRUN))
			_exit(1);
		/*
		 * The other two first: of one CPU, this one has the earliest
		 * deadline of the three and would overrun, and kill them,
		 * before they are where the signal is to find them.
		 */
		if (wait_for(5000, mv_state[0] >= 2))
			_exit(2);
		spin_ns(30 * NSEC_PER_MSEC);
		break;
	case 1:		/* asleep, its 0-lag time past */
		if (set_deadline(0, 20 * NSEC_PER_MSEC, PERIOD))
			_exit(1);
		spin_ns(NSEC_PER_MSEC);
		mv_state[0]++;
		break;
	case 2:		/* asleep, mostly before its 0-lag time */
		if (set_deadline(0, 15 * NSEC_PER_MSEC, PERIOD))
			_exit(1);
		mv_state[0]++;
		for (;;) {
			spin_ns(3 * NSEC_PER_MSEC);
			sleep_ns(22 * NSEC_PER_MSEC);
		}
	}
	for (;;)
		pause();
}

/*
 * The SIGXCPU of an overrun kills the whole thread group: the bandwidth
 * of its three deadline threads comes back.
 */
static int overrun_group_death(void)
{
	pid_t pid;
	int i, status;

	mv_state[0] = 0;
	pid = fork_child("a group of deadline threads");
	if (!pid) {
		pthread_t t;

		for (i = 0; i < 3; i++)
			if (pthread_create(&t, NULL, og_thread, (void *)(long)i))
				_exit(1);
		for (;;)
			pause();
	}
	if (pid < 0)
		return KSFT_FAIL;
	for (i = 0; i < 300; i++) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			break;
		sleep_ns(10 * NSEC_PER_MSEC);
	}
	if (i == 300) {
		reap(pid);
		ksft_print_msg("an overrun of 10ms every 100ms: no SIGXCPU in 3s\n");
		return KSFT_FAIL;
	}
	if (WIFEXITED(status)) {
		ksft_print_msg("a thread of the group was refused SCHED_DEADLINE\n");
		return KSFT_SKIP;
	}
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGXCPU) {
		ksft_print_msg("the group of an overrun died of signal %d, not SIGXCPU\n",
			       WIFSIGNALED(status) ? WTERMSIG(status) : 0);
		return KSFT_FAIL;
	}
	return KSFT_PASS;
}

/* SCHED_FLAG_RECLAIM */

#define RECLAIM_MIN	40	/* % of a CPU a reclaiming task of 10% gets */
#define PLAIN_MAX	25	/* % one without the flag gets */
#define SHARE_SLACK	15	/* % two measures of the same may differ by */
#define SHARE_NS	(400 * NSEC_PER_MSEC)

/*
 * A deadline task with SCHED_FLAG_RECLAIM runs past its runtime, taking
 * the bandwidth of its root domain that no deadline task uses: of a CPU
 * of its own, a task of 10% every 100ms gets most of it, where one
 * without the flag gets its 10%.
 */
static int reclaim_runs(void)
{
	int plain, grub, ret = KSFT_PASS;
	pthread_t t;

	if (start_spinner(&t, 10 * NSEC_PER_MSEC, 0)) {
		ksft_print_msg("no room for a task of 10%%\n");
		return KSFT_SKIP;
	}
	plain = share_of(t, SHARE_NS);
	stop_spinner(t);
	if (start_spinner(&t, 10 * NSEC_PER_MSEC, SCHED_FLAG_RECLAIM))
		return KSFT_FAIL;
	grub = share_of(t, SHARE_NS);
	stop_spinner(t);
	if (plain > PLAIN_MAX || grub < RECLAIM_MIN) {
		ksft_print_msg("of a CPU, a task of 10%% got %d%%, with SCHED_FLAG_RECLAIM %d%%\n",
			       plain, grub);
		ret = KSFT_FAIL;
	}
	return ret;
}

/*
 * A reclaiming task switched out of SCHED_DEADLINE before its 0-lag time
 * and back, which leaves its bandwidth standing until the inactive timer
 * releases it: what it reclaims afterwards is what it reclaimed before.
 */
static int reclaim_switched(void)
{
	int before, after = 0, r, ret = KSFT_PASS;
	pthread_t t;

	if (start_spinner(&t, 10 * NSEC_PER_MSEC, SCHED_FLAG_RECLAIM)) {
		ksft_print_msg("no room for a task of 10%%\n");
		return KSFT_SKIP;
	}
	before = share_of(t, SHARE_NS);
	for (r = 0; r < 4; r++) {
		if (set_other(t_tid)) {
			ret = KSFT_FAIL;
			break;
		}
		if (r & 1)
			sleep_ns(5 * NSEC_PER_MSEC);
		if (set_deadline_flags(t_tid, 10 * NSEC_PER_MSEC, PERIOD,
				       SCHED_FLAG_RECLAIM)) {
			ksft_print_msg("round %d of 4: back to SCHED_DEADLINE refused\n",
				       r + 1);
			ret = KSFT_FAIL;
			break;
		}
		sleep_ns(PERIOD);
	}
	if (ret == KSFT_PASS)
		after = share_of(t, SHARE_NS);
	stop_spinner(t);
	if (ret == KSFT_PASS &&
	    (after < before - SHARE_SLACK || after > before + SHARE_SLACK)) {
		ksft_print_msg("reclaiming: %d%% of a CPU, %d%% after 4 round trips\n",
			       before, after);
		ret = KSFT_FAIL;
	}
	return ret;
}

/*
 * A reclaiming deadline task of 10% that spins, publishing the CPU time
 * it has used in milliseconds, and idles while mv_state[2] is set. The
 * moves happen then: a cpuset move waits for an RCU grace period, which
 * a task that reclaims a whole CPU of its own keeps from ending where
 * RCU runs in kthreads of its own (PREEMPT_RT).
 */
static pid_t reclaim_mover(void)
{
	pid_t pid = fork_child("a reclaiming task");

	if (!pid) {
		if (set_deadline_flags(0, 10 * NSEC_PER_MSEC, PERIOD,
				       SCHED_FLAG_RECLAIM)) {
			mv_state[0] = -1;
			_exit(1);
		}
		mv_state[0] = 1;
		for (;;) {
			if (mv_state[2]) {
				sleep_ns(10 * NSEC_PER_MSEC);
				continue;
			}
			spin_ns(NSEC_PER_MSEC);
			mv_state[1] = (int)(thread_time_ns() / NSEC_PER_MSEC);
		}
	}
	return pid;
}

/* Moves the mover to @cg, idle: 0, or -1. */
static int move_idle(const char *cg, pid_t pid)
{
	int err;

	mv_state[2] = 1;
	sleep_ns(20 * NSEC_PER_MSEC);	/* it stops spinning */
	err = enter(cg, pid);
	mv_state[2] = 0;
	return err;
}

/* The share of a CPU, in percent, the mover uses over the next @ns. */
static int mover_share(uint64_t ns)
{
	uint64_t wall = now_ns();
	int cpu = mv_state[1];

	sleep_ns(ns);
	cpu = mv_state[1] - cpu;
	wall = now_ns() - wall;
	return (int)(100 * (uint64_t)cpu * NSEC_PER_MSEC / wall);
}

/*
 * A reclaiming task moved into P and back, then into P again: what it
 * reclaims of P's CPU is what it reclaimed there before, and the free
 * bandwidth of both root domains is the one it found.
 */
static int reclaim_moved(void)
{
	int base_p, base_r, in_p, again = 0, ret = KSFT_PASS;
	pid_t pid;

	base_p = probe(cg_p);
	base_r = probe(root);
	mv_state[0] = 0;
	mv_state[1] = 0;
	mv_state[2] = 1;		/* idle until it is in P */
	pid = reclaim_mover();
	if (pid < 0)
		return KSFT_FAIL;
	if (child_up("a reclaiming task", 0) || move_idle(cg_p, pid)) {
		mv_state[2] = 1;
		reap(pid);
		return KSFT_FAIL;
	}
	sleep_ns(2 * PERIOD);
	in_p = mover_share(SHARE_NS);
	if (move_idle(root, pid) || move_idle(cg_p, pid)) {
		ksft_print_msg("a move of a reclaiming task refused\n");
		ret = KSFT_FAIL;
	} else {
		sleep_ns(2 * PERIOD);
		again = mover_share(SHARE_NS);
		if (in_p < RECLAIM_MIN || again < in_p - SHARE_SLACK ||
		    again > in_p + SHARE_SLACK) {
			ksft_print_msg("reclaiming: %d%% of P's CPU, %d%% after a move\n",
				       in_p, again);
			ret = KSFT_FAIL;
		}
	}
	mv_state[2] = 1;
	reap(pid);
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS && (probe(cg_p) != base_p || probe(root) != base_r)) {
		ksft_print_msg("a reclaiming task moved: the free bandwidth changed\n");
		ret = KSFT_FAIL;
	}
	return ret;
}

/* Deadline servers */

#define SERVER_RUNTIME	(100 * NSEC_PER_MSEC)	/* 10%, where the cases start from */
#define SERVER_PERIOD	(1000 * NSEC_PER_MSEC)

static const char server_dir[] = "/sys/kernel/debug/sched/fair_server";
static uint64_t *server_found;	/* runtime and period of each CPU's fair server */

static int server_get(int cpu, const char *name, uint64_t *val)
{
	char path[96], buf[32];

	snprintf(path, sizeof(path), "%s/cpu%d/%s", server_dir, cpu, name);
	if (read_file(path, buf, sizeof(buf)))
		return -1;
	*val = strtoull(buf, NULL, 10);
	return 0;
}

static int server_put(int cpu, const char *name, uint64_t val)
{
	char dir[96], buf[32];

	snprintf(dir, sizeof(dir), "%s/cpu%d", server_dir, cpu);
	snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
	return write_file(dir, name, buf);
}

/* The fair server of @cpu runs @runtime every @period: 0, or -errno. */
static int server_set(int cpu, uint64_t runtime, uint64_t period)
{
	uint64_t cur;
	int err;

	if (!server_found || server_get(cpu, "period", &cur))
		return -ENOENT;
	/* Each write keeps the runtime within the period. */
	if (runtime <= cur) {
		err = server_put(cpu, "runtime", runtime);
		return err ? err : server_put(cpu, "period", period);
	}
	err = server_put(cpu, "period", period);
	return err ? err : server_put(cpu, "runtime", runtime);
}

/* The fair servers as the test finds them, or none without debugfs. */
static void server_init(void)
{
	int cpu;

	server_found = calloc(2 * ncpus, sizeof(*server_found));
	for (cpu = 0; server_found && cpu < ncpus; cpu++) {
		if (server_get(cpu, "runtime", &server_found[2 * cpu]) ||
		    server_get(cpu, "period", &server_found[2 * cpu + 1])) {
			free(server_found);
			server_found = NULL;
			ksft_print_msg("no fair server in debugfs: server cases skipped\n");
		}
	}
}

/* Puts the fair servers back as the test found them. */
static void server_restore(void)
{
	uint64_t runtime, period;
	int cpu;

	for (cpu = 0; server_found && cpu < ncpus; cpu++) {
		int err;

		if (server_get(cpu, "runtime", &runtime) || server_get(cpu, "period", &period))
			continue;
		if (runtime == server_found[2 * cpu] && period == server_found[2 * cpu + 1])
			continue;
		/*
		 * A root domain whose departing tasks still hold their
		 * bandwidth has room for nothing, a decrease included:
		 * their 0-lag times are a period or two away.
		 */
		err = wait_for(1000, !server_set(cpu, server_found[2 * cpu],
						 server_found[2 * cpu + 1]));
		if (err)
			ksft_print_msg("the fair server of CPU %d stays changed\n", cpu);
	}
}

/* Deadline threads of 10% every 100ms that sleep until released. */
static pthread_t *held;
static int nheld, held_err;
static sem_t held_up, held_go;

static void *holder(void *arg)
{
	int err = set_deadline(0, PROBE_RUNTIME, PROBE_PERIOD);

	held_err = err;
	if (sem_post(&held_up))
		ksft_print_msg("sem_post: %s\n", strerror(errno));
	if (!err)
		sem_wait_sig(&held_go);
	return NULL;
}

/* Starts @n more holders: 0 once they are all admitted, or -1. */
static int hold(int n)
{
	int max = 16 * ncpus + 16, err;
	pthread_attr_t a;

	if (!held) {
		held = calloc(max, sizeof(*held));
		if (!held || sem_init(&held_up, 0, 0) || sem_init(&held_go, 0, 0)) {
			ksft_print_msg("the holders cannot be set up\n");
			return -1;
		}
	}
	err = pthread_attr_init(&a);
	if (!err)
		err = pthread_attr_setstacksize(&a, THREAD_STACK);
	if (err) {
		ksft_print_msg("pthread_attr: %s\n", strerror(err));
		return -1;
	}
	for (; n > 0; n--) {
		if (nheld == max || pthread_create(&held[nheld], &a, holder, NULL)) {
			ksft_print_msg("a holder thread could not start\n");
			return -1;
		}
		if (sem_wait_for(&held_up, 10000)) {
			ksft_print_msg("a holder did not answer\n");
			return -1;
		}
		if (held_err) {
			join(held[nheld]);
			return -1;
		}
		nheld++;
	}
	return 0;
}

static void release_held(void)
{
	int i;

	for (i = 0; i < nheld; i++)
		if (sem_post(&held_go))
			ksft_print_msg("sem_post: %s\n", strerror(errno));
	for (i = 0; i < nheld; i++)
		join(held[i]);
	nheld = 0;
	sleep_ns(3 * PERIOD);	/* their 0-lag times */
}

/*
 * The fair server of P's CPU, changed at runtime, takes from P's root
 * domain what deadline tasks then cannot: from 10% every second, P admits
 * two probe tasks fewer with 30%, one fewer with 100ms every 500ms, one
 * more with the server off. A change P has no room for is refused.
 */
static int server_params(void)
{
	static const struct {
		uint64_t runtime, period;
		int more;
		const char *what;
	} step[] = {
		{ 3 * SERVER_RUNTIME, SERVER_PERIOD, -2, "a fair server of 30%" },
		{ SERVER_RUNTIME, SERVER_PERIOD / 2, -1, "a fair server of 100ms every 500ms" },
		{ 0, SERVER_PERIOD, 1, "the fair server off" },
		{ SERVER_RUNTIME, SERVER_PERIOD, 0, "the fair server back to 10%" },
	};
	int cpu = ncpus - 1, base, err, i, ret = KSFT_PASS;
	uint64_t runtime;

	if (server_set(cpu, SERVER_RUNTIME, SERVER_PERIOD)) {
		ksft_print_msg("the fair server of CPU %d cannot be set\n", cpu);
		return KSFT_SKIP;
	}
	base = probe(NULL);
	if (base < 2) {
		ksft_print_msg("no room in P for two probe tasks\n");
		return KSFT_SKIP;
	}
	for (i = 0; i < (int)ARRAY_SIZE(step); i++) {
		err = server_set(cpu, step[i].runtime, step[i].period);
		if (err) {
			ksft_print_msg("%s: %s\n", step[i].what, strerror(-err));
			return KSFT_FAIL;
		}
		if (admits(base + step[i].more, step[i].what))
			return KSFT_FAIL;
	}
	/*
	 * With base - 1 probe tasks held, P has room for 10% more and less
	 * than 20%: the server may grow to 20%, not to 30%.
	 */
	if (hold(base - 1)) {
		release_held();
		return KSFT_FAIL;
	}
	err = server_put(cpu, "runtime", 2 * SERVER_RUNTIME);
	if (err) {
		ksft_print_msg("a fair server of 20%% with room for it: %s\n", strerror(-err));
		ret = KSFT_FAIL;
	} else {
		err = server_put(cpu, "runtime", 3 * SERVER_RUNTIME);
		if (err != -EBUSY) {
			ksft_print_msg("a fair server of 30%% with no room for it: %s, not EBUSY\n",
				       err ? strerror(-err) : "accepted");
			ret = KSFT_FAIL;
		} else if (server_get(cpu, "runtime", &runtime) ||
			   runtime != 2 * SERVER_RUNTIME) {
			ksft_print_msg("a refused change changed the fair server\n");
			ret = KSFT_FAIL;
		}
	}
	if (server_set(cpu, SERVER_RUNTIME, SERVER_PERIOD))
		ret = KSFT_FAIL;
	release_held();
	if (ret == KSFT_PASS && admits(base, "after a refused change of the fair server"))
		ret = KSFT_FAIL;
	return ret;
}

/*
 * The rebuilds of the root domains' accounting put the fair servers'
 * bandwidth back as it is set, not as it was at boot: P's own, when a
 * partition root Q is made and unmade beside it; that of the other CPUs,
 * when P is unmade and its CPU joins them with its server; P's again,
 * when it is made anew.
 */
static int server_rebuilds(void)
{
	int cpu = ncpus - 1, base, n10, n30;

	if (server_set(cpu, SERVER_RUNTIME, SERVER_PERIOD)) {
		ksft_print_msg("the fair server of CPU %d cannot be set\n", cpu);
		return KSFT_SKIP;
	}
	base = probe(cg_p);
	if (base < 2) {
		ksft_print_msg("no room in P for two probe tasks\n");
		return KSFT_SKIP;
	}
	if (server_set(cpu, 3 * SERVER_RUNTIME, SERVER_PERIOD) ||
	    admits_in(cg_p, base - 2, "P, a fair server of 30%"))
		return KSFT_FAIL;
	if (make_partition(cg_q, ncpus - 2)) {
		remove_partition(cg_q);
		ksft_print_msg("no partition root Q beside P\n");
		return KSFT_FAIL;
	}
	if (admits_in(cg_p, base - 2, "P, Q made beside it")) {
		remove_partition(cg_q);
		return KSFT_FAIL;
	}
	remove_partition(cg_q);
	if (admits_in(cg_p, base - 2, "P, Q unmade beside it"))
		return KSFT_FAIL;
	/* P unmade: its CPU and its server join the root domain of the others. */
	remove_p();
	n30 = probe(root);
	if (server_set(cpu, SERVER_RUNTIME, SERVER_PERIOD))
		return KSFT_FAIL;
	n10 = probe(root);
	if (n10 != n30 + 2) {
		ksft_print_msg("P unmade: %d probe tasks, its CPU's server at 30%%, %d at 10%%\n",
			       n30, n10);
		return KSFT_FAIL;
	}
	if (server_set(cpu, 3 * SERVER_RUNTIME, SERVER_PERIOD) || make_p()) {
		ksft_print_msg("P made anew, a fair server of 30%%: failed\n");
		return KSFT_FAIL;
	}
	if (admits_in(cg_p, base - 2, "P made anew, a fair server of 30%") ||
	    server_set(cpu, SERVER_RUNTIME, SERVER_PERIOD) ||
	    admits_in(cg_p, base, "P, the fair server back to 10%"))
		return KSFT_FAIL;
	return KSFT_PASS;
}

/* Waits for @f to be set: a futex in shared memory. */
static void wait_set(atomic_int *f)
{
	while (!*f)
		syscall(SYS_futex, f, FUTEX_WAIT, 0, NULL, NULL, 0);
}

static void set_flag(atomic_int *f, int val)
{
	*f = val;
	syscall(SYS_futex, f, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

/*
 * A process of P that spins while mv_state[4 + @rt] is set, and sleeps
 * otherwise: a fair task, or a real-time one (@rt) that leaves P's CPU
 * for a millisecond every 50ms to the real-time kthreads of that CPU
 * (PREEMPT_RT runs RCU and the timers there).
 */
static pid_t server_client(int rt)
{
	pid_t pid = fork_child(rt ? "a real-time task of P" : "a fair task of P");

	if (!pid) {
		struct sched_param sp = { .sched_priority = 1 };

		if (enter(cg_p, getpid()) ||
		    (rt && sched_setscheduler(0, SCHED_FIFO, &sp)))
			_exit(1);
		mv_state[0]++;
		for (;;) {
			wait_set(&mv_state[4 + rt]);
			spin_ns(rt ? 50 * NSEC_PER_MSEC : NSEC_PER_MSEC);
			if (rt)
				sleep_ns(NSEC_PER_MSEC);
		}
	}
	return pid;
}

/* The share of a CPU, in percent, that process @pid uses over @ns. */
static int proc_share(pid_t pid, uint64_t ns)
{
	struct timespec t0, t1;
	uint64_t wall = now_ns();
	clockid_t c;

	if (clock_getcpuclockid(pid, &c) || clock_gettime(c, &t0))
		return -1;
	sleep_ns(ns);
	if (clock_gettime(c, &t1))
		return -1;
	wall = now_ns() - wall;
	return (int)(100 * ((t1.tv_sec - t0.tv_sec) * NSEC_PER_SEC +
			    t1.tv_nsec - t0.tv_nsec) / wall);
}

#define STARVED_SLACK	8	/* % a starved fair task's share may be off its server's */

/*
 * The fair server of P's CPU changed from outside P in the states a change
 * can find it in: stopped, deferred while a fair task of P runs, deferred
 * while a real-time task starves it, serving it, deferred again after
 * serving it, and deferred while the CPU idles. Each change starts a
 * period of the server, which defers serving a starved task to its
 * zero-laxity time, 0.7s in at 30% and 0.9s in at 10%. Afterwards, the
 * starved task gets the server's bandwidth as set last, and P's free
 * bandwidth is as it was. (Throttled after serving, the server is
 * deferred anew at once: the last of the served task's runtime is charged
 * to it as running in the background. Its idle wait ends in the timer
 * callback that starts it, on a CPU with no tick in idle.)
 */
static int server_states(void)
{
	static const struct {
		int fair, rt, ms;
	} step[] = {
		{ 0, 0, 1500 },	/* stopped: nothing of P ran for a period of the server */
		{ 1, 0, 300 },	/* deferred while the fair task runs */
		{ 1, 1, 300 },	/* deferred, the fair task starved: before 0.9s */
		{ 1, 1, 800 },	/* serving it: after 0.7s */
		{ 1, 1, 1500 },	/* deferred again: served from 0.9s to 1s */
		{ 0, 0, 600 },	/* deferred while the CPU idles */
	};
	int cpu = ncpus - 1, base, i, s30 = -1, s10 = -1, ret = KSFT_PASS;
	pid_t fair, rt;

	if (server_set(cpu, SERVER_RUNTIME, SERVER_PERIOD)) {
		ksft_print_msg("the fair server of CPU %d cannot be set\n", cpu);
		return KSFT_SKIP;
	}
	base = probe(cg_p);
	if (base <= 0) {
		ksft_print_msg("no room in P to probe\n");
		return KSFT_SKIP;
	}
	mv_state[0] = 0;
	set_flag(&mv_state[4], 0);
	set_flag(&mv_state[5], 0);
	fair = server_client(0);
	rt = server_client(1);
	if (fair < 0 || rt < 0 || wait_for(5000, mv_state[0] >= 2)) {
		if (fair >= 0 && rt >= 0)
			ksft_print_msg("a fair or real-time task of P could not start\n");
		ret = KSFT_FAIL;
	}
	/* Even steps set the server to 30%, odd ones back to 10%: its runtime alone. */
	for (i = 0; i < (int)ARRAY_SIZE(step) && ret == KSFT_PASS; i++) {
		set_flag(&mv_state[4], step[i].fair);
		set_flag(&mv_state[5], step[i].rt);
		sleep_ns(step[i].ms * NSEC_PER_MSEC);
		if (server_put(cpu, "runtime", (i & 1 ? 1 : 3) * SERVER_RUNTIME)) {
			ksft_print_msg("change %d of the fair server refused\n", i + 1);
			ret = KSFT_FAIL;
		}
	}
	if (ret == KSFT_PASS) {
		set_flag(&mv_state[4], 1);
		set_flag(&mv_state[5], 1);
		if (server_put(cpu, "runtime", 3 * SERVER_RUNTIME))
			ret = KSFT_FAIL;
		s30 = proc_share(fair, 2 * SERVER_PERIOD);
		if (server_put(cpu, "runtime", SERVER_RUNTIME))
			ret = KSFT_FAIL;
		s10 = proc_share(fair, 2 * SERVER_PERIOD);
	}
	set_flag(&mv_state[5], 0);
	set_flag(&mv_state[4], 0);
	reap(rt);
	reap(fair);
	if (ret == KSFT_PASS &&
	    (s30 < 30 - STARVED_SLACK || s30 > 30 + STARVED_SLACK ||
	     s10 < 10 - STARVED_SLACK || s10 > 10 + STARVED_SLACK)) {
		ksft_print_msg("a starved fair task got %d%% of a server of 30%%, %d%% of 10%%\n",
			       s30, s10);
		ret = KSFT_FAIL;
	}
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS && admits_in(cg_p, base, "after the fair server's changes"))
		ret = KSFT_FAIL;
	return ret;
}

/*
 * A reclaiming deadline task of P gets the share of P's CPU its root
 * domain leaves free, which P's fair server takes from: less once the
 * server grows, as much after a rebuild of the accounting (Q made and
 * unmade), and what it had once the server is back. The server is then
 * put back as the test found it, as P's free bandwidth must be.
 */
static int server_reclaim(void)
{
	int cpu = ncpus - 1, s10, s30, rebuilt, back, ret = KSFT_PASS;
	pthread_t t;

	if (server_set(cpu, SERVER_RUNTIME, SERVER_PERIOD)) {
		ksft_print_msg("the fair server of CPU %d cannot be set\n", cpu);
		return KSFT_SKIP;
	}
	if (start_spinner(&t, 10 * NSEC_PER_MSEC, SCHED_FLAG_RECLAIM)) {
		ksft_print_msg("no room for a task of 10%%\n");
		return KSFT_SKIP;
	}
	s10 = share_of(t, SHARE_NS);
	if (server_set(cpu, 3 * SERVER_RUNTIME, SERVER_PERIOD))
		ret = KSFT_FAIL;
	s30 = share_of(t, SHARE_NS);
	if (make_partition(cg_q, ncpus - 2))
		ret = KSFT_FAIL;
	remove_partition(cg_q);
	rebuilt = share_of(t, SHARE_NS);
	if (server_set(cpu, SERVER_RUNTIME, SERVER_PERIOD))
		ret = KSFT_FAIL;
	back = share_of(t, SHARE_NS);
	stop_spinner(t);
	server_restore();
	if (ret == KSFT_PASS &&
	    (s30 > s10 - SHARE_SLACK ||
	     rebuilt < s30 - SHARE_SLACK || rebuilt > s30 + SHARE_SLACK ||
	     back < s10 - SHARE_SLACK || back > s10 + SHARE_SLACK)) {
		ksft_print_msg("reclaiming: %d%%, %d%% at 30%%, %d%% rebuilt, %d%% back at 10%%\n",
			       s10, s30, rebuilt, back);
		ret = KSFT_FAIL;
	}
	return ret;
}

/* Parameter changes while a task wakes on another CPU */

#define WAKE_NS		(5 * NSEC_PER_SEC)	/* how long the changes go on */
#define WAKE_BURST	2000			/* changes between two clock readings */

/* this_bw and running_bw of @cpu, from sched_debug: 0, or -1. */
static int rq_bw_of(int cpu, long *this_bw, long *running_bw)
{
	char line[256], want[32];
	FILE *f = fopen("/sys/kernel/debug/sched/debug", "r");
	int in = 0, got = 0;

	if (!f)
		return -1;
	snprintf(want, sizeof(want), "dl_rq[%d]:", cpu);
	while (got < 2 && fgets(line, sizeof(line), f)) {
		char *val = strchr(line, ':');

		if (!strncmp(line, want, strlen(want)))
			in = 1;
		else if (!in || !val)
			continue;
		else if (strstr(line, ".running_bw") && sscanf(val + 1, "%ld", running_bw) == 1)
			got++;
		else if (strstr(line, ".this_bw") && sscanf(val + 1, "%ld", this_bw) == 1)
			got++;
	}
	fclose(f);
	return got == 2 ? 0 : -1;
}

/*
 * A deadline task of Q, a partition root of two CPUs taken by two
 * deadline tasks of an earlier deadline, that sleeps and wakes: it wakes
 * on whichever of the two has the later deadline to run, which moves it.
 * Its parameters change meanwhile, from a CPU outside Q.
 *
 * A wakeup that moves a task takes its bandwidth off the runqueue it
 * slept on, and the enqueue on the one it wakes on, which that CPU may
 * be left to do, charges it there. A change in between must not take it
 * off the runqueue of either: once every task of the case is gone,
 * this_bw of both CPUs is what it was before.
 *
 * Only then, with nothing left to change: sched_debug prints running_bw
 * and this_bw of a CPU one after the other, holding no lock in between,
 * and every parameter change takes a task's bandwidth off its runqueue
 * and puts it back, so a reading of the two while the changes go on
 * straddles one of those and tells nothing.
 */
static atomic_int *wk_state;		/* 0: tasks up, 1: wakeups, 2: stop */

static pid_t waking_task(uint64_t runtime, uint64_t period, int sleeps)
{
	pid_t pid = fork_child("a waking deadline task");

	if (!pid) {
		if (enter(cg_q, getpid()) || set_deadline(0, runtime, period)) {
			wk_state[0] = -1;
			_exit(1);
		}
		wk_state[0]++;
		for (;;) {
			if (wk_state[2])
				_exit(0);
			spin_ns(sleeps ? 20 * NSEC_PER_USEC : NSEC_PER_MSEC);
			if (sleeps) {
				sleep_ns(80 * NSEC_PER_USEC);
				wk_state[1]++;
			}
		}
	}
	return pid;
}

/* Changes the parameters of @pid over and over, until told to stop. */
static pid_t changer_proc(pid_t pid)
{
	pid_t self = fork_child("a changer of the parameters");

	if (!self) {
		while (!wk_state[2]) {
			if (set_deadline(pid, 20 * NSEC_PER_MSEC, PERIOD) ||
			    set_deadline(pid, 10 * NSEC_PER_MSEC, PERIOD))
				break;
		}
		_exit(0);
	}
	return self;
}

static int params_waking(void)
{
	int cpu_a = ncpus - 3, cpu_b = ncpus - 2, i, ret = KSFT_PASS, changes = 0;
	long ta, ra, tb, rb, ta2, ra2, tb2, rb2;
	pid_t pid[3] = { -1, -1, -1 }, changer = -1;
	uint64_t end;
	char cpus[32];

	if (ncpus < 4) {
		ksft_print_msg("needs 4 CPUs\n");
		return KSFT_SKIP;
	}
	snprintf(cpus, sizeof(cpus), "%d-%d", cpu_a, cpu_b);
	if (make_partition_cpus(cg_q, cpus)) {
		remove_partition(cg_q);
		ksft_print_msg("no partition root of two CPUs\n");
		return KSFT_SKIP;
	}
	if (rq_bw_of(cpu_a, &ta, &ra) || rq_bw_of(cpu_b, &tb, &rb)) {
		remove_partition(cg_q);
		ksft_print_msg("sched_debug tells of neither CPU's this_bw\n");
		return KSFT_SKIP;
	}
	wk_state = mv_state + 8;
	wk_state[0] = wk_state[1] = wk_state[2] = 0;
	/* Two of 40% hold both CPUs with an earlier deadline than the third. */
	pid[0] = waking_task(20 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC, 0);
	pid[1] = waking_task(20 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC, 0);
	pid[2] = waking_task(10 * NSEC_PER_MSEC, PERIOD, 1);
	if (pid[0] < 0 || pid[1] < 0 || pid[2] < 0) {
		ret = KSFT_FAIL;
		goto out;
	}
	wait_for(5000, wk_state[0] >= 3 || wk_state[0] < 0);
	if (wk_state[0] != 3) {
		ksft_print_msg("no room for the deadline tasks of a partition of two CPUs\n");
		ret = KSFT_SKIP;
		goto out;
	}
	changer = changer_proc(pid[2]);
	if (changer < 0) {
		ret = KSFT_FAIL;
		goto out;
	}
	end = now_ns() + WAKE_NS;
	while (now_ns() < end) {
		for (i = 0; i < WAKE_BURST; i++, changes += 2) {
			if (set_deadline(pid[2], 20 * NSEC_PER_MSEC, PERIOD) ||
			    set_deadline(pid[2], 10 * NSEC_PER_MSEC, PERIOD)) {
				ksft_print_msg("a change of the waking task refused\n");
				ret = KSFT_FAIL;
				goto out;
			}
		}
	}
	if (!wk_state[1]) {
		ksft_print_msg("the waking task never woke\n");
		ret = KSFT_FAIL;
	}
out:
	wk_state[2] = 1;
	reap(changer);
	for (i = 0; i < 3; i++)
		reap(pid[i]);
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS) {
		if (rq_bw_of(cpu_a, &ta2, &ra2) || rq_bw_of(cpu_b, &tb2, &rb2)) {
			ret = KSFT_FAIL;
		} else if (ta2 != ta || tb2 != tb) {
			ksft_print_msg("%d changes: this_bw %ld and %ld, was %ld and %ld\n",
				       changes, ta2, tb2, ta, tb);
			ret = KSFT_FAIL;
		}
	}
	remove_partition(cg_q);
	return ret;
}

/* A random mix of the transitions above */

#define MIX_WORKERS	6
#define MIX_NS		(3 * NSEC_PER_SEC)
#define MIX_SEED	20261103	/* fixed, so that a failure can be replayed */

/*
 * Each worker takes a random transition of its own over and over: into
 * SCHED_DEADLINE with random parameters and flags, out of it, asleep,
 * spinning past its runtime, through sched_yield(), through the shared
 * mutex of the priority inheritance cases, and out of life. The case
 * takes the ones only another task can: new parameters, a move between
 * the cgroups, a kill, and the fair server of a CPU. Every worker it
 * finds dead it starts anew, so the mix keeps its tasks.
 *
 * Nothing here looks at what a single transition does, which the cases
 * above do one at a time. What it wants is the accounting whole
 * afterwards: once every worker is gone and its 0-lag time is past, each
 * of the three root domains admits what it admitted before, and this_bw
 * of the three CPUs is what it was. The kernel's own warnings of
 * running_bw and this_bw over- and underflow count as well, which the
 * suite's dmesg tells of.
 */
static atomic_int *mix_state;		/* 0: stop, 1 + i: worker i's rounds */

static pid_t mix_worker(int idx, unsigned int seed)
{
	static const uint64_t runtime[] = { 5, 10, 20, 30 };
	static const uint64_t period[] = { 50, 100 };
	static const uint64_t flag[] = { 0, SCHED_FLAG_RECLAIM, SCHED_FLAG_DL_OVERRUN };
	pid_t pid = fork_child("a worker of the mix");

	if (!pid) {
		struct sigaction sa = { .sa_handler = catch_xcpu };
		int err;

		if (sigaction(SIGXCPU, &sa, NULL))
			_exit(1);
		seed += idx;
		/*
		 * A refusal is part of the mix: each call below is made in
		 * whatever state the worker is in, and an error is what the
		 * kernel is entitled to answer.
		 */
		while (!mix_state[0]) {
			unsigned int r = rand_r(&seed), v = r / 8;

			switch (r % 8) {
			case 0:
				set_deadline_flags(0, runtime[v % 4] * NSEC_PER_MSEC,
						   period[(v / 4) % 2] * NSEC_PER_MSEC,
						   flag[(v / 8) % 3]);
				break;
			case 1:
				set_other(0);
				break;
			case 2:
				sleep_ns((v % 20 + 1) * NSEC_PER_MSEC);
				break;
			case 3:
				spin_ns((v % 15 + 1) * NSEC_PER_MSEC);
				break;
			case 4:
				sched_yield();
				break;
			case 5:
				err = pthread_mutex_trylock(shm_lock);
				if (err == EOWNERDEAD)	/* its owner died holding it */
					err = pthread_mutex_consistent(shm_lock);
				if (!err) {
					spin_ns(NSEC_PER_MSEC);
					if (pthread_mutex_unlock(shm_lock))
						_exit(1);
				}
				break;
			case 6:
				_exit(0);	/* the case starts another */
			default:
				sleep_ns(NSEC_PER_MSEC / 2);
				break;
			}
			mix_state[1 + idx]++;
		}
		_exit(0);
	}
	return pid;
}

static int random_mix(void)
{
	static const uint64_t runtime[] = { 5, 10, 20, 30 };
	int cpu_a = ncpus - 3, cpu_b = ncpus - 2, cpu_p = ncpus - 1;
	int base_p, base_q, base_r, i, ret = KSFT_PASS, rounds = 0, sums;
	long ta = 0, ra, tb = 0, rb, tp = 0, rp, ta2, ra2, tb2, rb2, tp2, rp2;
	pid_t pid[MIX_WORKERS] = { -1, -1, -1, -1, -1, -1 };
	unsigned int seed = MIX_SEED;
	uint64_t end;
	char cpus[32];

	if (ncpus < 4) {
		ksft_print_msg("needs 4 CPUs\n");
		return KSFT_SKIP;
	}
	snprintf(cpus, sizeof(cpus), "%d-%d", cpu_a, cpu_b);
	if (make_partition_cpus(cg_q, cpus)) {
		remove_partition(cg_q);
		ksft_print_msg("no partition root of two CPUs\n");
		return KSFT_SKIP;
	}
	if (shm_lock_init()) {
		remove_partition(cg_q);
		return KSFT_SKIP;	/* no shared mutex for the workers */
	}
	/* Without the two sums of sched_debug, the admissions alone tell. */
	sums = !rq_bw_of(cpu_a, &ta, &ra) && !rq_bw_of(cpu_b, &tb, &rb) &&
	       !rq_bw_of(cpu_p, &tp, &rp);
	base_p = probe(cg_p);
	base_q = probe(cg_q);
	base_r = probe(root);
	if (base_p <= 0 || base_q <= 0 || base_r <= 0) {
		remove_partition(cg_q);
		ksft_print_msg("no room to probe P, Q or the rest\n");
		return KSFT_SKIP;
	}
	mix_state = mv_state + 16;
	for (i = 0; i <= MIX_WORKERS; i++)
		mix_state[i] = 0;
	for (i = 0; i < MIX_WORKERS; i++) {
		pid[i] = mix_worker(i, seed);
		if (pid[i] < 0) {
			ret = KSFT_FAIL;
			goto out;
		}
	}

	end = now_ns() + MIX_NS;
	while (now_ns() < end) {
		unsigned int r = rand_r(&seed);
		int w = r % MIX_WORKERS;

		switch ((r / MIX_WORKERS) % 7) {
		case 0:
			set_deadline(pid[w], runtime[r % 4] * NSEC_PER_MSEC, PERIOD);
			break;
		case 1:
			set_other(pid[w]);
			break;
		case 2:
			enter(cg_p, pid[w]);
			break;
		case 3:
			enter(cg_q, pid[w]);
			break;
		case 4:
			enter(root, pid[w]);
			break;
		case 5:
			/* Dead and reaped already: ESRCH, and it starts anew below. */
			if (kill(pid[w], SIGKILL) && errno != ESRCH)
				ksft_print_msg("kill %d: %s\n", pid[w], strerror(errno));
			break;
		default:
			server_set(r % 2 ? cpu_p : cpu_a,
				   (r % 3 + 1) * SERVER_RUNTIME, SERVER_PERIOD);
			break;
		}
		sleep_ns((r % 2000 + 100) * NSEC_PER_USEC);
		/* Whoever died, of its own or killed, runs again. */
		for (i = 0; i < MIX_WORKERS; i++) {
			pid_t done = waitpid(pid[i], NULL, WNOHANG);

			if (done < 0 && errno != EINTR) {
				ksft_print_msg("waitpid %d: %s\n", pid[i], strerror(errno));
				ret = KSFT_FAIL;
				goto out;
			}
			if (done == pid[i]) {
				pid[i] = mix_worker(i, seed + rounds);
				if (pid[i] < 0) {
					ret = KSFT_FAIL;
					goto out;
				}
				rounds++;
			}
		}
	}
out:
	mix_state[0] = 1;
	for (i = 0; i < MIX_WORKERS; i++)
		reap(pid[i]);
	sleep_ns(5 * PERIOD);		/* their 0-lag times */
	server_restore();		/* which needs the room they held */
	if (ret != KSFT_PASS) {
		remove_partition(cg_q);
		return ret;
	}
	for (i = 0, rounds = 0; i < MIX_WORKERS; i++)
		rounds += mix_state[1 + i];
	if (!rounds) {
		ksft_print_msg("the workers of the mix took no transition\n");
		ret = KSFT_FAIL;
	}
	if (ret == KSFT_PASS && sums) {
		if (rq_bw_of(cpu_a, &ta2, &ra2) || rq_bw_of(cpu_b, &tb2, &rb2) ||
		    rq_bw_of(cpu_p, &tp2, &rp2)) {
			ret = KSFT_FAIL;
		} else if (ta2 != ta || tb2 != tb || tp2 != tp) {
			ksft_print_msg("this_bw %ld %ld %ld, was %ld %ld %ld\n",
				       ta2, tb2, tp2, ta, tb, tp);
			ret = KSFT_FAIL;
		}
	}
	if (ret == KSFT_PASS &&
	    (admits_in(cg_p, base_p, "P after the mix") ||
	     admits_in(cg_q, base_q, "Q after the mix") ||
	     admits_in(root, base_r, "the root domain of the rest after the mix"))) {
		ksft_print_msg("seed %u, %d transitions\n", MIX_SEED, rounds);
		ret = KSFT_FAIL;
	}
	remove_partition(cg_q);
	return ret;
}

/* CPU hotplug, from the main process, with no P */

static int hp_cpu = -1;

/* Whether the online file of @cpu can be written, as hp_cpu's can. */
static int cpu_hotpluggable(int cpu)
{
	char path[64];

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu);
	return !access(path, W_OK);
}

static int cpu_online(int cpu, int on)
{
	char dir[64];

	snprintf(dir, sizeof(dir), "/sys/devices/system/cpu/cpu%d", cpu);
	return write_file(dir, "online", on ? "1" : "0");
}

/* Back online: every case that follows needs that CPU. */
static void cpu_up(int cpu)
{
	int err = cpu_online(cpu, 1);

	if (err)
		ksft_print_msg("CPU %d cannot come back online: %s\n", cpu, strerror(-err));
}

/* Deadline sleepers of 30% each: offline and online a CPU meanwhile. */
static int hotplug_sleepers(void)
{
	int base = probe(NULL), after, i, ret = KSFT_PASS;
	pid_t pid[4];

	for (i = 0; i < 4; i++) {
		mv_state[0] = 0;
		pid[i] = mover(MV_ASLEEP, 30 * NSEC_PER_MSEC);
		if (pid[i] < 0 || child_up("a deadline sleeper", 0)) {
			while (i >= 0)
				reap(pid[i--]);
			return KSFT_FAIL;
		}
	}
	if (cpu_online(hp_cpu, 0) || cpu_online(hp_cpu, 1)) {
		ksft_print_msg("offline and online of CPU %d with deadline sleepers failed\n",
			       hp_cpu);
		ret = KSFT_FAIL;
		cpu_up(hp_cpu);
	}
	for (i = 0; i < 4; i++)
		reap(pid[i]);
	sleep_ns(3 * PERIOD);
	after = probe(NULL);
	if (ret == KSFT_PASS && after != base) {
		ksft_print_msg("hotplug with sleepers: %d probe tasks admitted before, %d after\n",
			       base, after);
		ret = KSFT_FAIL;
	}
	return ret;
}

/* The deadline tasks fill the CPUs: going offline must be refused. */
static int hotplug_refused(void)
{
	int base = probe(NULL), n, i, ret = KSFT_PASS, err;
	pid_t *pid;

	if (base < 12) {
		ksft_print_msg("needs room for 12 probe tasks\n");
		return KSFT_SKIP;
	}
	/* 30% each, up to the free bandwidth but 30% to 60%: less than a CPU's. */
	n = base / 3 - 1;
	pid = calloc(n, sizeof(*pid));
	if (!pid) {
		ksft_print_msg("calloc: %s\n", strerror(errno));
		return KSFT_FAIL;
	}
	for (i = 0; i < n; i++) {
		mv_state[0] = 0;
		pid[i] = mover(MV_ASLEEP, 30 * NSEC_PER_MSEC);
		if (pid[i] < 0 || child_up("a deadline sleeper", 0)) {
			while (i >= 0)
				reap(pid[i--]);
			free(pid);
			return KSFT_FAIL;
		}
	}
	err = cpu_online(hp_cpu, 0);
	if (!err) {
		ksft_print_msg("CPU %d went offline with its deadline bandwidth in use\n", hp_cpu);
		cpu_up(hp_cpu);
		ret = KSFT_FAIL;
	} else if (err != -EBUSY) {
		ksft_print_msg("offline refused with %s, not EBUSY\n", strerror(-err));
		ret = KSFT_FAIL;
	}
	for (i = 0; i < n; i++)
		reap(pid[i]);
	free(pid);
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS && probe(NULL) != base) {
		ksft_print_msg("a refused offline changed the free bandwidth\n");
		ret = KSFT_FAIL;
	}
	return ret;
}

/*
 * A deadline task leaves the class while it runs, its 0-lag time ahead,
 * and the CPU it is on goes offline before that time.
 */
static int hotplug_departed(void)
{
	int base = probe(NULL), base_off, after, cpu, ret = KSFT_PASS;
	pid_t pid;

	/*
	 * What the root domain admits with the CPU offline and no deadline
	 * task of ours: the release of the departed task's bandwidth
	 * happens there, and the online that follows would rebuild the
	 * accounting over it.
	 */
	if (cpu_online(hp_cpu, 0)) {
		ksft_print_msg("offline of CPU %d refused\n", hp_cpu);
		return KSFT_FAIL;
	}
	base_off = probe(NULL);
	cpu_up(hp_cpu);
	if (base <= 0 || base_off <= 0) {
		ksft_print_msg("no room to probe with the CPU online or offline\n");
		return KSFT_SKIP;
	}
	mv_state[0] = 0;
	mv_state[1] = 0;
	pid = mover(MV_THROTTLED, 40 * NSEC_PER_MSEC);
	if (pid < 0 || child_up("a departing deadline task", 0)) {
		reap(pid);
		return KSFT_FAIL;
	}
	sleep_ns(PERIOD / 2);
	/* Running and queued: its bandwidth stays until its 0-lag time. */
	if (set_other(pid))
		ret = KSFT_FAIL;
	cpu = task_cpu_of(pid);
	if (cpu <= 0 || !cpu_hotpluggable(cpu)) {
		cpu_set_t set;

		/* Out of the class now: it may be moved to the CPU we offline. */
		cpu = hp_cpu;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(pid, sizeof(set), &set))
			ret = KSFT_FAIL;
	}
	if (ret == KSFT_PASS && cpu_online(cpu, 0)) {
		ksft_print_msg("offline of CPU %d refused\n", cpu);
		ret = KSFT_FAIL;
	}
	sleep_ns(3 * PERIOD);		/* its 0-lag time, the CPU offline */
	if (ret == KSFT_PASS) {
		after = probe(NULL);
		if (after != base_off) {
			ksft_print_msg("departed, CPU %d offline: %d admitted, %d expected\n",
				       cpu, after, base_off);
			ret = KSFT_FAIL;
		}
	}
	cpu_up(cpu);
	reap(pid);
	sleep_ns(3 * PERIOD);
	after = probe(NULL);
	if (ret == KSFT_PASS && after != base) {
		ksft_print_msg("departed, CPU back online: %d admitted before, %d after\n",
			       base, after);
		ret = KSFT_FAIL;
	}
	return ret;
}

/*
 * The fair server of the CPU set to 30%: its bandwidth leaves the root
 * domain with the CPU, as it does at 10%, and comes back with the CPU as
 * it was set.
 */
static int hotplug_server(void)
{
	int base, off10, off30;

	if (server_set(hp_cpu, SERVER_RUNTIME, SERVER_PERIOD)) {
		ksft_print_msg("the fair server of CPU %d cannot be set\n", hp_cpu);
		return KSFT_SKIP;
	}
	base = probe(NULL);
	if (cpu_online(hp_cpu, 0)) {
		ksft_print_msg("offline of CPU %d refused\n", hp_cpu);
		return KSFT_FAIL;
	}
	off10 = probe(NULL);
	cpu_up(hp_cpu);
	if (server_set(hp_cpu, 3 * SERVER_RUNTIME, SERVER_PERIOD) ||
	    admits(base - 2, "a fair server of 30%"))
		return KSFT_FAIL;
	if (cpu_online(hp_cpu, 0)) {
		ksft_print_msg("offline of CPU %d refused, its fair server at 30%%\n", hp_cpu);
		return KSFT_FAIL;
	}
	off30 = probe(NULL);
	cpu_up(hp_cpu);
	if (off30 != off10) {
		ksft_print_msg("CPU %d offline: %d probe tasks, its server at 10%%, %d at 30%%\n",
			       hp_cpu, off10, off30);
		return KSFT_FAIL;
	}
	if (admits(base - 2, "a fair server of 30%, its CPU back online") ||
	    server_set(hp_cpu, SERVER_RUNTIME, SERVER_PERIOD) ||
	    admits(base, "the fair server back to 10%"))
		return KSFT_FAIL;
	return KSFT_PASS;
}

/*
 * Going offline, a CPU takes its fair server's bandwidth along with its
 * capacity. With the server at the admission limit less 10%, the CPU goes
 * offline with as many probe tasks held as the root domain admits but
 * one, and no longer with one more.
 */
static int hotplug_server_refused(void)
{
	uint64_t rt_runtime, rt_period, runtime;
	int n, err, ret = KSFT_PASS;
	char buf[32];

	if (read_file("/proc/sys/kernel/sched_rt_runtime_us", buf, sizeof(buf)) ||
	    read_file("/proc/sys/kernel/sched_rt_period_us", buf, sizeof(buf))) {
		ksft_print_msg("no sched_rt_runtime_us or sched_rt_period_us\n");
		return KSFT_SKIP;
	}
	read_file("/proc/sys/kernel/sched_rt_runtime_us", buf, sizeof(buf));
	rt_runtime = atoll(buf);
	read_file("/proc/sys/kernel/sched_rt_period_us", buf, sizeof(buf));
	rt_period = atoll(buf);
	if (!rt_period || 5 * rt_runtime < rt_period) {
		ksft_print_msg("the admission limit is below a fifth of a CPU\n");
		return KSFT_SKIP;
	}
	runtime = SERVER_PERIOD * rt_runtime / rt_period - SERVER_PERIOD / 10;
	if (server_set(hp_cpu, runtime, SERVER_PERIOD)) {
		ksft_print_msg("the fair server of CPU %d cannot be set\n", hp_cpu);
		return KSFT_SKIP;
	}
	n = probe(NULL);
	if (n < 2 || hold(n - 1)) {
		release_held();
		return KSFT_FAIL;
	}
	err = cpu_online(hp_cpu, 0);
	if (err) {
		ksft_print_msg("CPU %d, room for one probe task left: offline refused (%s)\n",
			       hp_cpu, strerror(-err));
		ret = KSFT_FAIL;
	} else if (cpu_online(hp_cpu, 1) || hold(1)) {
		cpu_up(hp_cpu);
		ret = KSFT_FAIL;
	} else {
		err = cpu_online(hp_cpu, 0);
		if (err != -EBUSY) {
			ksft_print_msg("CPU %d, no room left: offline %s, not EBUSY\n",
				       hp_cpu, err ? strerror(-err) : "accepted");
			if (!err)
				cpu_up(hp_cpu);
			ret = KSFT_FAIL;
		}
	}
	release_held();
	return ret;
}

/* Setup */

static int find_cgroup2(void)
{
	char line[PATH_MAX + 64], dev[64], dir[PATH_MAX], type[64];
	FILE *f = fopen("/proc/self/mounts", "r");
	int found = 0;

	while (f && !found && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%63s %4095s %63s", dev, dir, type) == 3 &&
		    !strcmp(type, "cgroup2")) {
			snprintf(root, sizeof(root), "%s", dir);
			found = 1;
		}
	}
	if (f)
		fclose(f);
	return found ? 0 : -1;
}

static void report(const char *name, int ret)
{
	if (ret == KSFT_PASS)
		ksft_test_result_pass("%s\n", name);
	else if (ret == KSFT_SKIP)
		ksft_test_result_skip("%s\n", name);
	else
		ksft_test_result_fail("%s\n", name);
}

enum where {
	MEASURED_IN_P,	/* in P, measured before and after */
	IN_P,		/* in P, measuring itself */
	FROM_OUTSIDE,	/* from the main process */
};

static const struct p_case {
	const char *name;
	int (*fn)(void);
	enum where where;
} p_cases[] = {
	{ "pi_deadline_owner", pi_deadline_owner, MEASURED_IN_P },
	{ "pi_other_owner_asleep", pi_other_owner_asleep, MEASURED_IN_P },
	{ "pi_former_owner_asleep", pi_former_owner_asleep, MEASURED_IN_P },
	{ "pi_unlock_running", pi_unlock_running, MEASURED_IN_P },
	{ "params_asleep", params_asleep, IN_P },
	{ "params_non_contending", params_non_contending, MEASURED_IN_P },
	{ "params_throttled", params_throttled, MEASURED_IN_P },
	{ "params_refused", params_refused, IN_P },
	{ "params_keep_class_change", params_keep_class_change, MEASURED_IN_P },
	{ "params_keep_boosted", params_keep_boosted, MEASURED_IN_P },
	{ "move_asleep", move_asleep, FROM_OUTSIDE },
	{ "move_throttled", move_throttled, FROM_OUTSIDE },
	{ "move_non_contending", move_non_contending, FROM_OUTSIDE },
	{ "move_refused", move_refused, FROM_OUTSIDE },
	{ "death_running", death_running, MEASURED_IN_P },
	{ "death_switched_out", death_switched_out, MEASURED_IN_P },
	{ "death_shrunk", death_shrunk, MEASURED_IN_P },
	{ "death_killed_asleep", death_killed_asleep, MEASURED_IN_P },
	{ "death_killed_non_contending", death_killed_non_contending, MEASURED_IN_P },
	{ "death_killed_throttled", death_killed_throttled, MEASURED_IN_P },
	{ "death_killed_running", death_killed_running, MEASURED_IN_P },
	{ "death_pi_waiter", death_pi_waiter, MEASURED_IN_P },
	{ "death_pi_owner", death_pi_owner, MEASURED_IN_P },
	{ "death_pi_deadline_owner", death_pi_deadline_owner, MEASURED_IN_P },
	{ "death_group", death_group, MEASURED_IN_P },
	{ "yield_switched_out", yield_switched_out, IN_P },
	{ "yield_moved", yield_moved, FROM_OUTSIDE },
	{ "yield_killed", yield_killed, MEASURED_IN_P },
	{ "overrun_caught", overrun_caught, IN_P },
	{ "overrun_group_death", overrun_group_death, MEASURED_IN_P },
	{ "reclaim_runs", reclaim_runs, MEASURED_IN_P },
	{ "reclaim_switched", reclaim_switched, MEASURED_IN_P },
	{ "reclaim_moved", reclaim_moved, FROM_OUTSIDE },
	{ "server_params", server_params, IN_P },
	{ "server_rebuilds", server_rebuilds, FROM_OUTSIDE },
	{ "server_states", server_states, FROM_OUTSIDE },
	{ "server_reclaim", server_reclaim, MEASURED_IN_P },
	{ "params_waking", params_waking, FROM_OUTSIDE },
	{ "random_mix", random_mix, FROM_OUTSIDE },
};

#define N_P_CASES	((int)ARRAY_SIZE(p_cases))

static const struct hp_case {
	const char *name;
	int (*fn)(void);
} hp_cases[] = {
	{ "hotplug_sleepers", hotplug_sleepers },
	{ "hotplug_refused", hotplug_refused },
	{ "hotplug_departed", hotplug_departed },
	{ "hotplug_server", hotplug_server },
	{ "hotplug_server_refused", hotplug_server_refused },
};

#define N_HP_CASES	((int)ARRAY_SIZE(hp_cases))

/* The cases in P, each in a P of its own. */
static void run_p_cases(void)
{
	char path[PATH_MAX + 32], buf[256];
	int i, ret, added = 0, told = 0, ok;

	ok = !find_cgroup2() &&
	     snprintf(cg_p, sizeof(cg_p), "%s/deadline_bw_p", root) < (int)sizeof(cg_p) &&
	     snprintf(cg_q, sizeof(cg_q), "%s/deadline_bw_q", root) < (int)sizeof(cg_q);
	if (ok) {
		/* The cpuset controller for the root's children while the cases run. */
		snprintf(path, sizeof(path), "%s/cgroup.subtree_control", root);
		ok = !read_file(path, buf, sizeof(buf));
		added = ok && !strstr(buf, "cpuset");
		if (added && write_file(root, "cgroup.subtree_control", "+cpuset")) {
			ok = 0;
			added = 0;
		}
	}
	for (i = 0; i < N_P_CASES; i++) {
		const struct p_case *c = &p_cases[i];

		if (ok && make_p()) {
			remove_p();
			ok = 0;
		}
		if (!ok) {
			if (!told++)
				ksft_print_msg("no one-CPU partition root: cases in P skipped\n");
			ksft_test_result_skip("%s\n", c->name);
			continue;
		}
		if (c->where == MEASURED_IN_P)
			ret = measured(c->name, cg_p, c->fn);
		else if (c->where == IN_P)
			ret = run_in(cg_p, c->fn);
		else
			ret = c->fn();
		remove_p();
		server_restore();
		report(c->name, ret);
	}
	if (added && write_file(root, "cgroup.subtree_control", "-cpuset"))
		ksft_print_msg("the cpuset controller stays enabled\n");
}

int main(void)
{
	char buf[64];
	int i;

	ksft_print_header();
	if (geteuid())
		ksft_exit_skip("needs root\n");
	ncpus = get_nprocs();
	if (ncpus < 3)
		ksft_exit_skip("needs 3 CPUs\n");
	if (read_file("/proc/sys/kernel/sched_rt_runtime_us", buf, sizeof(buf)) || atol(buf) < 0)
		ksft_exit_skip("no deadline admission control\n");
	mv_state = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (mv_state == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	shm_lock = (pthread_mutex_t *)((char *)mv_state + 256);

	ksft_set_plan(N_P_CASES + N_HP_CASES);
	server_init();
	run_p_cases();

	for (i = ncpus - 1; i > 0 && hp_cpu < 0; i--) {
		if (cpu_hotpluggable(i))
			hp_cpu = i;
	}
	if (hp_cpu < 0)
		ksft_print_msg("no CPU hotplug: hotplug cases skipped\n");
	for (i = 0; i < N_HP_CASES; i++) {
		if (hp_cpu < 0) {
			ksft_test_result_skip("%s\n", hp_cases[i].name);
			continue;
		}
		report(hp_cases[i].name, hp_cases[i].fn());
		server_restore();
	}
	ksft_finished();
}
