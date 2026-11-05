// SPDX-License-Identifier: GPL-2.0
/*
 * Test the bandwidth accounting of SCHED_DEADLINE. The deadline tasks
 * of the cases go through:
 *  - priority inheritance
 *  - parameter changes
 *  - cpuset moves
 *  - deaths, yields and runtime overruns
 *  - bandwidth reclaiming and fair server changes
 *  - CPU hotplug
 *  - a random mix of all of these
 *
 * Most cases measure the free bandwidth of a root domain before and after
 * they run. probe() counts the tasks of 10ms every 100ms that the root
 * domain admits. Once the tasks of the case are gone and their 0-lag time
 * is past, the two counts must be equal. Fewer means leaked bandwidth.
 * More means bandwidth released twice, which lets the root domain admit
 * deadline tasks beyond its capacity. A deadline task that a case keeps
 * running must also make progress.
 *
 * The reclaiming cases measure the share of a CPU that a deadline task
 * with SCHED_FLAG_RECLAIM gets. That share follows extra_bw: the free
 * bandwidth that the task's runqueue can reclaim. probe() follows the
 * root domain's total_bw instead.
 *
 * The CPUs that isolcpus="domain" isolates are in the default root
 * domain. The other CPUs are in another one. The test leaves the
 * isolated CPUs out, and keeps its tasks off them. Only the cases that
 * need them use them.
 *
 * Every case but the hotplug ones runs with P, a partition root of the
 * last online CPU. P is made anew for each case. Making it rebuilds the
 * accounting, which clears what an earlier case left. The hotplug cases
 * run in the root domain of all CPUs. The server cases change the fair
 * server of a CPU through debugfs, and the test then puts it back.
 *
 * The test needs root, deadline admission control and 3 CPUs. Some cases
 * also need the cgroup v2 cpuset controller, debugfs or CPU hotplug. Nine
 * need isolcpus="domain" at boot.
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
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>

#include "../../kselftest.h"

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
/* The online CPUs at the start, ascending, except the isolated ones */
static int ncpus, *online;
static int nisol, *isol;	/* the online CPUs that isolcpus="domain" isolates */
static char isolated[4096];	/* the CPU list of /sys/devices/system/cpu/isolated */

/* Return the @n-th highest online CPU, counting from 1. */
static int top_cpu(int n)
{
	return online[ncpus - n];
}

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
 * Poll @cond every @step ns, for at most @ms. Return 0 once it holds, or
 * -1 on a timeout. The bound makes a child that never started, or was
 * refused SCHED_DEADLINE, fail its case instead of hanging it.
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

/* Wait on @s. Retry when a signal interrupts the wait. */
static void sem_wait_sig(sem_t *s)
{
	while (sem_wait(s)) {
		if (errno == EINTR)
			continue;
		ksft_print_msg("sem_wait: %s\n", strerror(errno));
		return;
	}
}

/*
 * Wait on @s for at most @ms, retrying on signals. Return 0, or -1 on a
 * timeout or an error.
 */
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

/*
 * Fork a child that is killed when the thread that forked it dies, so
 * that it cannot keep its bandwidth. Every caller forks from the main
 * thread of its process. Report a failure. The caller must handle a
 * return of -1.
 */
static pid_t fork_child(const char *what)
{
	pid_t parent = getpid();
	pid_t pid = fork();

	if (pid < 0)
		ksft_print_msg("fork for %s: %s\n", what, strerror(errno));
	else if (!pid && (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent))
		_exit(KSFT_FAIL);
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

/* Make @cg a partition root of the CPUs in @cpus. Return 0, or -1. */
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

/* Make @cg a partition root of CPU @cpu. Return 0, or -1. */
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

/* Make P a partition root of the last online CPU. Return 0, or -1. */
static int make_p(void)
{
	return make_partition(cg_p, top_cpu(1));
}

static void remove_p(void)
{
	remove_partition(cg_p);
}

/*
 * Return field @n of /proc/@pid/stat, or @fail. Fields are numbered as
 * in proc(5), and @n must be 3 or more: a field after the command name.
 */
static int stat_field(pid_t pid, int n, int fail)
{
	char path[64], buf[1024], *p;
	int i;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	if (read_file(path, buf, sizeof(buf)))
		return fail;
	p = strrchr(buf, ')');
	for (i = 0; p && i < n - 2; i++)
		p = strchr(p + 1, ' ');
	return p ? atoi(p + 1) : fail;
}

/* Return the CPU @pid last ran on, from /proc, or -1. */
static int task_cpu_of(pid_t pid)
{
	return stat_field(pid, 39, -1);
}

/* The priority /proc shows for a deadline task, or a task boosted by one. */
#define DL_TASK_PRIO	(-101)

/* Return the priority @pid runs at, from /proc, or 0. */
static int prio_of(pid_t pid)
{
	return stat_field(pid, 18, 0);
}

/* Return the CPU time @t has used, or 0 if it cannot be read. */
static uint64_t cpu_time_of(pthread_t t)
{
	static int told;
	struct timespec ts;
	clockid_t c;
	int err;

	err = pthread_getcpuclockid(t, &c);
	if (!err && clock_gettime(c, &ts))
		err = errno;
	if (err) {
		if (!told++)
			ksft_print_msg("the CPU time of a thread cannot be read: %s\n",
				       strerror(err));
		return 0;
	}
	return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

/* Return the state letter of @pid in /proc, or 0. */
static char proc_state_of(pid_t pid)
{
	char path[64], buf[1024], *p;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	if (read_file(path, buf, sizeof(buf)))
		return 0;
	p = strrchr(buf, ')');
	if (!p || p[1] != ' ')
		return 0;
	return p[2];
}

/*
 * Wait a period. Then return the CPU share, in percent, that @t uses
 * over the next @ns.
 */
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
 * probe(): count the deadline tasks of PROBE_RUNTIME every PROBE_PERIOD
 * that the root domain of @cg admits. A child process joins @cg, or stays
 * in the caller's cgroup if @cg is NULL. Its threads become deadline tasks
 * one at a time, until one is refused. Then they all exit. Return the
 * number admitted, or -1 on an error.
 *
 * The threads barely run, so their bandwidth goes when they exit. probe()
 * still waits for their 0-lag times.
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
	/* Sleep until the probe closes the write end. */
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
				i++;	/* join that thread too */
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
 * Run @fn in a child process in @cg. Every thread of the case is then in
 * @cg, and dies with the child. Return KSFT_PASS, KSFT_FAIL or KSFT_SKIP.
 */
static int run_in(const char *cg, int (*fn)(void))
{
	pid_t pid = fork_child("a case");
	int status;

	if (!pid) {
		int err = cg ? enter(cg, getpid()) : 0;

		if (err) {
			ksft_print_msg("the case cannot enter %s: %s\n", cg, strerror(-err));
			_exit(KSFT_SKIP);
		}
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

/*
 * Run @fn in @cg with run_in(), and probe the root domain of @cg before
 * and after. Return the result of @fn, or KSFT_FAIL if the two probes
 * differ.
 */
static int measured(const char *name, const char *cg, int (*fn)(void))
{
	int before, after, ret;

	before = probe(cg);
	if (before <= 0) {
		ksft_print_msg("%s: %s\n", name, before ? "the probe failed" : "no room to probe");
		return before ? KSFT_FAIL : KSFT_SKIP;
	}
	ret = run_in(cg, fn);
	sleep_ns(3 * PERIOD);
	after = probe(cg);
	if (after < 0) {
		ksft_print_msg("%s: the probe failed after the case\n", name);
		ret = KSFT_FAIL;
	} else if (ret == KSFT_PASS && after != before) {
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

/*
 * Set up pi_lock, a priority inheritance mutex, and the owner's
 * semaphores. Return 0, or -1 if they cannot be set up.
 */
static int pi_init(void)
{
	pthread_mutexattr_t a;
	int err;

	err = pthread_mutexattr_init(&a);
	if (!err) {
		err = pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
		if (!err)
			err = pthread_mutex_init(&pi_lock, &a);
		pthread_mutexattr_destroy(&a);
	}
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

/* Undo pi_init(). The owner and the waiters must be gone. */
static void pi_fini(void)
{
	pthread_mutex_destroy(&pi_lock);
	sem_destroy(&owner_ready);
	sem_destroy(&owner_go);
}

/*
 * Make the calling thread a deadline task, and wait up to @ms for
 * pi_lock. Then switch back to SCHED_OTHER. Return 0 if the wait ends as
 * @timeout expects: a timeout if it is set, the lock if not. Return -1
 * otherwise.
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

/*
 * Take pi_lock as the owner, and return 0. On a failure, report it, set
 * owner_holds to -1 and return -1.
 */
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

/*
 * The owner thread: take pi_lock and sleep 400ms with it. If @arg
 * (was_dl) is set, first run 1ms as a deadline task of 30%. Then wait on
 * owner_go while the main thread switches the owner out of the class.
 */
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
		/* Asleep, it is switched out past its 0-lag time. */
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
	/* Each wait boosts the sleeping owner. Its timeout deboosts it. */
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

/*
 * The owner thread: become a deadline task of 30%, and take pi_lock. Hold
 * it for 300ms, spinning 2ms and sleeping 5ms in turn. The waiters have
 * an earlier deadline, so they boost it.
 */
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

/*
 * The owner thread: take pi_lock and spin 50ms with it. The unlock
 * deboosts it while it runs.
 */
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
		pi_fini();
	}
	return ret;
}

/* Parameter changes */

static atomic_int t_state;	/* 1: about to sleep, or spinning */
static _Atomic pid_t t_tid;
static sem_t t_exit;

/*
 * The sleeper thread: become a deadline task of 20ms every 100ms. Run
 * @arg ns of a fresh period, then sleep until t_exit is posted.
 */
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

/*
 * Start runs_then_sleeps() in @t, to run @run ns. Return 0 once it is
 * about to sleep, or -1.
 */
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
		sem_post(&t_exit);	/* in case it gets there late */
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

/* Return 0 if the root domain of @cg admits @want probe tasks now, or -1. */
static int admits_in(const char *cg, int want, const char *what)
{
	int n = probe(cg);

	if (n != want) {
		ksft_print_msg("%s: %d probe tasks admitted, %d expected\n", what, n, want);
		return -1;
	}
	return 0;
}

/* Do the same check in the root domain of the caller's cgroup. */
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
	/* probe(NULL) probes P: this process is in P. */
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

	/*
	 * It runs 5ms of its 20ms. Its 0-lag time is then about 20ms after
	 * it sleeps.
	 */
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
/* Set before dl_spinner() starts. */
static uint64_t spin_runtime, spin_flags;

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

/*
 * Start dl_spinner() in @t, a deadline task of @runtime every 100ms with
 * @flags. Return 0, or -1.
 */
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
	spin_stop = 1;		/* in case it gets there late */
	join(*t);
	return -1;
}

static void stop_spinner(pthread_t t)
{
	spin_stop = 1;
	set_other(t_tid);	/* out of the class already: no matter */
	join(t);
}

/*
 * Wait until @t stops using CPU time: it is throttled. Return 0, or -1
 * if that takes over 1s or its CPU time cannot be read.
 */
static int wait_throttled(pthread_t t)
{
	uint64_t end = now_ns() + NSEC_PER_SEC, last = cpu_time_of(t);

	while (last && now_ns() < end) {
		uint64_t cur;

		sleep_ns(3 * NSEC_PER_MSEC);
		cur = cpu_time_of(t);
		if (cur == last)
			return 0;
		last = cur;
	}
	return -1;
}

/* Return 0 if @t runs 1ms more within @ns, or -1. */
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
	 * P has room for base probe tasks of 10%, and not for one more. So
	 * growing the task's runtime by (base + 1) * 10ms must be refused,
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
 * A SCHED_OTHER task with a slice of 40ms asks for SCHED_DEADLINE with
 * SCHED_FLAG_KEEP_PARAMS. That changes the class, so it must be refused
 * with EINVAL. It must not reserve bandwidth for the parameters it would
 * keep, which take the slice as their runtime.
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

/*
 * The waiter thread: wait up to 300ms for pi_lock as a deadline task,
 * which boosts the owner. Return NULL if the wait timed out.
 */
static void *boosting_waiter(void *arg)
{
	return (void *)(long)wait_lock_for(300, 1);
}

/*
 * Start a boosting_waiter() thread in @w. Wait up to 200ms for it to boost
 * the owner. Return 0 once the owner is boosted. Otherwise report the
 * failure, join the waiter and return -1.
 */
static int start_boost(pthread_t *w)
{
	if (pthread_create(w, NULL, boosting_waiter, NULL)) {
		ksft_print_msg("the waiter thread could not start\n");
		return -1;
	}
	if (!wait_for(200, prio_of(owner_tid) == DL_TASK_PRIO))
		return 0;
	ksft_print_msg("the waiter never boosted the owner\n");
	join(*w);
	return -1;
}

/*
 * Join the boosting_waiter() thread @w. Return -1 if its wait did not
 * time out.
 */
static int join_waiter(pthread_t w)
{
	void *res;
	int err = pthread_join(w, &res);

	if (err)
		ksft_print_msg("pthread_join: %s\n", strerror(err));
	return err || res ? -1 : 0;
}

/*
 * A deadline waiter boosts a SCHED_OTHER owner into the deadline class.
 * Then request SCHED_DEADLINE with SCHED_FLAG_KEEP_PARAMS for the owner.
 * It is in that class already, so the request changes no class. But the
 * kept parameters would not give it the policy it asks for, so the
 * request must be refused with EINVAL. Admitted, it would reserve the
 * bandwidth of the owner's slice, and nothing would release it.
 */
static int params_keep_boosted(void)
{
	struct dl_attr slice = {
		.size = sizeof(slice),
		.sched_policy = SCHED_OTHER,
		.sched_runtime = 40 * NSEC_PER_MSEC,	/* the slice to keep */
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
	if (start_boost(&w)) {
		ret = KSFT_FAIL;
		goto out;
	}
	err = syscall(SYS_sched_setattr, owner_tid, &keep, 0) ? -errno : 0;
	if (err != -EINVAL) {
		ksft_print_msg("KEEP_PARAMS to SCHED_DEADLINE on a boosted owner: %s, not EINVAL\n",
			       err ? strerror(-err) : "accepted");
		ret = KSFT_FAIL;
	}
	if (join_waiter(w))
		ret = KSFT_FAIL;	/* the waiter said why */
out:
	join(o);
	return ret;
}

/* Check that an owner boosted by a deadline waiter can change its policy. */
static int boosted_owner_policy(void)
{
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
	if (start_boost(&w)) {
		join(o);
		return KSFT_FAIL;
	}
	err = set_other(owner_tid);
	if (err) {
		ksft_print_msg("SCHED_OTHER on a boosted owner: %s\n", strerror(-err));
		ret = KSFT_FAIL;
	}
	if (join_waiter(w))
		ret = KSFT_FAIL;	/* the waiter said why */
	join(o);
	return ret;
}

static atomic_int owner_first_period_ms;
static _Atomic pid_t donor_tid;

/*
 * The owner thread: take pi_lock, and spin until the waiter boosts it,
 * then 5ms more. The unlock deboosts it. Then become a deadline task of
 * 10ms every 100ms. The first period must run on these parameters, not
 * on what the donor left.
 *
 * With @arg 1, first run 1ms as a deadline task of 60ms every 100ms. Then
 * sleep until the main thread switches it out of the class, past its
 * 0-lag time. If the switch kept its parameters, they would be larger
 * than what the donor leaves. The deboost would then find the 0-lag
 * time ahead, and would not clear them.
 *
 * With @arg 2, become the deadline task while boosted, before the 5ms.
 * The deboost then leaves it in the class.
 */
static void *deboosted_owner(void *arg)
{
	uint64_t t0, cpu0;
	int err;

	owner_tid = gettid_();
	if (arg == (void *)1L) {
		err = set_deadline(0, 60 * NSEC_PER_MSEC, PERIOD);
		if (err) {
			ksft_print_msg("the owner refused as a deadline task: %s\n",
				       strerror(-err));
			return (void *)1;
		}
		spin_ns(NSEC_PER_MSEC);
		sem_post(&owner_ready);
		sem_wait_sig(&owner_go);
	}
	if (take_pi_lock())
		return (void *)1;
	owner_holds = 2;
	t0 = now_ns();
	while (prio_of(owner_tid) != DL_TASK_PRIO) {
		if (now_ns() - t0 > NSEC_PER_SEC) {
			drop_pi_lock();
			return (void *)2;	/* never boosted */
		}
	}
	if (arg == (void *)2L) {
		err = set_deadline(0, 10 * NSEC_PER_MSEC, PERIOD);
		if (err) {
			ksft_print_msg("the boosted owner refused as a deadline task: %s\n",
				       strerror(-err));
			drop_pi_lock();
			return (void *)1;
		}
	}
	spin_ns(5 * NSEC_PER_MSEC);
	drop_pi_lock();
	err = arg == (void *)2L ? 0 : set_deadline(0, 10 * NSEC_PER_MSEC, PERIOD);
	if (err) {
		ksft_print_msg("the owner refused as a deadline task: %s\n", strerror(-err));
		return (void *)1;
	}
	t0 = now_ns();
	cpu0 = thread_time_ns();
	while (now_ns() - t0 < PERIOD)
		;
	owner_first_period_ms = (thread_time_ns() - cpu0) / NSEC_PER_MSEC;
	return NULL;
}

/*
 * The waiter thread: take pi_lock as a deadline task of 40ms every
 * 100ms. It boosts the owner, which then runs on the waiter's runtime.
 * Return NULL once it got the lock.
 */
static void *donor_waiter(void *arg)
{
	int err;

	donor_tid = gettid_();
	err = set_deadline(0, 40 * NSEC_PER_MSEC, PERIOD);
	if (err) {
		ksft_print_msg("a waiter refused as a deadline task: %s\n", strerror(-err));
		return (void *)1;
	}
	err = take_pi_lock();
	if (!err)
		drop_pi_lock();
	if (set_other(0))
		ksft_print_msg("a waiter cannot leave SCHED_DEADLINE\n");
	return (void *)(long)err;
}

/*
 * Run a deboost case. With @mode 1, the owner first leaves the class
 * asleep, past its 0-lag time. With @mode 2, it becomes a deadline task
 * while boosted.
 */
static int deboost_case(long mode)
{
	int err, ret = KSFT_PASS;
	void *o_ret, *w_ret;
	pthread_t o, w;

	if (pi_init())
		return KSFT_SKIP;
	owner_first_period_ms = -1;
	if (pthread_create(&o, NULL, deboosted_owner, (void *)mode)) {
		ksft_print_msg("the owner thread could not start\n");
		return KSFT_FAIL;
	}
	if (mode == 1) {
		if (sem_wait_for(&owner_ready, 5000)) {
			sem_post(&owner_go);	/* only late: let it end */
			join(o);
			return KSFT_FAIL;	/* the owner said why */
		}
		sleep_ns(3 * PERIOD);
		err = proc_state_of(owner_tid) == 'S' ? set_other(owner_tid) : -EAGAIN;
		sem_post(&owner_go);
		if (err) {
			ksft_print_msg("the owner could not leave the class asleep: %s\n",
				       strerror(-err));
			join(o);
			return KSFT_FAIL;
		}
	}
	if (wait_for(5000, owner_holds) || owner_holds != 2) {
		ksft_print_msg("the owner never took the lock\n");
		join(o);
		return KSFT_FAIL;
	}
	if (pthread_create(&w, NULL, donor_waiter, NULL)) {
		ksft_print_msg("the waiter thread could not start\n");
		join(o);
		return KSFT_FAIL;
	}
	err = pthread_join(w, &w_ret);
	if (err) {
		ksft_print_msg("pthread_join: %s\n", strerror(err));
		ret = KSFT_FAIL;
	} else if (w_ret) {
		ret = KSFT_FAIL;	/* the waiter said why */
	}
	err = pthread_join(o, &o_ret);
	if (err) {
		ksft_print_msg("pthread_join: %s\n", strerror(err));
		ret = KSFT_FAIL;
	} else if (o_ret == (void *)2) {
		ksft_print_msg("the waiter never boosted the owner\n");
		ret = KSFT_FAIL;
	} else if (owner_first_period_ms < 0) {
		ret = KSFT_FAIL;
	} else if (owner_first_period_ms > 20) {
		ksft_print_msg("the owner ran %d ms of its first 100: the donor's runtime\n",
			       (int)owner_first_period_ms);
		ret = KSFT_FAIL;
	}
	return ret;
}

static int deboost_resets_period(void)
{
	return deboost_case(0);
}

/*
 * The same, for an owner whose bandwidth was released when it left the
 * class. Its parameters must go with it, and the deboost must reset what
 * the donor left.
 */
static int deboost_released_resets_period(void)
{
	return deboost_case(1);
}

/*
 * The same, for an owner that becomes a deadline task while boosted. It
 * stays in the class at the deboost, which must reset what the donor
 * left.
 */
static int deboost_in_class_resets_period(void)
{
	return deboost_case(2);
}

static sem_t ret_ready, ret_go;
static _Atomic pid_t ret_tid;
static atomic_int ret_first_period_ms;

/*
 * The thread of the return cases. Run 20ms as a deadline task of 50ms
 * every 100ms. This puts its 0-lag time about 20ms ahead, with 30ms of
 * runtime left. Then come back as a deadline task of 10ms every 100ms,
 * through SCHED_OTHER. With @arg 0, the thread does both changes while
 * it runs. With @arg 1, it sleeps, and the main thread changes it. With
 * @arg 2, the thread changes to 10ms every 100ms while it runs, without
 * leaving the class. Then measure the CPU time it gets in the next
 * 100ms. It must run on the new parameters, not on the runtime left from
 * the old ones.
 */
static void *returner(void *arg)
{
	uint64_t t0, cpu0;
	int err;

	ret_tid = gettid_();
	err = set_deadline(0, 50 * NSEC_PER_MSEC, PERIOD);
	if (err) {
		ksft_print_msg("the task refused as a deadline task: %s\n", strerror(-err));
		return (void *)1;
	}
	spin_ns(20 * NSEC_PER_MSEC);
	if (arg == (void *)1L) {
		sem_post(&ret_ready);
		sem_wait_sig(&ret_go);
	} else {
		err = arg ? 0 : set_other(0);
		if (!err)
			err = set_deadline(0, 10 * NSEC_PER_MSEC, PERIOD);
		if (err) {
			ksft_print_msg("the task could not change: %s\n", strerror(-err));
			return (void *)1;
		}
	}
	t0 = now_ns();
	cpu0 = thread_time_ns();
	while (now_ns() - t0 < PERIOD)
		;
	ret_first_period_ms = (thread_time_ns() - cpu0) / NSEC_PER_MSEC;
	return NULL;
}

/* Run a return case: returner() with @mode as its argument. */
static int return_case(long mode)
{
	int err, ret = KSFT_PASS;
	void *t_ret;
	pthread_t t;

	if (sem_init(&ret_ready, 0, 0) || sem_init(&ret_go, 0, 0)) {
		ksft_print_msg("sem_init: %s\n", strerror(errno));
		return KSFT_FAIL;
	}
	ret_first_period_ms = -1;
	if (pthread_create(&t, NULL, returner, (void *)mode)) {
		ksft_print_msg("the task thread could not start\n");
		return KSFT_FAIL;
	}
	if (mode == 1) {
		if (sem_wait_for(&ret_ready, 5000)) {
			sem_post(&ret_go);	/* only late: let it end */
			join(t);
			return KSFT_FAIL;	/* the task said why */
		}
		/* Before its 0-lag time, about 20ms ahead */
		err = wait_for(10, proc_state_of(ret_tid) == 'S') ? -EAGAIN : 0;
		if (!err)
			err = set_other(ret_tid);
		if (!err)
			err = set_deadline(ret_tid, 10 * NSEC_PER_MSEC, PERIOD);
		sem_post(&ret_go);
		if (err) {
			ksft_print_msg("the task could not come back asleep: %s\n",
				       strerror(-err));
			join(t);
			return KSFT_FAIL;
		}
	}
	err = pthread_join(t, &t_ret);
	if (err) {
		ksft_print_msg("pthread_join: %s\n", strerror(err));
		ret = KSFT_FAIL;
	} else if (t_ret || ret_first_period_ms < 0) {
		ret = KSFT_FAIL;	/* the task said why */
	} else if (ret_first_period_ms > 20) {
		ksft_print_msg("the task ran %d ms of its first 100: the old runtime\n",
			       (int)ret_first_period_ms);
		ret = KSFT_FAIL;
	}
	sem_destroy(&ret_ready);
	sem_destroy(&ret_go);
	return ret;
}

/*
 * A task leaves the class while it runs, and comes back at once with a
 * smaller runtime. It keeps its deadline and the runtime left, as its
 * 0-lag time is ahead. That runtime must fit the new parameters.
 */
static int return_running_new_params(void)
{
	return return_case(0);
}

/* The same, for a task that leaves and comes back while it sleeps. */
static int return_asleep_new_params(void)
{
	return return_case(1);
}

/* The same, for a task that changes its parameters within the class. */
static int change_running_new_params(void)
{
	return return_case(2);
}

static pthread_mutex_t pi_lock2;
static sem_t holder_go;
static atomic_int holder_holds, chain_waits, donor_done, x_waits, l2_takes;
static atomic_int chain_rank, x_rank;
static _Atomic pid_t x_tid, fair_tid;

/* The holder thread: take pi_lock2, and hold it until holder_go. */
static void *l2_holder(void *arg)
{
	int err = pthread_mutex_lock(&pi_lock2);

	if (err) {
		ksft_print_msg("the holder cannot take pi_lock2: %s\n", strerror(err));
		holder_holds = -1;
		return (void *)1;
	}
	holder_holds = 1;
	sem_wait_sig(&holder_go);
	pthread_mutex_unlock(&pi_lock2);
	return NULL;
}

/* Take pi_lock2, store the caller's rank in @rank, and drop it. */
static int take_l2_ranked(atomic_int *rank)
{
	int err = pthread_mutex_lock(&pi_lock2);

	if (err) {
		ksft_print_msg("a waiter cannot take pi_lock2: %s\n", strerror(err));
		return -1;
	}
	*rank = l2_takes++;
	pthread_mutex_unlock(&pi_lock2);
	return 0;
}

/*
 * The owner thread: a deadline task of 10ms every 200ms. Take pi_lock,
 * and sleep until owner_go. The donor boosts it meanwhile. Then spin
 * 20ms. Its own runtime runs out, and the boost refills it from the
 * donor's. So its runtime is above its own at the deboost. Then wait on
 * pi_lock2, with pi_lock held.
 */
static void *chain_owner(void *arg)
{
	uint64_t t0;
	int err;

	owner_tid = gettid_();
	err = set_deadline(0, 10 * NSEC_PER_MSEC, 2 * PERIOD);
	if (err) {
		ksft_print_msg("the owner refused as a deadline task: %s\n", strerror(-err));
		owner_holds = -1;
		return (void *)1;
	}
	if (take_pi_lock())
		return (void *)1;
	owner_holds = 2;
	sem_wait_sig(&owner_go);
	t0 = now_ns();
	spin_ns(20 * NSEC_PER_MSEC);
	if (now_ns() - t0 > PERIOD / 2) {
		drop_pi_lock();
		return (void *)2;	/* throttled: never boosted */
	}
	chain_waits = 1;
	err = take_l2_ranked(&chain_rank);
	drop_pi_lock();
	return err ? (void *)1 : NULL;
}

/*
 * The donor thread: a deadline task of 40ms every 100ms. Wait 150ms for
 * pi_lock. Its timeout deboosts the owner, which waits on pi_lock2 then.
 */
static void *timed_donor(void *arg)
{
	struct timespec ts;
	int err;

	donor_tid = gettid_();
	err = set_deadline(0, 40 * NSEC_PER_MSEC, PERIOD);
	if (err) {
		ksft_print_msg("the donor refused as a deadline task: %s\n", strerror(-err));
		return (void *)1;
	}
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += 150 * 1000000L;
	ts.tv_sec += ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;
	err = pthread_mutex_timedlock(&pi_lock, &ts);
	donor_done = 1;
	if (!err)
		pthread_mutex_unlock(&pi_lock);
	if (set_other(0))
		ksft_print_msg("the donor cannot leave SCHED_DEADLINE\n");
	if (err != ETIMEDOUT) {
		ksft_print_msg("the donor: %s, not a timeout\n",
			       err ? strerror(err) : "got pi_lock");
		return (void *)1;
	}
	return NULL;
}

/*
 * The other waiter: a deadline task of 5ms every 50ms. Its deadline is
 * earlier than the one the owner gets at the deboost. Wait on pi_lock2.
 */
static void *early_waiter(void *arg)
{
	int err;

	x_tid = gettid_();
	err = set_deadline(0, 5 * NSEC_PER_MSEC, PERIOD / 2);
	if (err) {
		ksft_print_msg("a waiter refused as a deadline task: %s\n", strerror(-err));
		return (void *)1;
	}
	x_waits = 1;
	err = take_l2_ranked(&x_rank);
	if (set_other(0))
		ksft_print_msg("a waiter cannot leave SCHED_DEADLINE\n");
	return err ? (void *)1 : NULL;
}

/* Set up pi_lock2 and holder_go. Return 0, or -1. */
static int pi_init_lock2(void)
{
	pthread_mutexattr_t a;
	int err;

	err = pthread_mutexattr_init(&a);
	if (!err) {
		err = pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
		if (!err)
			err = pthread_mutex_init(&pi_lock2, &a);
		pthread_mutexattr_destroy(&a);
	}
	if (err) {
		ksft_print_msg("no second priority inheritance mutex: %s\n", strerror(err));
		return -1;
	}
	if (sem_init(&holder_go, 0, 0)) {
		ksft_print_msg("sem_init: %s\n", strerror(errno));
		pthread_mutex_destroy(&pi_lock2);
		return -1;
	}
	holder_holds = 0;
	chain_waits = 0;
	donor_done = 0;
	x_waits = 0;
	l2_takes = 0;
	chain_rank = -1;
	x_rank = -1;
	donor_tid = 0;
	x_tid = 0;
	fair_tid = 0;
	return 0;
}

/*
 * Check the order of the waiters of pi_lock2 after a deboost. The holder
 * has pi_lock2, and the owner has pi_lock:
 * 1. The donor waits on pi_lock, and boosts the owner.
 * 2. The owner spins on the donor's runtime, then waits on pi_lock2.
 * 3. The other waiter waits on pi_lock2.
 * 4. The donor times out. The deboost starts a new period for the
 *    owner, whose runtime is above its own.
 * 5. The holder drops pi_lock2.
 *
 * The other waiter must take pi_lock2 first: its deadline is earlier
 * than the owner's new one. At step 4 the owner is not queued, and the
 * chain walk copies its deadline into its waiter node on pi_lock2. A
 * deadline of 0 in the node would put the owner first.
 */
static int deboost_blocked_keeps_order(void)
{
	void *o_ret = NULL, *d_ret = NULL, *x_ret = NULL;
	int ret = KSFT_FAIL, have_o = 0, have_d = 0, have_x = 0;
	pthread_t h, o, d, x;

	if (pi_init())
		return KSFT_SKIP;
	if (pi_init_lock2()) {
		pi_fini();
		return KSFT_SKIP;
	}
	if (pthread_create(&h, NULL, l2_holder, NULL)) {
		ksft_print_msg("the holder thread could not start\n");
		goto out_fini;
	}
	if (wait_for(5000, holder_holds) || holder_holds != 1) {
		ksft_print_msg("the holder never took pi_lock2\n");
		goto out;
	}
	have_o = !pthread_create(&o, NULL, chain_owner, NULL);
	if (!have_o || wait_for(5000, owner_holds) || owner_holds != 2) {
		ksft_print_msg("the owner never took pi_lock\n");
		goto out;
	}
	have_d = !pthread_create(&d, NULL, timed_donor, NULL);
	if (!have_d || wait_for(1000, donor_tid && proc_state_of(donor_tid) == 'S')) {
		ksft_print_msg("the donor never waited on pi_lock\n");
		goto out;
	}
	sem_post(&owner_go);
	if (wait_for(1000, chain_waits && proc_state_of(owner_tid) == 'S')) {
		ksft_print_msg("the owner never waited on pi_lock2\n");
		goto out;
	}
	if (donor_done) {
		ksft_print_msg("the donor timed out before the owner waited on pi_lock2\n");
		goto out;
	}
	have_x = !pthread_create(&x, NULL, early_waiter, NULL);
	if (!have_x || wait_for(1000, x_waits && proc_state_of(x_tid) == 'S')) {
		ksft_print_msg("the other waiter never waited on pi_lock2\n");
		goto out;
	}
	/* The donor's timeout deboosts the owner, which waits on pi_lock2. */
	have_d = 0;
	if (!pthread_join(d, &d_ret) && !d_ret)
		ret = KSFT_PASS;
out:
	sem_post(&owner_go);	/* only late: let the owner end */
	sem_post(&holder_go);
	if (have_d)
		join(d);
	if (have_x && pthread_join(x, &x_ret))
		ret = KSFT_FAIL;
	if (have_o && pthread_join(o, &o_ret))
		ret = KSFT_FAIL;
	join(h);
	if (o_ret == (void *)2) {
		ksft_print_msg("the donor never boosted the owner\n");
		ret = KSFT_FAIL;
	} else if (o_ret || x_ret) {
		ret = KSFT_FAIL;	/* the thread said why */
	} else if (ret == KSFT_PASS && chain_rank < x_rank) {
		ksft_print_msg("the deboosted owner took pi_lock2 before the earlier waiter\n");
		ret = KSFT_FAIL;
	}
out_fini:
	pthread_mutex_destroy(&pi_lock2);
	sem_destroy(&holder_go);
	pi_fini();
	return ret;
}

/*
 * The fair waiter: wait on pi_lock2 as a fair task. With @arg set, hold
 * pi_lock meanwhile, for a donor to boost it. Otherwise the main thread
 * makes it a deadline task while it waits. It leaves the class at the end.
 */
static void *fair_waiter(void *arg)
{
	int err;

	fair_tid = gettid_();
	if (arg && take_pi_lock())
		return (void *)1;
	chain_waits = 1;
	err = take_l2_ranked(&chain_rank);
	if (arg)
		drop_pi_lock();
	else if (set_other(0))
		ksft_print_msg("a waiter cannot leave SCHED_DEADLINE\n");
	return err ? (void *)1 : NULL;
}

/*
 * Check the order of the waiters of pi_lock2 when one of them enters the
 * class while it waits. The holder has pi_lock2:
 * 1. The fair waiter waits on pi_lock2. With @boost, it holds pi_lock.
 * 2. The other waiter, a deadline task, waits on pi_lock2.
 * 3. The fair waiter enters the class:
 *    - Without @boost, the main thread makes it a deadline task of 10ms
 *      every 200ms.
 *    - With @boost, a donor of 40ms every 100ms waits on pi_lock, and
 *      boosts it.
 * 4. The holder drops pi_lock2.
 *
 * The other waiter must take pi_lock2 first: it started its period
 * earlier, with a shorter deadline. At step 3 the fair waiter is not
 * queued, and the chain walk copies its deadline into its waiter node on
 * pi_lock2. A deadline of 0 in the node would put the fair waiter first.
 */
static int blocked_keeps_order(long boost)
{
	void *f_ret = NULL, *x_ret = NULL, *w_ret = NULL;
	int ret = KSFT_FAIL, have_f = 0, have_x = 0, have_w = 0, err;
	pthread_t h, f, x, w;

	if (pi_init())
		return KSFT_SKIP;
	if (pi_init_lock2()) {
		pi_fini();
		return KSFT_SKIP;
	}
	if (pthread_create(&h, NULL, l2_holder, NULL)) {
		ksft_print_msg("the holder thread could not start\n");
		goto out_fini;
	}
	if (wait_for(5000, holder_holds) || holder_holds != 1) {
		ksft_print_msg("the holder never took pi_lock2\n");
		goto out;
	}
	have_f = !pthread_create(&f, NULL, fair_waiter, (void *)boost);
	if (!have_f || wait_for(1000, chain_waits && proc_state_of(fair_tid) == 'S')) {
		ksft_print_msg("the fair waiter never waited on pi_lock2\n");
		goto out;
	}
	have_x = !pthread_create(&x, NULL, early_waiter, NULL);
	if (!have_x || wait_for(1000, x_waits && proc_state_of(x_tid) == 'S')) {
		ksft_print_msg("the other waiter never waited on pi_lock2\n");
		goto out;
	}
	if (boost) {
		have_w = !pthread_create(&w, NULL, donor_waiter, NULL);
		if (!have_w || wait_for(1000, donor_tid && proc_state_of(donor_tid) == 'S')) {
			ksft_print_msg("the donor never waited on pi_lock\n");
			goto out;
		}
		if (prio_of(fair_tid) != DL_TASK_PRIO) {
			ksft_print_msg("the donor never boosted the fair waiter\n");
			goto out;
		}
	} else {
		err = set_deadline(fair_tid, 10 * NSEC_PER_MSEC, 2 * PERIOD);
		if (err) {
			ksft_print_msg("the fair waiter refused as a deadline task: %s\n",
				       strerror(-err));
			if (err == -EBUSY)
				ret = KSFT_SKIP;	/* no room in P */
			goto out;
		}
	}
	ret = KSFT_PASS;
out:
	sem_post(&holder_go);	/* pi_lock2 goes to its top waiter */
	if (have_w && pthread_join(w, &w_ret))
		ret = KSFT_FAIL;
	if (have_x && pthread_join(x, &x_ret))
		ret = KSFT_FAIL;
	if (have_f && pthread_join(f, &f_ret))
		ret = KSFT_FAIL;
	join(h);
	if (f_ret || x_ret || w_ret) {
		ret = KSFT_FAIL;	/* the thread said why */
	} else if (ret == KSFT_PASS && chain_rank < x_rank) {
		ksft_print_msg("the fair waiter took pi_lock2 before the earlier waiter\n");
		ret = KSFT_FAIL;
	}
out_fini:
	pthread_mutex_destroy(&pi_lock2);
	sem_destroy(&holder_go);
	pi_fini();
	return ret;
}

/* sched_setattr() puts the fair waiter in the class. */
static int setattr_blocked_keeps_order(void)
{
	return blocked_keeps_order(0);
}

/* A boost puts the fair waiter in the class. */
static int boost_blocked_keeps_order(void)
{
	return blocked_keeps_order(1);
}

/* cpuset moves, from the main process, outside P */

/* Shared with the processes of a case: atomic in shared memory. */
static atomic_int *mv_state;

/*
 * Wait up to 5s for a child to report in mv_state[0]. Return 0 if it is
 * up (1). Return -1 if it was refused SCHED_DEADLINE (-1), could not
 * start its threads (-2) or did not report. @fast polls every 100us.
 */
static int child_up(const char *what, int fast)
{
	if (wait_for_step(5000, fast ? 100 * NSEC_PER_USEC : NSEC_PER_MSEC, mv_state[0])) {
		ksft_print_msg("%s: the task never started\n", what);
		return -1;
	}
	if (mv_state[0] == -2) {
		ksft_print_msg("%s: the process cannot start its threads\n", what);
		return -1;
	}
	if (mv_state[0] != 1) {
		ksft_print_msg("%s: the task was refused SCHED_DEADLINE\n", what);
		return -1;
	}
	return 0;
}

/*
 * mover() forks a process in the caller's cgroup. The process becomes a
 * deadline task of @runtime every 100ms, sets mv_state[0] to 1, and does
 * what @mode says:
 *  - MV_ASLEEP: run 1ms, then sleep.
 *  - MV_THROTTLED: spin and count in mv_state[1].
 *  - MV_YIELDED: spin 0.5ms, count and yield, over and over.
 *  - MV_NON_CONTENDING: in a fresh period, run a quarter of its runtime,
 *    then sleep before its 0-lag time.
 */
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

static int wait_spinner(int moving);

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
	if (mode == MV_NON_CONTENDING) {
		sleep_ns(NSEC_PER_MSEC);
	} else if (mode == MV_ASLEEP) {
		sleep_ns(2 * PERIOD);
	} else if (wait_spinner(0)) {
		/* Throttled, or yielded: its counter stops. */
		ksft_print_msg("%s: never stopped\n", name);
		reap(pid);
		return KSFT_FAIL;
	}
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
	/* With a task of 60% in P, moving in another one of 60% is refused. */
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

/*
 * A runtime of 1024ns every 2s gives a bandwidth of 0. A cpuset move
 * skips such a task: it has no bandwidth to move. Move one asleep into P,
 * where a task of 60% runs, and wake it on P's CPU. Then raise it to 60%.
 * P has no room for that, so the change must be refused with EBUSY. The
 * root domain the task left must not admit it.
 */
static int move_zero_bw(void)
{
	int base_p, base_r, ret = KSFT_PASS, err, zfd[2];
	pid_t in_p, pid;

	if (pipe(zfd))
		return KSFT_FAIL;
	base_p = probe(cg_p);
	base_r = probe(root);
	mv_state[0] = 0;
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
		ret = KSFT_FAIL;
		goto out;
	}
	mv_state[0] = 0;
	pid = fork_child("a task with no bandwidth");
	if (!pid) {
		char c;

		close(zfd[1]);
		mv_state[0] = 1;
		while (read(zfd[0], &c, 1) == 1)
			;
		_exit(0);
	}
	if (pid < 0 || child_up("a task to move into P", 0) ||
	    wait_for(1000, proc_state_of(pid) == 'S')) {
		reap(pid);
		reap(in_p);
		ret = KSFT_FAIL;
		goto out;
	}
	/*
	 * Set the parameters from here, while the task sleeps. With a runtime
	 * of 1024ns, the task could be throttled before it sleeps again, for
	 * up to its period of 2s.
	 */
	err = set_deadline(pid, 1024, 2 * NSEC_PER_SEC);
	if (err) {
		ksft_print_msg("a runtime of 1024ns every 2s: %s\n", strerror(-err));
		ret = KSFT_FAIL;
	} else if (enter(cg_p, pid)) {
		ksft_print_msg("a task with no bandwidth: its move into P refused\n");
		ret = KSFT_FAIL;
	} else if (write(zfd[1], "w", 1) != 1 || wait_for(1000, task_cpu_of(pid) == top_cpu(1))) {
		ksft_print_msg("a task moved into P did not wake on P's CPU\n");
		ret = KSFT_FAIL;
	} else {
		err = set_deadline(pid, 60 * NSEC_PER_MSEC, PERIOD);
		if (err != -EBUSY) {
			ksft_print_msg("0%% to 60%% beside a task of 60%% in P: %s, not EBUSY\n",
				       err ? strerror(-err) : "accepted");
			ret = KSFT_FAIL;
		}
	}
	/* Throttled, it would wait up to 2s to run and die. */
	set_other(pid);
	reap(pid);
	reap(in_p);
out:
	close(zfd[0]);
	close(zfd[1]);
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS && (probe(cg_p) != base_p || probe(root) != base_r)) {
		ksft_print_msg("a refused change changed the free bandwidth\n");
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

/*
 * Fork a process that writes sched_rt_runtime_us back to itself @n times,
 * 5ms apart, and exits. Each write rebuilds the sched domains. Return
 * its pid, or -1.
 */
static pid_t rebuilder(int n)
{
	pid_t pid = fork_child("a sysctl writer");

	if (!pid) {
		char buf[64];

		if (read_file("/proc/sys/kernel/sched_rt_runtime_us", buf, sizeof(buf)))
			_exit(1);
		while (n-- > 0) {
			write_file("/proc/sys/kernel", "sched_rt_runtime_us", buf);
			sleep_ns(5 * NSEC_PER_MSEC);
		}
		_exit(0);
	}
	return pid;
}

static void *pauser(void *arg)
{
	for (;;)
		pause();
	return arg;
}

/*
 * Run as the deadline thread of wide_mover(): a deadline task of 30% that
 * runs 1ms, sets mv_state[0] to 1 and sleeps.
 */
static void *dl_pauser(void *arg)
{
	if (set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD)) {
		mv_state[0] = -1;
		return arg;
	}
	spin_ns(NSEC_PER_MSEC);
	*mv_state = 1;
	for (;;)
		pause();
	return arg;
}

/*
 * Fork a process of @threads sleeping threads, then one deadline thread,
 * dl_pauser(). A cgroup move of the process moves its threads one by one,
 * between cpuset_can_attach() and cpuset_attach(). The deadline thread
 * moves last, so that a rebuild in that window can still find it in the
 * cgroup it leaves. Return the pid, or -1.
 */
static pid_t wide_mover(int threads)
{
	pid_t pid = fork_child("a wide deadline process");

	if (!pid) {
		pthread_attr_t a;
		pthread_t t;
		int i;

		if (pthread_attr_init(&a) || pthread_attr_setstacksize(&a, THREAD_STACK))
			goto no_threads;
		for (i = 0; i < threads; i++) {
			if (pthread_create(&t, &a, pauser, NULL))
				goto no_threads;
		}
		if (pthread_create(&t, NULL, dl_pauser, NULL))
			goto no_threads;
		for (;;)
			pause();
no_threads:
		mv_state[0] = -2;
		_exit(1);
	}
	return pid;
}

/*
 * Move a deadline task in and out of P while a writer of the sysctl
 * rebuilds the sched domains every 5ms. A rebuild between
 * cpuset_can_attach() and cpuset_attach() drops the bandwidth that
 * cpuset_can_attach() set aside in the destination. cpuset_attach() then
 * moves the task's bandwidth all the same. Without the 5ms gaps, the 20
 * rebuilds would be over before the first move reaches that window.
 *
 * A rebuild puts the accounting right again. So the moves go on until the
 * writer is done, and the probes see the last move it raced. Run six
 * rounds.
 */
static int move_vs_rebuild(void)
{
	int base_p, base_r, with_p, with_r, after_p, after_r, round, ret = KSFT_PASS;
	const char *name = "move_vs_rebuild";
	pid_t pid, writer;

	base_p = probe(cg_p);
	base_r = probe(root);
	mv_state[0] = 0;
	pid = wide_mover(2048);
	if (pid < 0 || child_up(name, 0)) {
		reap(pid);
		return mv_state[0] == -2 ? KSFT_SKIP : KSFT_FAIL;
	}
	/* The reference for the rounds: the task asleep in the root cgroup. */
	sleep_ns(2 * PERIOD);
	with_p = probe(cg_p);
	with_r = probe(root);
	for (round = 0; round < 6 && ret == KSFT_PASS; round++) {
		int status, err = 0;

		writer = rebuilder(20);
		if (writer < 0) {
			ret = KSFT_FAIL;
			break;
		}
		while (!err && waitpid(writer, &status, WNOHANG) == 0) {
			err = enter(cg_p, pid);
			if (!err)
				err = enter(root, pid);
		}
		if (err) {
			ksft_print_msg("%s: a move refused: %s\n", name, strerror(-err));
			reap(writer);
			ret = KSFT_FAIL;
			break;
		}
		sleep_ns(3 * PERIOD);
		after_p = probe(cg_p);
		after_r = probe(root);
		if (after_p != with_p || after_r != with_r) {
			ksft_print_msg("%s: round %d: P %d -> %d probe tasks, the rest %d -> %d\n",
				       name, round, with_p, after_p, with_r, after_r);
			ret = KSFT_FAIL;
		}
	}
	reap(pid);
	sleep_ns(3 * PERIOD);
	after_p = probe(cg_p);
	after_r = probe(root);
	if (ret == KSFT_PASS && (after_p != base_p || after_r != base_r)) {
		ksft_print_msg("%s: task gone: P %d -> %d probe tasks, the rest %d -> %d\n",
			       name, base_p, after_p, base_r, after_r);
		ret = KSFT_FAIL;
	}
	return ret;
}

/* Deaths, in P */

enum die_mode { DIE_RUNNING, DIE_SWITCHED_OUT, DIE_SHRUNK };

/*
 * Fork a deadline task of 30% that runs 5ms of a fresh period and exits,
 * its 0-lag time ahead. @mode says what it does before it exits:
 *  - DIE_RUNNING: nothing.
 *  - DIE_SWITCHED_OUT: switch to SCHED_OTHER.
 *  - DIE_SHRUNK: shrink to 10%, which puts the 0-lag time behind it.
 *
 * Its bandwidth goes at the 0-lag time, or at the exit for DIE_SHRUNK.
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
 * With @moving, wait until the spinner's count in mv_state[1] moves: it
 * runs. Without, wait until the count stops for 5ms: it is throttled.
 * Return 0, or -1 after 1s.
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
 * Start a deadline task of 30% with mover() in @mode, then kill it as
 * @when says:
 *  - KILL_AFTER: @after ns after it is up.
 *  - KILL_THROTTLED: once it is throttled.
 *  - KILL_RUNNING: while it runs.
 *
 * The killer first leaves P. It then watches and kills from another CPU,
 * and does not take the task's CPU.
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
 * A robust priority inheritance lock shared between processes.
 * mv_state[0] holds the state of its owner, mv_state[2] that of its
 * waiter.
 */
static pthread_mutex_t *shm_lock;

/* Set up shm_lock. Return 0, or -1 if it cannot be set up. */
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

/*
 * Fork a process that takes shm_lock and sleeps with it, as a deadline
 * task of 30% if @dl. Return its pid, or -1.
 */
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

/*
 * Fork a deadline task of 5ms every 50ms that waits for shm_lock. It sets
 * mv_state[2] to 1 before the wait, and to 2 once it got the lock. Return
 * its pid, or -1.
 */
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

/* Wait up to 1s for @var to reach @want. Return 0, or -1. */
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
 * The owner sleeps with shm_lock, as a deadline task if @dl_owner, and
 * the waiter boosts it. Then kill the owner if @kill_owner, or else the
 * waiter. A killed owner does not die boosted. Its exit first releases
 * its priority inheritance futexes in futex_exit_exec_release(), which
 * deboosts it.
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

/*
 * The deadline threads of death_group(), killed at once. Thread 0, of
 * 10ms every 100ms, runs 1ms and sleeps. Thread 1, of 10ms too, spins and
 * counts in mv_state[1]: it is throttled first. Thread 2, of 60ms, waits
 * for mv_state[2] from the killer. Then it spins and counts in
 * mv_state[3].
 */
static void *group_thread(void *arg)
{
	long kind = (long)arg;

	if (set_deadline(0, (kind == 2 ? 60 : 10) * NSEC_PER_MSEC, PERIOD)) {
		mv_state[0] = -1000;
		return NULL;
	}
	if (kind == 2)
		while (!mv_state[2])
			sleep_ns(NSEC_PER_MSEC);
	spin_ns(NSEC_PER_MSEC);
	mv_state[0]++;
	if (!kind)
		for (;;)
			pause();
	for (;;)
		mv_state[kind == 2 ? 3 : 1]++;
	return NULL;
}

static int death_group(void)
{
	int last, ret = KSFT_PASS;
	pid_t pid;

	mv_state[0] = mv_state[1] = mv_state[2] = mv_state[3] = 0;
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
	/* From outside P: in it, the killer never runs while a thread does. */
	if (enter(root, getpid())) {
		ksft_print_msg("the killer cannot leave P\n");
		ret = KSFT_FAIL;
		goto out;
	}
	if (wait_for(5000, mv_state[0] < 0 || mv_state[0] == 2) || mv_state[0] != 2) {
		ksft_print_msg("only %d threads of the group started\n", (int)mv_state[0]);
		ret = KSFT_FAIL;
		goto out;
	}
	if (wait_spinner(0)) {
		ksft_print_msg("the first spinner never throttled\n");
		ret = KSFT_FAIL;
		goto out;
	}
	last = mv_state[3];
	mv_state[2] = 1;
	if (wait_for(5000, mv_state[0] < 0 || mv_state[3] != last) || mv_state[0] != 3) {
		ksft_print_msg("the second spinner never ran\n");
		ret = KSFT_FAIL;
	}
out:
	/* One asleep, one throttled, one running. */
	reap(pid);
	return ret;
}

/*
 * A deadline task of P exits and stays a zombie. Switching the zombie to
 * SCHED_OTHER must be refused with ESRCH. On kernels where each cpuset
 * counts its deadline tasks, cgroup_task_dead() took the zombie off P's
 * count. The switch would run switched_from_dl() and take it off a
 * second time. The count would then be 0 with a deadline task left in
 * P. A rebuild of the accounting would skip P, and that task's
 * bandwidth with it. So rebuild the accounting, and check that P still
 * holds that bandwidth.
 */
static int death_zombie_switched(void)
{
	int base, ret = KSFT_PASS, err;
	pid_t in_p, zombie;

	base = probe(cg_p);
	if (base < 4) {
		ksft_print_msg("no room in P for a task of 30%% and a probe\n");
		return KSFT_SKIP;
	}
	mv_state[0] = 0;
	in_p = fork_child("a deadline task of P");
	if (!in_p) {
		if (enter(cg_p, getpid()) || set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD))
			_exit(1);
		mv_state[0] = 1;
		for (;;)
			pause();
	}
	if (in_p < 0 || child_up("a task of 30% in P", 0)) {
		reap(in_p);
		return KSFT_FAIL;
	}
	mv_state[0] = 0;
	zombie = fork_child("a deadline task that exits");
	if (!zombie) {
		if (enter(cg_p, getpid()) || set_deadline(0, 10 * NSEC_PER_MSEC, PERIOD))
			_exit(1);
		mv_state[0] = 1;
		_exit(0);
	}
	if (zombie < 0 || child_up("a task of 10% in P that exits", 0) ||
	    wait_for(1000, proc_state_of(zombie) == 'Z')) {
		ksft_print_msg("no zombie deadline task\n");
		reap(zombie);
		reap(in_p);
		return KSFT_FAIL;
	}
	/* Its bandwidth goes at its 0-lag time, within a period. */
	sleep_ns(3 * PERIOD);
	err = set_other(zombie);
	if (err != -ESRCH) {
		ksft_print_msg("SCHED_OTHER on a zombie deadline task: %s, not ESRCH\n",
			       err ? strerror(-err) : "accepted");
		ret = KSFT_FAIL;
	}
	/* Rebuild the accounting: make Q beside P, then remove it. */
	if (make_partition(cg_q, top_cpu(2))) {
		ksft_print_msg("no partition root Q beside P\n");
		ret = KSFT_FAIL;
	}
	remove_partition(cg_q);
	if (admits_in(cg_p, base - 3, "P rebuilt, a task of 30% and a zombie in it"))
		ret = KSFT_FAIL;
	reap(zombie);
	reap(in_p);
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS && admits_in(cg_p, base, "P, its tasks gone"))
		ret = KSFT_FAIL;
	return ret;
}

/* sched_yield() */

static atomic_int y_rounds;	/* the yields of the yielder */

/*
 * The yielder thread: a deadline task of 30% that yields after 0.5ms of
 * every period. sched_yield() throttles it with runtime left, so it is
 * throttled most of the time. Its 0-lag time is then a few milliseconds
 * away, and the inactive timer that releases its bandwidth is armed for
 * that long. t_state is 2 while it is in the yield.
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
	spin_stop = 1;		/* in case it gets there late */
	join(*t);
	return -1;
}

/*
 * Switch a deadline task to SCHED_OTHER while sched_yield() throttles it.
 * Its bandwidth must come back, and it must run again.
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

/* Kill a deadline task while sched_yield() throttles it. */
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

/*
 * The overrun thread: a deadline task of 10% with SCHED_FLAG_DL_OVERRUN.
 * Run 15ms, past the runtime, then sleep until spin_stop is set.
 */
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
 * When a task with SCHED_FLAG_DL_OVERRUN overruns its runtime, its thread
 * group gets SIGXCPU. Catch it here. The task's bandwidth must stay
 * reserved after the overrun, and come back when the task leaves
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
		 * Wait for the other two first. On P's one CPU, this
		 * thread has the earliest deadline of the three. It would
		 * overrun, and kill them, before they reach the state the
		 * signal must find them in.
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
 * Let the SIGXCPU of an overrun kill the whole thread group. The
 * bandwidth of its three deadline threads must come back.
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
				_exit(3);
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
		switch (WEXITSTATUS(status)) {
		case 1:
			ksft_print_msg("a thread of the group was refused SCHED_DEADLINE\n");
			return KSFT_SKIP;
		case 2:
			ksft_print_msg("the other two threads of the group were not ready in 5s\n");
			break;
		default:
			ksft_print_msg("a thread of the group could not start\n");
		}
		return KSFT_FAIL;
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
 * A deadline task with SCHED_FLAG_RECLAIM runs past its runtime. It takes
 * the bandwidth of its root domain that no deadline task uses. On a CPU
 * of its own, a task of 10ms every 100ms gets most of the CPU. Without
 * the flag, it gets its 10%.
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
 * Switch a reclaiming task out of SCHED_DEADLINE before its 0-lag time,
 * and back. Its bandwidth stays until the inactive timer releases it.
 * Afterwards, the task must reclaim as much as before.
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
 * Fork a reclaiming deadline task of 10%. It spins, and publishes in
 * mv_state[1] the CPU time it has used, in milliseconds. It idles while
 * mv_state[2] is set, and the moves happen then. A cpuset move waits for
 * an RCU grace period. Where RCU runs in kthreads (PREEMPT_RT), a task
 * that reclaims its whole CPU keeps that grace period from ending. Return
 * the pid, or -1.
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

/* Idle the reclaiming task, then move it to @cg. Return 0, or -errno. */
static int move_idle(const char *cg, pid_t pid)
{
	int err;

	mv_state[2] = 1;
	sleep_ns(20 * NSEC_PER_MSEC);	/* it stops spinning */
	err = enter(cg, pid);
	mv_state[2] = 0;
	return err;
}

/* Return the CPU share, in percent, of the reclaiming task over @ns. */
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
 * Move a reclaiming task into P, back out, and into P again. It must
 * reclaim as much of P's CPU as it did the first time. Once it is gone,
 * the free bandwidth of both root domains must be what it was.
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

#define SERVER_RUNTIME	(100 * NSEC_PER_MSEC)	/* the cases start at 10% */
#define SERVER_PERIOD	(1000 * NSEC_PER_MSEC)

static const char server_dir[] = "/sys/kernel/debug/sched/fair_server";
static uint64_t *server_found;	/* each fair server's runtime and period */

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

/*
 * Set the fair server of @cpu to @runtime every @period.
 * Return 0, or -errno.
 */
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

/* Record the fair servers as the test finds them: none without debugfs. */
static void server_init(void)
{
	int i;

	server_found = calloc(2 * ncpus, sizeof(*server_found));
	for (i = 0; server_found && i < ncpus; i++) {
		if (server_get(online[i], "runtime", &server_found[2 * i]) ||
		    server_get(online[i], "period", &server_found[2 * i + 1])) {
			free(server_found);
			server_found = NULL;
			ksft_print_msg("no fair server in debugfs: server cases skipped\n");
		}
	}
}

/* Put the fair servers back as the test found them. */
static void server_restore(void)
{
	uint64_t runtime, period;
	int i;

	for (i = 0; server_found && i < ncpus; i++) {
		int cpu = online[i], err;

		if (server_get(cpu, "runtime", &runtime) || server_get(cpu, "period", &period))
			continue;
		if (runtime == server_found[2 * i] && period == server_found[2 * i + 1])
			continue;
		/*
		 * Retry for up to 1s. While departing tasks still hold
		 * their bandwidth, a root domain can refuse any change of a
		 * server, a decrease too. Their 0-lag times are a period or
		 * two away.
		 */
		err = wait_for(1000, !server_set(cpu, server_found[2 * i],
						 server_found[2 * i + 1]));
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

/*
 * Start @n more holders. Return 0 once they are all admitted, or -1. The
 * caller must call release_held(), on a failure too.
 */
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
			nheld++;	/* release_held() joins it */
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
	/* Drain the semaphore counts a late or refused holder left. */
	while (held && (!sem_trywait(&held_up) || !sem_trywait(&held_go)))
		;
	nheld = 0;
	sleep_ns(3 * PERIOD);	/* their 0-lag times */
}

/*
 * Change the fair server of P's CPU at runtime. Its bandwidth comes out of
 * what P's root domain has for deadline tasks. From 10% every second, P
 * must admit two probe tasks fewer at 30%, and one fewer at 100ms every
 * 500ms. With the server off, it must admit one more. A change that P has
 * no room for must be refused.
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
	int cpu = top_cpu(1), base, err, i, ret = KSFT_PASS;
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
	 * With base - 1 probe tasks held, P has room for 10% more, and not
	 * for 20%. So the server may grow to 20%, and not to 30%.
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
 * A rebuild of the root domains' accounting must count each fair server
 * as it is set, not as it was at boot. Check it for:
 *  - P's server, as Q is made and unmade beside P.
 *  - The server of P's CPU, in the root domain of the other CPUs once P
 *    is unmade.
 *  - P's server again, once P is made anew.
 */
static int server_rebuilds(void)
{
	int cpu = top_cpu(1), base, n10, n30;

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
	if (make_partition(cg_q, top_cpu(2))) {
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
	/* Unmake P: its CPU and its server join the other CPUs. */
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

/* Wait for the futex @f, in shared memory, to be set. */
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
 * Fork a process of P that spins while mv_state[4 + @rt] is set, and
 * sleeps otherwise. It is a fair task, or a real-time one if @rt. The
 * real-time one sleeps 1ms every 50ms, to let the real-time kthreads of
 * P's CPU run: PREEMPT_RT runs RCU and the timers there. Return the pid,
 * or -1.
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

/* Return the CPU share of process @pid over @ns, in percent, or -1. */
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

#define STARVED_SLACK	8	/* % a starved task may be off its server's */

/*
 * Change the fair server of P's CPU, from outside P, in each state a
 * change can find it in:
 *  - stopped
 *  - deferred while a fair task of P runs
 *  - deferred while a real-time task starves that fair task
 *  - serving it
 *  - deferred again after serving it
 *  - deferred while the CPU idles
 *
 * Each change starts a period of the server. The server defers serving a
 * starved task to its zero-laxity time. That is 0.7s into the period at
 * 30%, and 0.9s at 10%. Afterwards, the starved task must get the
 * server's bandwidth, at 30% then at 10%. P's free bandwidth must then be
 * what it was.
 *
 * A change cannot find the server in two more states. Throttled after
 * serving, the server is deferred anew at once: the last of the served
 * task's runtime is charged to it as background running. And on a CPU
 * with no tick in idle, its idle wait begins and ends in one timer
 * callback.
 */
static int server_states(void)
{
	static const struct {
		int fair, rt, ms;
	} step[] = {
		{ 0, 0, 1500 },	/* stopped: nothing of P ran for a period */
		{ 1, 0, 300 },	/* deferred while the fair task runs */
		{ 1, 1, 300 },	/* deferred, fair task starved: before 0.9s */
		{ 1, 1, 800 },	/* serving it: after 0.7s */
		{ 1, 1, 1500 },	/* deferred again: served from 0.9s to 1s */
		{ 0, 0, 600 },	/* deferred while the CPU idles */
	};
	int cpu = top_cpu(1), base, i, s30 = -1, s10 = -1, ret = KSFT_PASS;
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
	/*
	 * Even steps set the server's runtime to 30%, odd ones back to 10%.
	 * The period stays.
	 */
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
 * A reclaiming deadline task of P gets the share of P's CPU that its root
 * domain leaves free. P's fair server takes from that share. The task
 * must get:
 *  - less once the server grows from 10% to 30%.
 *  - as much after a rebuild of the accounting (Q made and unmade).
 *  - what it had at first once the server is back at 10%.
 *
 * Then put the server back as the test found it, so that P's free
 * bandwidth is what it was.
 */
static int server_reclaim(void)
{
	int cpu = top_cpu(1), s10, s30, rebuilt, back, ret = KSFT_PASS;
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
	if (make_partition(cg_q, top_cpu(2)))
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
#define WAKE_BURST	2000			/* changes per clock reading */

/* Read this_bw and running_bw of @cpu from sched_debug. Return 0, or -1. */
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
		else if (in && !strncmp(line, "dl_rq[", 6))
			break;			/* the next CPU's */
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
 * params_waking: Q is a partition root of two CPUs, taken by two deadline
 * tasks with earlier deadlines. A third deadline task of Q sleeps and
 * wakes. It wakes on whichever of the two CPUs has the later deadline,
 * which moves it. Its parameters change meanwhile, from a CPU outside Q.
 *
 * A wakeup that moves a task takes its bandwidth off the runqueue it
 * slept on. The enqueue on the new runqueue charges it there, and may be
 * left to the CPU it wakes on. A change in between must not take the
 * bandwidth off either runqueue. Once every task of the case is gone,
 * this_bw of both CPUs must be what it was before.
 *
 * Check this_bw only then. sched_debug prints running_bw and this_bw of a
 * CPU one after the other, with no lock held in between. Every parameter
 * change takes a task's bandwidth off its runqueue and puts it back. So a
 * reading taken while the changes go on can straddle one of them, and
 * tells nothing.
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

/*
 * Fork a process that changes the parameters of @pid over and over, until
 * wk_state[2] is set. Return its pid, or -1.
 */
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
	int cpu_a = top_cpu(3), cpu_b = top_cpu(2), i, ret = KSFT_PASS, changes = 0;
	long ta, ra, tb, rb, ta2, ra2, tb2, rb2;
	pid_t pid[3] = { -1, -1, -1 }, changer = -1;
	uint64_t end;
	char cpus[32];

	if (ncpus < 4) {
		ksft_print_msg("needs 4 non-isolated CPUs\n");
		return KSFT_SKIP;
	}
	snprintf(cpus, sizeof(cpus), "%d,%d", cpu_a, cpu_b);
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
#define MIX_SEED	20261103	/* fixed: a rerun starts with the same draws */

/*
 * random_mix: each worker takes random transitions of its own, over and
 * over:
 *  - into SCHED_DEADLINE, with random parameters and flags
 *  - out of SCHED_DEADLINE
 *  - asleep
 *  - spinning, at times past its runtime
 *  - through sched_yield()
 *  - through shm_lock, the mutex of the priority inheritance cases
 *  - exit
 *
 * The case changes the workers from outside too. It sets their
 * parameters, moves them between the cgroups and kills them. It also
 * changes the fair servers. It starts anew every worker it finds dead.
 *
 * The case does not check what a single transition does: the cases above
 * do that. It checks that the accounting is whole afterwards. Once every
 * worker is gone and its 0-lag time is past, each of the three root
 * domains must admit what it admitted before. this_bw of the three CPUs
 * must be what it was. An over- or underflow of running_bw or this_bw
 * also shows as a warning in the kernel log, which the test does not
 * read.
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
		 * Ignore the errors. Each call below is made in whatever
		 * state the worker is in, and the kernel may refuse it.
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
				if (err == EOWNERDEAD)	/* its owner died */
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
	int cpu_a = top_cpu(3), cpu_b = top_cpu(2), cpu_p = top_cpu(1);
	int base_p, base_q, base_r, i, ret = KSFT_PASS, rounds = 0, sums;
	long ta = 0, ra, tb = 0, rb, tp = 0, rp, ta2, ra2, tb2, rb2, tp2, rp2;
	pid_t pid[MIX_WORKERS] = { -1, -1, -1, -1, -1, -1 };
	unsigned int seed = MIX_SEED;
	uint64_t end;
	char cpus[32];

	if (ncpus < 4) {
		ksft_print_msg("needs 4 non-isolated CPUs\n");
		return KSFT_SKIP;
	}
	snprintf(cpus, sizeof(cpus), "%d,%d", cpu_a, cpu_b);
	if (make_partition_cpus(cg_q, cpus)) {
		remove_partition(cg_q);
		ksft_print_msg("no partition root of two CPUs\n");
		return KSFT_SKIP;
	}
	if (shm_lock_init()) {
		remove_partition(cg_q);
		return KSFT_SKIP;	/* no shared mutex for the workers */
	}
	/* Without this_bw in sched_debug, check the admissions only. */
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
			/* Reaped already: ESRCH. It starts anew below. */
			if (kill(pid[w], SIGKILL) && errno != ESRCH)
				ksft_print_msg("kill %d: %s\n", pid[w], strerror(errno));
			break;
		default:
			server_set(r % 2 ? cpu_p : cpu_a,
				   (r % 3 + 1) * SERVER_RUNTIME, SERVER_PERIOD);
			break;
		}
		sleep_ns((r % 2000 + 100) * NSEC_PER_USEC);
		/* Start anew each worker that exited or was killed. */
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
	     admits_in(root, base_r, "the root domain of the rest after the mix")))
		ret = KSFT_FAIL;
	if (ret != KSFT_PASS)
		ksft_print_msg("seed %u, %d transitions\n", MIX_SEED, rounds);
	remove_partition(cg_q);
	return ret;
}

/* CPU hotplug, from the main process, with no P */

static int hp_cpu = -1;

/* Return whether the online file of @cpu can be written. */
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

/* Bring @cpu back online: every case that follows needs it. */
static void cpu_up(int cpu)
{
	int err = cpu_online(cpu, 1);

	if (err)
		ksft_print_msg("CPU %d cannot come back online: %s\n", cpu, strerror(-err));
}

/*
 * Take a CPU offline and back with four deadline sleepers of 30%. Once
 * they are gone, the free bandwidth must be what it was. The sleepers
 * take the room of 12 probe tasks, and the offline that of about 10.
 */
static int hotplug_sleepers(void)
{
	int base = probe(NULL), after, i, ret = KSFT_PASS;
	pid_t pid[4];

	if (base < 22) {
		ksft_print_msg("needs room for 22 probe tasks\n");
		return KSFT_SKIP;
	}
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

/*
 * Take most of the free bandwidth with deadline sleepers. Taking a CPU
 * offline must then be refused with EBUSY.
 */
static int hotplug_refused(void)
{
	int base = probe(NULL), n, i, ret = KSFT_PASS, err;
	pid_t *pid;

	if (base < 12) {
		ksft_print_msg("needs room for 12 probe tasks\n");
		return KSFT_SKIP;
	}
	/*
	 * Sleepers of 30% take the free bandwidth but 30% to 60% of a CPU.
	 * That is less than a CPU, so the offline must be refused.
	 */
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
 * A deadline task that is not asleep leaves the class before its 0-lag
 * time. Then the CPU it is on goes offline, before that time too.
 */
static int hotplug_departed(void)
{
	int base = probe(NULL), base_off, after, cpu, ret = KSFT_PASS;
	pid_t pid;

	/*
	 * Probe the root domain with the CPU offline and no task of ours,
	 * as the reference. The case checks against it while the CPU is
	 * still offline, when the departed task's bandwidth is released.
	 * The online that follows would rebuild the accounting over it.
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
	/* Not asleep: its bandwidth stays until its 0-lag time. */
	if (set_other(pid))
		ret = KSFT_FAIL;
	cpu = task_cpu_of(pid);
	if (cpu <= 0 || !cpu_hotpluggable(cpu)) {
		cpu_set_t set;

		/* Out of the class now, it may be pinned to hp_cpu. */
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
 * Read dl_bw->total_bw of @cpu from sched_debug: the bandwidth that the
 * root domain of @cpu counts. Return it, or -1.
 */
static long long total_bw_of(int cpu)
{
	char line[256], want[32];
	FILE *f = fopen("/sys/kernel/debug/sched/debug", "r");
	long long bw = -1;
	int in = 0;

	if (!f)
		return -1;
	snprintf(want, sizeof(want), "dl_rq[%d]:", cpu);
	while (fgets(line, sizeof(line), f)) {
		char *val = strchr(line, ':');

		if (!strncmp(line, want, strlen(want)))
			in = 1;
		else if (in && !strncmp(line, "dl_rq[", 6))
			break;			/* the next CPU's */
		else if (in && val && strstr(line, ".dl_bw->total_bw") &&
			 sscanf(val + 1, "%lld", &bw) == 1)
			break;
	}
	fclose(f);
	return bw;
}

#define ZOMBIE_RUNTIME	(200 * NSEC_PER_MSEC)
#define ZOMBIE_PERIOD	(2 * NSEC_PER_SEC)

/*
 * Q is a partition root of two CPUs. A deadline task of Q runs most of its
 * runtime and exits, its 0-lag time ahead. It stays a zombie, so a rebuild
 * of the accounting still finds it in the task list. Its bandwidth stays
 * in Q until its 0-lag time. Take each CPU of Q offline and back. One of
 * them is the CPU recorded with the bandwidth. Its offline drops the
 * bandwidth. The release at the 0-lag time then removes nothing. So no
 * later rebuild may count the bandwidth again. Before the 0-lag time,
 * total_bw of Q must be what it was without the task.
 *
 * The case runs with the cases in P, from the main process: Q needs the
 * cpuset controller. Q has two CPUs because the last CPU of a root domain
 * that holds deadline bandwidth cannot go offline.
 */
static int death_zombie_hotplug(void)
{
	int cpu[2] = { top_cpu(3), top_cpu(2) }, i, ret = KSFT_PASS;
	long long base, bw, held, after;
	char cpus[32];
	uint64_t t;
	pid_t pid;

	if (ncpus < 4 || !cpu_hotpluggable(cpu[0]) || !cpu_hotpluggable(cpu[1])) {
		ksft_print_msg("needs 4 non-isolated CPUs, 2 hotpluggable beside P\n");
		return KSFT_SKIP;
	}
	snprintf(cpus, sizeof(cpus), "%d,%d", cpu[0], cpu[1]);
	if (make_partition_cpus(cg_q, cpus)) {
		ksft_print_msg("no partition root Q of CPUs %s\n", cpus);
		remove_partition(cg_q);
		return KSFT_SKIP;
	}
	base = total_bw_of(cpu[0]);
	if (base < 0) {
		ksft_print_msg("no total_bw in /sys/kernel/debug/sched/debug\n");
		remove_partition(cg_q);
		return KSFT_SKIP;
	}
	bw = (ZOMBIE_RUNTIME << 20) / ZOMBIE_PERIOD;	/* as to_ratio() */
	mv_state[0] = 0;
	pid = fork_child("a deadline task that exits");
	if (!pid) {
		if (enter(cg_q, getpid()) || set_deadline(0, ZOMBIE_RUNTIME, ZOMBIE_PERIOD))
			_exit(1);
		/* 10ms left: the 0-lag time is 100ms before the deadline. */
		spin_ns(ZOMBIE_RUNTIME - 10 * NSEC_PER_MSEC);
		mv_state[0] = 1;
		_exit(0);
	}
	if (pid < 0 || child_up("a task of Q that exits", 0) ||
	    wait_for(1000, proc_state_of(pid) == 'Z')) {
		ksft_print_msg("no zombie deadline task\n");
		reap(pid);
		remove_partition(cg_q);
		return KSFT_FAIL;
	}
	/* Its 0-lag time is about 1.7s ahead. Check total_bw within 1s. */
	t = now_ns();
	held = total_bw_of(cpu[0]);
	if (held != base + bw) {
		ksft_print_msg("total_bw of Q: %lld with the zombie, %lld expected\n",
			       held, base + bw);
		ret = KSFT_FAIL;
	}
	for (i = 0; i < 2 && ret == KSFT_PASS; i++) {
		if (cpu_online(cpu[i], 0) || cpu_online(cpu[i], 1)) {
			ksft_print_msg("offline and online of CPU %d failed\n", cpu[i]);
			cpu_up(cpu[i]);
			ret = KSFT_FAIL;
		}
	}
	after = total_bw_of(cpu[0]);
	if (ret == KSFT_PASS && now_ns() - t > NSEC_PER_SEC) {
		ksft_print_msg("the hotplugs took over 1s: the 0-lag time may be past\n");
		ret = KSFT_SKIP;
	} else if (ret == KSFT_PASS && after != base) {
		ksft_print_msg("total_bw of Q after the hotplugs: %lld, %lld expected%s\n",
			       after, base,
			       after == base + bw ? ": the zombie counted again" : "");
		ret = KSFT_FAIL;
	}
	reap(pid);
	sleep_ns(ZOMBIE_PERIOD);		/* past its 0-lag time */
	after = total_bw_of(cpu[0]);
	if (ret == KSFT_PASS && after != base) {
		ksft_print_msg("total_bw of Q past the 0-lag time: %lld, %lld expected\n",
			       after, base);
		ret = KSFT_FAIL;
	}
	remove_partition(cg_q);
	return ret;
}

/*
 * Fork the owner of pi_departed_moved(). It takes shm_lock, becomes a
 * deadline task and runs most of its runtime. It sets mv_state[0] to 1
 * and sleeps 300ms. Then it sets mv_state[1] to 1 and sleeps with the
 * lock. Return its pid, or -1.
 */
static pid_t departed_owner(void)
{
	pid_t pid = fork_child("the owner of the lock");

	if (!pid) {
		if (pthread_mutex_lock(shm_lock) ||
		    set_deadline(0, ZOMBIE_RUNTIME, ZOMBIE_PERIOD)) {
			mv_state[0] = -1;
			_exit(1);
		}
		/* 20ms left: the 0-lag time is 200ms before the deadline. */
		spin_ns(ZOMBIE_RUNTIME - 20 * NSEC_PER_MSEC);
		mv_state[0] = 1;
		sleep_ns(300 * NSEC_PER_MSEC);
		mv_state[1] = 1;
		for (;;)
			pause();
	}
	return pid;
}

/*
 * A deadline task leaves the class asleep, its 0-lag time ahead. Its root
 * domain keeps its bandwidth until then. Move the task into P and let it
 * run there. Then a deadline waiter boosts it, and dies. When the boost
 * ends, the bandwidth must still be counted in the root domain the task
 * left, and P must not lose it. Past the 0-lag time, both root domains
 * must count what they did before.
 */
static int pi_departed_moved(void)
{
	int p_cpu = top_cpu(1), r_cpu = online[0], ret = KSFT_PASS;
	long long base_p, base_r, bw, in_p, in_r;
	pid_t owner, waiter = -1;

	base_p = total_bw_of(p_cpu);
	base_r = total_bw_of(r_cpu);
	if (base_p < 0 || base_r < 0) {
		ksft_print_msg("no total_bw in /sys/kernel/debug/sched/debug\n");
		return KSFT_SKIP;
	}
	if (shm_lock_init())
		return KSFT_SKIP;
	bw = (ZOMBIE_RUNTIME << 20) / ZOMBIE_PERIOD;	/* as to_ratio() */
	mv_state[1] = 0;
	owner = departed_owner();
	if (owner < 0)
		return KSFT_FAIL;
	if (child_up("the owner of the lock", 0)) {
		reap(owner);
		return KSFT_FAIL;
	}
	sleep_ns(20 * NSEC_PER_MSEC);	/* asleep, about 1.6s before its 0-lag time */
	if (set_other(owner) || enter(cg_p, owner)) {
		ksft_print_msg("the owner cannot leave the class and enter P\n");
		ret = KSFT_FAIL;
		goto out;
	}
	if (wait_state(&mv_state[1], 1) || task_cpu_of(owner) != p_cpu) {
		ksft_print_msg("the owner did not run in P\n");
		ret = KSFT_FAIL;
		goto out;
	}
	waiter = lock_waiter();
	if (waiter < 0) {
		ret = KSFT_FAIL;
		goto out;
	}
	if (wait_state(&mv_state[2], 1)) {
		ksft_print_msg("no waiter\n");
		ret = KSFT_FAIL;
		goto out;
	}
	sleep_ns(50 * NSEC_PER_MSEC);	/* blocked, boosting the owner */
	reap(waiter);
	waiter = -1;
	sleep_ns(150 * NSEC_PER_MSEC);	/* the waiter is past its 0-lag time */
	in_p = total_bw_of(p_cpu);
	in_r = total_bw_of(r_cpu);
	if (in_p != base_p || in_r != base_r + bw) {
		ksft_print_msg("after the boost: total_bw of P %lld, %lld expected\n",
			       in_p, base_p);
		ksft_print_msg("after the boost: total_bw outside %lld, %lld expected\n",
			       in_r, base_r + bw);
		ret = KSFT_FAIL;
	}
out:
	reap(waiter);
	reap(owner);
	sleep_ns(ZOMBIE_PERIOD);		/* past the owner's 0-lag time */
	in_p = total_bw_of(p_cpu);
	in_r = total_bw_of(r_cpu);
	if (ret == KSFT_PASS && (in_p != base_p || in_r != base_r)) {
		ksft_print_msg("past the 0-lag time: total_bw of P %lld, %lld expected\n",
			       in_p, base_p);
		ksft_print_msg("past the 0-lag time: total_bw outside %lld, %lld expected\n",
			       in_r, base_r);
		ret = KSFT_FAIL;
	}
	return ret;
}

/*
 * Set the fair server of a CPU to 30%, then take the CPU offline and back.
 * Its bandwidth must leave the root domain with the CPU, as it does at
 * 10%. It must come back at 30% with the CPU.
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
 * A CPU that goes offline takes its fair server's bandwidth with its
 * capacity. Set the server to the admission limit less 10%. With one
 * probe task fewer held than the root domain admits, the CPU can still
 * go offline. With one more, the offline must be refused with EBUSY.
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
	if (n < 2) {
		ksft_print_msg("no room in P for two probe tasks\n");
		return KSFT_SKIP;
	}
	if (hold(n - 1)) {
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

/* Return whether @cpu is in @list, a CPU list as sysfs prints it. */
static int in_cpu_list(const char *list, int cpu)
{
	const char *p = list;

	while (*p >= '0' && *p <= '9') {
		char *end;
		long lo = strtol(p, &end, 10), hi = lo;

		if (*end == '-')
			hi = strtol(end + 1, &end, 10);
		if (cpu >= lo && cpu <= hi)
			return 1;
		p = *end == ',' ? end + 1 : end;
	}
	return 0;
}

/*
 * Wake the task of fallback_case() through @fd. Wait until it has run and
 * sleeps again. Return 0, or -1.
 */
static int fallback_wake(int fd, pid_t pid)
{
	int n = mv_state[1];

	if (write(fd, "w", 1) != 1) {
		ksft_print_msg("write to the deadline task: %s\n", strerror(errno));
		return -1;
	}
	if (wait_for(1000, mv_state[1] != n) ||
	    wait_for(1000, proc_state_of(pid) == 'S')) {
		ksft_print_msg("the deadline task did not wake up and sleep again\n");
		return -1;
	}
	return 0;
}

/*
 * The CPUs of fallback_case(): I1 and I2 are isolated by
 * isolcpus="domain", so they are in the default root domain. H is a
 * housekeeping CPU above I2, in the root domain of the housekeeping CPUs.
 * K is another housekeeping CPU.
 */
static int fb_i1 = -1, fb_i2 = -1, fb_h = -1, fb_k = -1;

/* Read the total_bw of both root domains. Return 0, or -1. */
static int fallback_totals(long long *def, long long *hk)
{
	*def = total_bw_of(fb_i2);
	*hk = total_bw_of(fb_h);
	if (*def == -1 || *hk == -1) {
		ksft_print_msg("no total_bw in /sys/kernel/debug/sched/debug\n");
		return -1;
	}
	return 0;
}

/*
 * Find I1, I2, H and K. I1 must be hotpluggable. Return 0, or -1 if the
 * CPUs do not allow the case. fb_k is set only once all four are found.
 */
static int fallback_cpus(void)
{
	int i;

	if (fb_k >= 0)
		return 0;
	/* A failed scan can leave I1, I2 or H set. Scan from scratch. */
	fb_i1 = -1;
	fb_i2 = -1;
	fb_h = -1;
	for (i = 0; i < nisol; i++) {
		if (fb_i1 < 0 && cpu_hotpluggable(isol[i]))
			fb_i1 = isol[i];
		else if (fb_i2 < 0)
			fb_i2 = isol[i];
	}
	for (i = 0; fb_i2 >= 0 && i < ncpus && fb_h < 0; i++) {
		if (online[i] > fb_i2)
			fb_h = online[i];
	}
	for (i = 0; fb_h >= 0 && i < ncpus && fb_k < 0; i++) {
		if (online[i] != fb_h)
			fb_k = online[i];
	}
	if (fb_i1 >= 0 && fb_i2 >= 0 && fb_k >= 0)
		return 0;
	fb_k = -1;
	return -1;
}

enum fb_mode { FB_ON_I1, FB_ON_K, FB_ON_H, FB_MASK_H };

/*
 * B is a cpuset of I1, I2 and H. C is a cpuset of I1, in B. Neither is a
 * partition. A deadline task sleeps in C, and I1 goes offline:
 *  - hotplug_fallback: the task sleeps on I1.
 *  - hotplug_fallback_moved: the task moved into C asleep. It still
 *    sleeps on K, where it ran before.
 *  - hotplug_fallback_in_mask: the same, but it sleeps on H. Its new
 *    mask holds H, so the rebuild counts it there.
 * The cpuset gives the task the CPUs of B, and the rebuild of the
 * accounting counts the task while it sleeps. At its wakeup,
 * select_fallback_rq() puts it on the first active CPU of B in its node:
 * I2. The root domain of the CPU it wakes on must count its bandwidth.
 *
 * mask_back_asleep moves the task into C asleep on H too. Then C gets H
 * instead of I1, with no CPU offline. The task wakes up on H with no
 * migration. The housekeeping root domain must count it then.
 *
 * Take the reference values of total_bw with I1 offline and no task.
 * Once the task is gone, total_bw must be back to them. The messages give
 * total_bw of the isolated root domain first, then of the housekeeping one.
 */
static int fallback_case(enum fb_mode mode)
{
	int on_h = mode == FB_ON_H || mode == FB_MASK_H;
	int on_i1 = mode == FB_ON_I1;
	char cg_b[PATH_MAX], cg_c[PATH_MAX], path[PATH_MAX + 32], buf[256], one[16];
	long long def0, hk0, def_off, hk_off, def_ref, hk_ref, def, hk, bw;
	int fd[2], ret = KSFT_SKIP, added = 0, made = 0, w, at;
	pid_t pid = -1;

	if (!root[0] || fallback_cpus()) {
		ksft_print_msg("needs isolcpus=domain: 2 isolated, 2 housekeeping CPUs\n");
		return KSFT_SKIP;
	}
	snprintf(path, sizeof(path), "%s/cgroup.subtree_control", root);
	if (read_file(path, buf, sizeof(buf)))
		return KSFT_SKIP;
	if (!strstr(buf, "cpuset")) {
		if (write_file(root, "cgroup.subtree_control", "+cpuset"))
			return KSFT_SKIP;
		added = 1;
	}
	if (snprintf(cg_b, sizeof(cg_b), "%s/deadline_bw_b", root) >= (int)sizeof(cg_b) ||
	    snprintf(cg_c, sizeof(cg_c), "%s/c", cg_b) >= (int)sizeof(cg_c))
		goto out;
	if (mkdir(cg_b, 0755) && errno != EEXIST) {
		ksft_print_msg("mkdir %s: %s\n", cg_b, strerror(errno));
		goto out;
	}
	made = 1;
	snprintf(buf, sizeof(buf), "%d,%d,%d", fb_i1, fb_i2, fb_h);
	snprintf(one, sizeof(one), "%d", fb_i1);
	if (write_file(cg_b, "cpuset.cpus", buf) ||
	    write_file(cg_b, "cgroup.subtree_control", "+cpuset") ||
	    (mkdir(cg_c, 0755) && errno != EEXIST) ||
	    write_file(cg_c, "cpuset.cpus", one)) {
		ksft_print_msg("cpusets B and C cannot be made\n");
		goto out;
	}
	if (fallback_totals(&def0, &hk0))
		goto out;
	ret = KSFT_FAIL;
	if (cpu_online(fb_i1, 0)) {
		ksft_print_msg("offline of CPU %d refused\n", fb_i1);
		goto out;
	}
	if (fallback_totals(&def_off, &hk_off))
		ret = KSFT_SKIP;
	cpu_up(fb_i1);
	if (ret == KSFT_SKIP)
		goto out;

	if (pipe(fd)) {
		ksft_print_msg("pipe: %s\n", strerror(errno));
		goto out;
	}
	mv_state[0] = 0;
	mv_state[1] = 0;
	pid = fork_child("a deadline task");
	if (!pid) {
		cpu_set_t set;
		int i, err;
		char c;

		/* Run on K or H, then allow every CPU again, and stay there. */
		close(fd[1]);
		CPU_ZERO(&set);
		CPU_SET(on_h ? fb_h : fb_k, &set);
		err = sched_setaffinity(0, sizeof(set), &set);
		for (i = 0; i < CPU_SETSIZE; i++)
			CPU_SET(i, &set);
		if (err || sched_setaffinity(0, sizeof(set), &set) ||
		    set_deadline(0, 10 * NSEC_PER_MSEC, PERIOD)) {
			mv_state[0] = -1;
			_exit(1);
		}
		mv_state[0] = 1;
		while (read(fd[0], &c, 1) == 1)
			mv_state[1]++;
		_exit(0);
	}
	close(fd[0]);
	if (pid < 0 || child_up("hotplug_fallback", 0))
		goto out_fd;
	if (wait_for(1000, proc_state_of(pid) == 'S') || fallback_totals(&def, &hk))
		goto out_fd;
	bw = def + hk - def0 - hk0;
	if (bw <= 0) {
		ksft_print_msg("the deadline task adds %lld to total_bw\n", bw);
		goto out_fd;
	}

	/* Move it into C asleep. Wake it so that it runs on I1, or do not. */
	if (enter(cg_c, pid) || (on_i1 && fallback_wake(fd[1], pid)))
		goto out_fd;
	at = task_cpu_of(pid);
	if (on_i1 ? at != fb_i1 : on_h ? at != fb_h : at == fb_i1) {
		ksft_print_msg("the deadline task sleeps on CPU %d\n", at);
		ret = on_h ? KSFT_SKIP : KSFT_FAIL;
		goto out_fd;
	}
	/* With no CPU offline, the reference is total_bw with no task. */
	def_ref = mode == FB_MASK_H ? def0 : def_off;
	hk_ref = mode == FB_MASK_H ? hk0 : hk_off;
	if (mode == FB_MASK_H) {
		snprintf(one, sizeof(one), "%d", fb_h);
		if (write_file(cg_c, "cpuset.cpus", one)) {
			ksft_print_msg("C cannot take CPU %d\n", fb_h);
			goto out_fd;
		}
	} else if (cpu_online(fb_i1, 0)) {
		ksft_print_msg("offline of CPU %d with the task asleep refused\n", fb_i1);
		goto out_fd;
	}
	if (fallback_wake(fd[1], pid) || fallback_totals(&def, &hk))
		goto out_up;
	w = task_cpu_of(pid);
	ret = KSFT_PASS;
	if (in_cpu_list(isolated, w) ? def != def_ref + bw || hk != hk_ref :
				     hk != hk_ref + bw || def != def_ref) {
		ksft_print_msg("woken on CPU %d: total_bw %lld, %lld, not %lld, %lld\n",
			       w, def, hk,
			       in_cpu_list(isolated, w) ? def_ref + bw : def_ref,
			       in_cpu_list(isolated, w) ? hk_ref : hk_ref + bw);
		ret = KSFT_FAIL;
	} else if (mode != FB_MASK_H && !in_cpu_list(isolated, w)) {
		ksft_print_msg("woken on housekeeping CPU %d: no fallback to another root domain\n",
			       w);
		ret = KSFT_SKIP;
	}
	reap(pid);
	pid = -1;
	sleep_ns(3 * PERIOD);
	if (fallback_totals(&def, &hk) || def != def_ref || hk != hk_ref) {
		ksft_print_msg("task gone: total_bw %lld, %lld, not %lld, %lld\n",
			       def, hk, def_ref, hk_ref);
		ret = KSFT_FAIL;
	}
out_up:
	reap(pid);
	cpu_up(fb_i1);
	pid = -1;
out_fd:
	reap(pid);
	close(fd[1]);
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS && (fallback_totals(&def, &hk) || def != def0 || hk != hk0)) {
		ksft_print_msg("task gone, CPU %d online: total_bw %lld, %lld, not %lld, %lld\n",
			       fb_i1, def, hk, def0, hk0);
		ret = KSFT_FAIL;
	}
out:
	if (made) {
		if (wait_for(1000, !rmdir(cg_c) || errno == ENOENT) ||
		    wait_for(1000, !rmdir(cg_b) || errno == ENOENT))
			ksft_print_msg("rmdir %s: %s\n", cg_b, strerror(errno));
	}
	if (added && write_file(root, "cgroup.subtree_control", "-cpuset"))
		ksft_print_msg("the cpuset controller stays enabled\n");
	return ret;
}

static int hotplug_fallback(void)
{
	return fallback_case(FB_ON_I1);
}

static int hotplug_fallback_moved(void)
{
	return fallback_case(FB_ON_K);
}

static int hotplug_fallback_in_mask(void)
{
	return fallback_case(FB_ON_H);
}

static int mask_back_asleep(void)
{
	return fallback_case(FB_MASK_H);
}

/* Moves between two root domains, from the main process, with P */

/*
 * Fork a deadline task of 30% every 100ms, pinned to the @n CPUs of @cpus.
 * It runs 1ms, then sleeps. If @fd is not -1, each byte read from @fd
 * wakes it up, and it counts the wakeups in mv_state[1]. It sets
 * mv_state[0] as mover() does.
 */
static pid_t pinned_sleeper(const int *cpus, int n, int fd)
{
	pid_t pid = fork_child("a pinned deadline task");

	if (!pid) {
		cpu_set_t set;
		char c;
		int i;

		CPU_ZERO(&set);
		for (i = 0; i < n; i++)
			CPU_SET(cpus[i], &set);
		if (sched_setaffinity(0, sizeof(set), &set) ||
		    set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD)) {
			mv_state[0] = -1;
			_exit(1);
		}
		spin_ns(NSEC_PER_MSEC);
		mv_state[0] = 1;
		while (fd >= 0 && read(fd, &c, 1) == 1)
			mv_state[1]++;
		for (;;)
			pause();
	}
	return pid;
}

/*
 * Read total_bw of the root domain of the isolated CPUs, of the other
 * CPUs but P, and of P, into @bw. Return 0, or -1.
 */
static int split_totals(long long bw[3])
{
	bw[0] = total_bw_of(isol[0]);
	bw[1] = total_bw_of(online[0]);
	bw[2] = total_bw_of(top_cpu(1));
	if (bw[0] == -1 || bw[1] == -1 || bw[2] == -1) {
		ksft_print_msg("no total_bw in /sys/kernel/debug/sched/debug\n");
		return -1;
	}
	return 0;
}

/*
 * Return KSFT_PASS if total_bw is now @want, else KSFT_FAIL. The message
 * gives total_bw of the isolated root domain, then of the other CPUs but
 * P, then of P.
 */
static int split_check(const char *what, const long long want[3])
{
	long long bw[3];

	if (split_totals(bw))
		return KSFT_FAIL;
	if (bw[0] == want[0] && bw[1] == want[1] && bw[2] == want[2])
		return KSFT_PASS;
	ksft_print_msg("%s: total_bw %lld, %lld, %lld, not %lld, %lld, %lld\n",
		       what, bw[0], bw[1], bw[2], want[0], want[1], want[2]);
	return KSFT_FAIL;
}

/*
 * Read total_bw into @base, for the cases below. Return 0, or nonzero if
 * the case must skip. It needs CPUs isolated by isolcpus="domain".
 */
static int split_skip(long long base[3])
{
	if (!nisol) {
		ksft_print_msg("needs CPUs isolated by isolcpus=domain\n");
		return 1;
	}
	return split_totals(base);
}

/*
 * The root cpuset holds the CPUs that isolcpus="domain" isolates, so it
 * spans two root domains. The affinity of a task can keep it in either.
 * A move into the root cpuset must reserve the bandwidth of the task in
 * the root domain that the task gets.
 *
 * Move a deadline sleeper into P and back, pinned to the isolated CPUs,
 * then to the other CPUs but P. Once it is gone, total_bw of the three
 * root domains must be what it was.
 */
static int move_isolated(void)
{
	long long base[3];
	int round, err;
	pid_t pid;

	if (split_skip(base))
		return KSFT_SKIP;
	for (round = 0; round < 2; round++) {
		const char *what = round ? "pinned to the other CPUs" :
				   "pinned to the isolated CPUs";

		mv_state[0] = 0;
		pid = round ? pinned_sleeper(online, ncpus - 1, -1) :
			      pinned_sleeper(isol, nisol, -1);
		if (pid < 0 || child_up(what, 0)) {
			reap(pid);
			return KSFT_FAIL;
		}
		err = enter(cg_p, pid);
		if (!err)
			err = enter(root, pid);
		reap(pid);
		if (err) {
			ksft_print_msg("%s: a move refused: %s\n", what, strerror(-err));
			return KSFT_FAIL;
		}
		sleep_ns(3 * PERIOD);
		if (split_check(what, base))
			return KSFT_FAIL;
	}
	return KSFT_PASS;
}

/*
 * Fork a process pinned to the isolated CPUs. Its threads become deadline
 * tasks of 10% every 100ms, as in probe(), until one is refused. Then it
 * sets mv_state[1] to 1, or to -1 on an error. The threads sleep until the
 * process is killed.
 */
static pid_t fill_isolated(void)
{
	pid_t pid = fork_child("the tasks that fill the isolated CPUs");

	if (!pid) {
		int i, max = 16 * nisol + 16;
		struct probe_slot *s = calloc(max, sizeof(*s));
		pthread_attr_t a;
		cpu_set_t set;

		CPU_ZERO(&set);
		for (i = 0; i < nisol; i++)
			CPU_SET(isol[i], &set);
		if (!s || sched_setaffinity(0, sizeof(set), &set) ||
		    sem_init(&probe_done, 0, 0) || pipe(probe_hold) ||
		    pthread_attr_init(&a) || pthread_attr_setstacksize(&a, THREAD_STACK)) {
			mv_state[1] = -1;
			_exit(1);
		}
		for (i = 0; i < max; i++) {
			if (pthread_create(&s[i].t, &a, probe_thread, &s[i]) ||
			    sem_wait_for(&probe_done, 10000)) {
				mv_state[1] = -1;
				_exit(1);
			}
			if (s[i].err)
				break;
		}
		mv_state[1] = i < max && s[i].err == -EBUSY ? 1 : -1;
		for (;;)
			pause();
	}
	return pid;
}

/*
 * Move a deadline sleeper pinned to the isolated CPUs into P. Fill the
 * isolated CPUs with deadline tasks of 10%, until one is refused. Moving
 * the sleeper back must then be refused with EBUSY.
 */
static int move_isolated_refused(void)
{
	pid_t pid, fill = -1;
	long long base[3];
	int err, ret = KSFT_FAIL;

	if (split_skip(base))
		return KSFT_SKIP;
	mv_state[0] = 0;
	pid = pinned_sleeper(isol, nisol, -1);
	if (pid < 0 || child_up("a sleeper pinned to the isolated CPUs", 0))
		goto out;
	err = enter(cg_p, pid);
	if (err) {
		ksft_print_msg("a move into P refused: %s\n", strerror(-err));
		goto out;
	}
	mv_state[1] = 0;
	fill = fill_isolated();
	if (fill < 0 || wait_for(10000, mv_state[1]) || mv_state[1] != 1) {
		ksft_print_msg("the isolated CPUs cannot be filled\n");
		goto out;
	}
	err = enter(root, pid);
	if (err == -EBUSY) {
		ret = KSFT_PASS;
	} else {
		ksft_print_msg("a move back to the full isolated CPUs: %s, not EBUSY\n",
			       err ? strerror(-err) : "accepted");
	}
out:
	reap(fill);
	reap(pid);
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS)
		ret = split_check("the isolated CPUs filled", base);
	return ret;
}

/*
 * A deadline sleeper pinned to the isolated CPUs is allowed all CPUs. It
 * is moved into P and back, and is not woken. So it stays on an isolated
 * CPU, which its mask holds again. The root domain of that CPU must count
 * its bandwidth. Then wake it, and check that again.
 */
static int move_isolated_asleep(void)
{
	long long base[3], want[3];
	int fd[2], err, cpu, i, ret = KSFT_FAIL;
	cpu_set_t set;
	pid_t pid;

	if (split_skip(base))
		return KSFT_SKIP;
	if (pipe(fd)) {
		ksft_print_msg("pipe: %s\n", strerror(errno));
		return KSFT_FAIL;
	}
	mv_state[0] = 0;
	mv_state[1] = 0;
	pid = pinned_sleeper(isol, nisol, fd[0]);
	if (pid < 0 || child_up("a sleeper pinned to the isolated CPUs", 0) ||
	    wait_for(1000, proc_state_of(pid) == 'S') || split_totals(want))
		goto out;
	CPU_ZERO(&set);
	for (i = 0; i < CPU_SETSIZE; i++)
		CPU_SET(i, &set);
	err = sched_setaffinity(pid, sizeof(set), &set) ? -errno : 0;
	if (!err)
		err = enter(cg_p, pid);
	if (!err)
		err = enter(root, pid);
	if (err) {
		ksft_print_msg("the affinity of all CPUs, or a move: %s\n", strerror(-err));
		goto out;
	}
	for (i = 0; i < 2; i++) {
		const char *what = i ? "woken" : "moved back asleep";

		cpu = task_cpu_of(pid);
		if (!in_cpu_list(isolated, cpu)) {
			ksft_print_msg("%s on CPU %d, not an isolated one\n", what, cpu);
			goto out;
		}
		if (split_check(what, want))
			goto out;
		if (!i && fallback_wake(fd[1], pid))
			goto out;
	}
	ret = KSFT_PASS;
out:
	reap(pid);
	close(fd[0]);
	close(fd[1]);
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS)
		ret = split_check("task gone", base);
	return ret;
}

/* The CPUs of move_spanning(): two root domains, an isolated CPU first */
static cpu_set_t span_set;

/*
 * Fork a deadline task of 30% every 100ms. It becomes one on the isolated
 * CPUs, then takes the CPUs of span_set. If @fd is -1, it spins and counts
 * in mv_state[1]. Else it sleeps, and each byte read from @fd wakes it up.
 * It sets mv_state[0] as mover() does.
 */
static pid_t spanning_task(int fd)
{
	pid_t pid = fork_child("a deadline task on two root domains");

	if (!pid) {
		cpu_set_t set;
		char c;
		int i;

		CPU_ZERO(&set);
		for (i = 0; i < nisol; i++)
			CPU_SET(isol[i], &set);
		if (sched_setaffinity(0, sizeof(set), &set) ||
		    set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD) ||
		    sched_setaffinity(0, sizeof(span_set), &span_set)) {
			mv_state[0] = -1;
			_exit(1);
		}
		mv_state[0] = 1;
		while (fd < 0)
			mv_state[1]++;
		while (read(fd, &c, 1) == 1)
			mv_state[1]++;
		for (;;)
			pause();
	}
	return pid;
}

/*
 * A deadline task may run on the isolated CPUs and on the other CPUs
 * above the first isolated one, but P. In the root cpuset, its CPUs then
 * span two root domains, and the first one is isolated. Run it in P, and
 * move it back. It must go to a housekeeping CPU, and the root domain of
 * that CPU must count its bandwidth. Do it with a running task, then with
 * a sleeper woken after the move.
 */
static int move_spanning(void)
{
	int fd[2] = { -1, -1 }, round, cpu, err, i, n = 0, ret = KSFT_PASS;
	long long base[3], want[3];
	pid_t pid;

	if (split_skip(base))
		return KSFT_SKIP;
	CPU_ZERO(&span_set);
	for (i = 0; i < nisol; i++)
		CPU_SET(isol[i], &span_set);
	for (i = 0; i < ncpus - 1; i++) {
		if (online[i] > isol[0]) {
			CPU_SET(online[i], &span_set);
			n++;
		}
	}
	if (!n) {
		ksft_print_msg("needs a CPU above the first isolated one, besides P\n");
		return KSFT_SKIP;
	}
	for (round = 0; round < 2 && ret == KSFT_PASS; round++) {
		const char *what = round ? "a sleeper" : "a running task";

		ret = KSFT_FAIL;
		if (round && pipe(fd)) {
			ksft_print_msg("pipe: %s\n", strerror(errno));
			break;
		}
		mv_state[0] = 0;
		mv_state[1] = 0;
		pid = spanning_task(fd[0]);
		if (pid < 0 || child_up(what, 0) || split_totals(want))
			goto next;
		/* Moved back, its bandwidth leaves the isolated CPUs. */
		want[1] += want[0] - base[0];
		want[0] = base[0];
		err = enter(cg_p, pid);
		if (err) {
			ksft_print_msg("%s: a move into P refused: %s\n", what, strerror(-err));
			goto next;
		}
		if ((round && fallback_wake(fd[1], pid)) ||
		    wait_for(1000, task_cpu_of(pid) == top_cpu(1))) {
			ksft_print_msg("%s: not on P's CPU\n", what);
			goto next;
		}
		err = enter(root, pid);
		if (err) {
			ksft_print_msg("%s: the move back refused: %s\n", what, strerror(-err));
			goto next;
		}
		if (round ? fallback_wake(fd[1], pid) :
			    wait_for(1000, task_cpu_of(pid) != top_cpu(1)))
			goto next;
		sleep_ns(PERIOD);
		cpu = task_cpu_of(pid);
		if (in_cpu_list(isolated, cpu)) {
			ksft_print_msg("%s: moved back to isolated CPU %d\n", what, cpu);
			goto next;
		}
		ret = split_check(what, want);
next:
		reap(pid);
		if (round) {
			close(fd[0]);
			close(fd[1]);
		}
		sleep_ns(3 * PERIOD);
		if (ret == KSFT_PASS)
			ret = split_check("task gone", base);
	}
	return ret;
}

/*
 * A deadline task moved asleep into another cpuset still sleeps on a CPU
 * of the root domain it left. The root domain it moved to counts its
 * bandwidth. sched_setaffinity() must check a new mask against that one.
 *
 * H is a cpuset of the other CPUs but P. The steps:
 * 1. Pin a fair task to the isolated CPUs.
 * 2. Move it into H. Its affinity does not meet H, so it gets all the
 *    CPUs of H.
 * 3. Make it a deadline task there, and let it sleep.
 * 4. Move it asleep into the root cgroup. Its affinity gives it the
 *    isolated CPUs again, and their root domain counts it.
 * 5. Set its affinity to the CPUs of H. That would take it out of the
 *    root domain that counts it, so it must fail with EBUSY.
 *
 * If the change is accepted, wake the task and report where it runs.
 */
static int affinity_moved_asleep(void)
{
	char cg_h[PATH_MAX], cpus[4096], what[64];
	int fd[2], i, n = 0, cpu, err, made = 0, ret = KSFT_SKIP;
	long long base[3], want[3], bw;
	cpu_set_t set;
	pid_t pid = -1;

	if (split_skip(base))
		return KSFT_SKIP;
	if (pipe(fd)) {
		ksft_print_msg("pipe: %s\n", strerror(errno));
		return KSFT_FAIL;
	}
	/*
	 * H holds the other CPUs but P. A task there can become a deadline
	 * task: its mask holds its whole root domain.
	 */
	CPU_ZERO(&set);
	for (i = 0; i < ncpus - 1 && n < (int)sizeof(cpus); i++) {
		n += snprintf(cpus + n, sizeof(cpus) - n, "%s%d", i ? "," : "", online[i]);
		CPU_SET(online[i], &set);
	}
	if (snprintf(cg_h, sizeof(cg_h), "%s/deadline_bw_h", root) >= (int)sizeof(cg_h))
		goto out;
	if (mkdir(cg_h, 0755) && errno != EEXIST) {
		ksft_print_msg("mkdir %s: %s\n", cg_h, strerror(errno));
		goto out;
	}
	made = 1;
	if (n >= (int)sizeof(cpus) || write_file(cg_h, "cpuset.cpus", cpus)) {
		ksft_print_msg("cpuset H cannot be made\n");
		goto out;
	}
	ret = KSFT_FAIL;
	mv_state[0] = 0;
	mv_state[1] = 0;
	pid = fork_child("a task moved asleep");
	if (!pid) {
		cpu_set_t iso;
		char c;

		CPU_ZERO(&iso);
		for (i = 0; i < nisol; i++)
			CPU_SET(isol[i], &iso);
		if (sched_setaffinity(0, sizeof(iso), &iso)) {
			mv_state[1] = -1;
			_exit(1);
		}
		mv_state[1] = 1;
		/* Wait for the move into H. Become a deadline task there. */
		if (read(fd[0], &c, 1) != 1 || set_deadline(0, 30 * NSEC_PER_MSEC, PERIOD)) {
			mv_state[0] = -1;
			_exit(1);
		}
		spin_ns(NSEC_PER_MSEC);
		mv_state[0] = 1;
		while (read(fd[0], &c, 1) == 1)
			mv_state[1]++;
		for (;;)
			pause();
	}
	if (pid < 0)
		goto out;
	if (wait_for(1000, mv_state[1]) || mv_state[1] != 1) {
		ksft_print_msg("the task cannot take the isolated CPUs\n");
		goto out;
	}
	err = enter(cg_h, pid);
	if (err) {
		ksft_print_msg("a move into H refused: %s\n", strerror(-err));
		goto out;
	}
	if (write(fd[1], "d", 1) != 1) {
		ksft_print_msg("write to the task: %s\n", strerror(errno));
		goto out;
	}
	if (child_up("a task moved into H", 0) || wait_for(1000, proc_state_of(pid) == 'S'))
		goto out;
	bw = (30 * NSEC_PER_MSEC << 20) / PERIOD;	/* as to_ratio() */
	memcpy(want, base, sizeof(want));
	want[1] += bw;
	if (split_check("a deadline task in H", want))
		goto out;

	/*
	 * Moved asleep, it stays on its CPU. The isolated root domain
	 * counts it.
	 */
	err = enter(root, pid);
	if (err) {
		ksft_print_msg("the move into the root cgroup refused: %s\n", strerror(-err));
		goto out;
	}
	cpu = task_cpu_of(pid);
	if (in_cpu_list(isolated, cpu)) {
		ksft_print_msg("moved asleep on CPU %d, an isolated one\n", cpu);
		goto out;
	}
	want[0] += bw;
	want[1] -= bw;
	if (split_check("moved asleep", want))
		goto out;

	/* The CPUs of H leave out the root domain that counts the task. */
	err = sched_setaffinity(pid, sizeof(set), &set) ? -errno : 0;
	if (err == -EBUSY) {
		ret = KSFT_PASS;
		goto out;
	}
	ksft_print_msg("the affinity of the CPUs of H: %s, not EBUSY\n",
		       err ? strerror(-err) : "accepted");
	if (err || fallback_wake(fd[1], pid))
		goto out;
	/* Report its CPU, and whether the root domain of that CPU counts it. */
	cpu = task_cpu_of(pid);
	memcpy(want, base, sizeof(want));
	want[in_cpu_list(isolated, cpu) ? 0 : 1] += bw;
	snprintf(what, sizeof(what), "woken on CPU %d", cpu);
	if (split_check(what, want) == KSFT_PASS)
		ksft_print_msg("%s: its root domain counts it\n", what);
out:
	reap(pid);
	close(fd[0]);
	close(fd[1]);
	sleep_ns(3 * PERIOD);
	if (ret == KSFT_PASS)
		ret = split_check("task gone", base);
	if (made && wait_for(1000, !rmdir(cg_h) || errno == ENOENT))
		ksft_print_msg("rmdir %s: %s\n", cg_h, strerror(errno));
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
	{ "pi_departed_moved", pi_departed_moved, FROM_OUTSIDE },
	{ "params_asleep", params_asleep, IN_P },
	{ "params_non_contending", params_non_contending, MEASURED_IN_P },
	{ "params_throttled", params_throttled, MEASURED_IN_P },
	{ "params_refused", params_refused, IN_P },
	{ "params_keep_class_change", params_keep_class_change, MEASURED_IN_P },
	{ "params_keep_boosted", params_keep_boosted, MEASURED_IN_P },
	{ "boosted_owner_policy", boosted_owner_policy, MEASURED_IN_P },
	{ "deboost_resets_period", deboost_resets_period, MEASURED_IN_P },
	{ "deboost_released_resets_period", deboost_released_resets_period, MEASURED_IN_P },
	{ "deboost_in_class_resets_period", deboost_in_class_resets_period, MEASURED_IN_P },
	{ "return_running_new_params", return_running_new_params, MEASURED_IN_P },
	{ "return_asleep_new_params", return_asleep_new_params, MEASURED_IN_P },
	{ "change_running_new_params", change_running_new_params, MEASURED_IN_P },
	{ "deboost_blocked_keeps_order", deboost_blocked_keeps_order, MEASURED_IN_P },
	{ "setattr_blocked_keeps_order", setattr_blocked_keeps_order, MEASURED_IN_P },
	{ "boost_blocked_keeps_order", boost_blocked_keeps_order, MEASURED_IN_P },
	{ "move_asleep", move_asleep, FROM_OUTSIDE },
	{ "move_throttled", move_throttled, FROM_OUTSIDE },
	{ "move_non_contending", move_non_contending, FROM_OUTSIDE },
	{ "move_refused", move_refused, FROM_OUTSIDE },
	{ "move_zero_bw", move_zero_bw, FROM_OUTSIDE },
	{ "move_vs_rebuild", move_vs_rebuild, FROM_OUTSIDE },
	{ "move_isolated", move_isolated, FROM_OUTSIDE },
	{ "move_isolated_refused", move_isolated_refused, FROM_OUTSIDE },
	{ "move_isolated_asleep", move_isolated_asleep, FROM_OUTSIDE },
	{ "move_spanning", move_spanning, FROM_OUTSIDE },
	{ "affinity_moved_asleep", affinity_moved_asleep, FROM_OUTSIDE },
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
	{ "death_zombie_switched", death_zombie_switched, FROM_OUTSIDE },
	{ "death_zombie_hotplug", death_zombie_hotplug, FROM_OUTSIDE },
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
	{ "hotplug_fallback", hotplug_fallback },
	{ "hotplug_fallback_moved", hotplug_fallback_moved },
	{ "hotplug_fallback_in_mask", hotplug_fallback_in_mask },
	{ "mask_back_asleep", mask_back_asleep },
};

#define N_HP_CASES	((int)ARRAY_SIZE(hp_cases))

/* Run the cases in P, each in a P made anew. */
static void run_p_cases(void)
{
	char path[PATH_MAX + 32], buf[256];
	int i, ret, added = 0, told = 0, ok;

	ok = !find_cgroup2() &&
	     snprintf(cg_p, sizeof(cg_p), "%s/deadline_bw_p", root) < (int)sizeof(cg_p) &&
	     snprintf(cg_q, sizeof(cg_q), "%s/deadline_bw_q", root) < (int)sizeof(cg_q);
	if (ok) {
		/*
		 * Enable the cpuset controller for the children of the
		 * root while the cases run.
		 */
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

/*
 * Read the online CPUs into *@cpus, in ascending order. Read those that
 * isolcpus="domain" isolates if @iso, the others if not. Return their
 * count, or -1.
 */
static int read_online(int **cpus, int iso)
{
	static char buf[65536];		/* a sysfs list fits in a page */
	char *p = buf;
	int n = 0, max = 0;

	if (read_file("/sys/devices/system/cpu/online", buf, sizeof(buf)) ||
	    strlen(buf) == sizeof(buf) - 1)
		return -1;
	while (*p >= '0' && *p <= '9') {
		long lo = strtol(p, &p, 10), hi = lo;

		if (*p == '-')
			hi = strtol(p + 1, &p, 10);
		for (; lo <= hi; lo++) {
			if (in_cpu_list(isolated, lo) != iso)
				continue;
			if (n == max) {
				int *more = realloc(*cpus, (max = 2 * max + 64) * sizeof(**cpus));

				if (!more)
					return -1;
				*cpus = more;
			}
			(*cpus)[n++] = lo;
		}
		if (*p == ',')
			p++;
	}
	return n;
}

int main(void)
{
	char buf[64];
	cpu_set_t set;
	int i;

	ksft_print_header();
	if (geteuid())
		ksft_exit_skip("needs root\n");
	/* No such file: nothing isolated. */
	if (read_file("/sys/devices/system/cpu/isolated", isolated, sizeof(isolated)))
		isolated[0] = 0;
	ncpus = read_online(&online, 0);
	nisol = read_online(&isol, 1);
	if (ncpus < 0 || nisol < 0)
		ksft_exit_fail_msg("the online CPUs cannot be read\n");
	if (ncpus < 3)
		ksft_exit_skip("needs 3 CPUs not isolated by isolcpus=domain\n");
	/*
	 * Keep the tasks of the test off the isolated CPUs. A deadline task
	 * there is admitted in the default root domain. The children inherit
	 * this affinity. A change of the partitions resets the CPUs of the
	 * tasks in the root cpuset, isolated ones included. The kernel keeps
	 * the new CPUs within an affinity set by sched_setaffinity().
	 */
	CPU_ZERO(&set);
	for (i = 0; i < ncpus; i++)
		CPU_SET(online[i], &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		ksft_exit_fail_msg("sched_setaffinity: %s\n", strerror(errno));
	if (read_file("/proc/sys/kernel/sched_rt_runtime_us", buf, sizeof(buf)) || atol(buf) < 0)
		ksft_exit_skip("no deadline admission control\n");
	mv_state = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (mv_state == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	shm_lock = (pthread_mutex_t *)((char *)mv_state + 256);

	ksft_set_plan(N_P_CASES + N_HP_CASES);
	server_init();
	run_p_cases();

	for (i = 1; i < ncpus && hp_cpu < 0; i++) {
		if (cpu_hotpluggable(top_cpu(i)))
			hp_cpu = top_cpu(i);
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
