// SPDX-License-Identifier: GPL-2.0
/*
 * Test the bandwidth of a SCHED_DEADLINE task that leaves the policy, or
 * dies, before its 0-lag time. The bandwidth stays in the task's root
 * domain until the inactive timer removes it. It must be removed once,
 * from the root domain that holds it.
 *
 * P is a partition root of one CPU. The cases are:
 * - dies_before_rebuild: a task of P dies. Before its 0-lag time, a
 *   second partition root Q is made. This rebuilds the accounting of
 *   every root domain.
 * - switched_out_before_rebuild: the same, but the task switches to
 *   SCHED_OTHER.
 * - moved_after_switch_out: a task outside P switches to SCHED_OTHER.
 *   Before its 0-lag time, it moves into P and dies there.
 *
 * Once the inactive timer has fired, P must admit one task of 600ms
 * every second, and refuse a second one with EBUSY. A kernel with the
 * bug removes the bandwidth from P's root domain after the rebuild
 * dropped it, or when P never had it. total_bw of that root domain then
 * wraps around, and P admits both tasks.
 *
 * The test needs root, 3 CPUs, the cgroup v2 cpuset controller and
 * deadline admission control.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../kselftest.h"

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE	6
#endif

#define NSEC_PER_MSEC	1000000ULL
#define NSEC_PER_SEC	1000000000ULL

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

static int set_deadline(pid_t pid, uint64_t runtime)
{
	struct dl_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_DEADLINE,
		.sched_runtime = runtime,
		.sched_deadline = NSEC_PER_SEC,
		.sched_period = NSEC_PER_SEC,
	};

	return syscall(__NR_sched_setattr, pid, &attr, 0) ? -errno : 0;
}

static int set_other(pid_t pid)
{
	struct sched_param sp = { .sched_priority = 0 };

	return sched_setscheduler(pid, SCHED_OTHER, &sp) ? -errno : 0;
}

/*
 * Return the CLOCK_MONOTONIC time in ns. clock_gettime() cannot fail: the
 * clock is always supported, and the pointer is valid.
 */
static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void sleep_ns(uint64_t ns)
{
	struct timespec ts = { .tv_sec = ns / NSEC_PER_SEC, .tv_nsec = ns % NSEC_PER_SEC };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static void reap(pid_t pid)
{
	int status;

	if (pid <= 0)
		return;
	if (kill(pid, SIGKILL) && errno != ESRCH)
		ksft_print_msg("kill %d: %s\n", pid, strerror(errno));
	set_other(pid);			/* gone already: nothing to switch */
	while (waitpid(pid, &status, 0) != pid) {
		if (errno == EINTR)
			continue;
		ksft_print_msg("waitpid %d: %s\n", pid, strerror(errno));
		break;
	}
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

/* Make @cg a partition root of CPU @cpu alone. Return 0, or -1 on error. */
static int make_partition(const char *cg, int cpu)
{
	char path[PATH_MAX], buf[32];

	if (mkdir(cg, 0755) && errno != EEXIST)
		return -1;
	snprintf(buf, sizeof(buf), "%d", cpu);
	if (write_file(cg, "cpuset.cpus", buf) || write_file(cg, "cpuset.cpus.partition", "root"))
		return -1;
	snprintf(path, sizeof(path), "%s/cpuset.cpus.partition", cg);
	if (read_file(path, buf, sizeof(buf)) || strcmp(buf, "root\n"))
		return -1;
	return 0;
}

static void remove_partition(const char *cg)
{
	int err = write_file(cg, "cpuset.cpus.partition", "member");
	uint64_t end;

	if (err && err != -ENOENT)
		ksft_print_msg("%s back to a member: %s\n", cg, strerror(-err));
	/* A cgroup whose last task just died takes a moment to empty. */
	end = now_ns() + NSEC_PER_SEC;
	while (rmdir(cg)) {
		if (errno == ENOENT)
			return;
		if (errno != EBUSY || now_ns() > end) {
			ksft_print_msg("rmdir %s: %s\n", cg, strerror(errno));
			return;
		}
		sleep_ns(10 * NSEC_PER_MSEC);
	}
}

/*
 * Return the CPU time of the calling thread in ns. clock_gettime() cannot
 * fail: the clock is always supported, and the pointer is valid.
 */
static uint64_t cpu_time_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

enum depart { DIES, SWITCHED_OUT };

/*
 * Fork a child that enters @cg, if given, then SCHED_DEADLINE at 400ms
 * every second. The child runs 380ms of its runtime. Then it dies if @how
 * is DIES. Otherwise it switches to SCHED_OTHER, writes a byte to @fd and
 * sleeps. Its 0-lag time is then about 570ms ahead. Return the pid of the
 * child, or -1. Only the read end of @fd stays open in the caller.
 */
static pid_t departing(const char *cg, enum depart how, int fd[2])
{
	pid_t pid = fork();

	if (pid < 0)
		ksft_print_msg("fork: %s\n", strerror(errno));
	if (!pid) {
		uint64_t t0;

		close(fd[0]);
		if (cg && enter(cg, getpid()))
			_exit(2);
		if (set_deadline(0, 400 * NSEC_PER_MSEC))
			_exit(3);
		t0 = cpu_time_ns();
		while (cpu_time_ns() - t0 < 380 * NSEC_PER_MSEC)
			;
		if (how == DIES)
			_exit(0);
		if (set_other(0) || write(fd[1], "x", 1) != 1)
			_exit(4);
		for (;;)
			pause();
	}
	close(fd[1]);
	fd[1] = -1;
	return pid;
}

/*
 * Wait until the child is out of SCHED_DEADLINE: dead or switched out.
 * Return 0 on success. If the child failed, return its exit status, or
 * -1. Set *@pid to -1 once the child is reaped.
 */
static int departed(pid_t *pid, enum depart how, int fd)
{
	int status;
	char c;

	if (*pid < 0)
		return -1;
	/*
	 * A child that switches out sends a byte. Without the byte, the
	 * child died or failed, and its exit status tells which.
	 */
	if (how == SWITCHED_OUT) {
		ssize_t n;

		do {
			n = read(fd, &c, 1);
		} while (n < 0 && errno == EINTR);
		if (n == 1)
			return 0;
	}
	if (waitpid(*pid, &status, 0) != *pid)
		return -1;
	*pid = -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * Fork a child that enters @cg and sleeps there. The child moves itself,
 * so that it last ran on a CPU of @cg. Wait until it is there. Return its
 * pid, or -1.
 */
static pid_t sleeper(const char *cg)
{
	int fd[2];
	pid_t pid;
	char c;

	if (pipe(fd))
		return -1;
	pid = fork();
	if (pid < 0)
		ksft_print_msg("fork: %s\n", strerror(errno));
	if (!pid) {
		close(fd[0]);
		if (enter(cg, getpid()) || write(fd[1], "x", 1) != 1)
			_exit(1);
		for (;;)
			pause();
	}
	close(fd[1]);
	if (pid > 0 && read(fd[0], &c, 1) != 1) {
		reap(pid);
		pid = -1;
	}
	close(fd[0]);
	return pid;
}

/*
 * Check that P admits one task of 600ms every second, but not two. The
 * caller must wait until every inactive timer has fired.
 */
static int check_p(void)
{
	pid_t a = sleeper(cg_p), b = sleeper(cg_p);
	int ret = KSFT_FAIL, e1, e2;

	if (a < 0 || b < 0) {
		ksft_print_msg("no task could enter P\n");
		goto out;
	}
	e1 = set_deadline(a, 600 * NSEC_PER_MSEC);
	e2 = set_deadline(b, 600 * NSEC_PER_MSEC);
	if (e1)
		ksft_print_msg("a first task refused in P: %s\n", strerror(-e1));
	else if (!e2)
		ksft_print_msg("1.2 CPUs of deadline tasks admitted in a partition of one CPU\n");
	else if (e2 != -EBUSY)
		ksft_print_msg("the second task refused with %s, not EBUSY\n", strerror(-e2));
	else
		ret = KSFT_PASS;
out:
	reap(a);
	reap(b);
	return ret;
}

/*
 * Have a task of P die or switch out, as @how says. Make Q before its
 * 0-lag time, then check P.
 */
static int test_rebuild(enum depart how)
{
	int ret = KSFT_FAIL, fd[2], err;
	pid_t pid = -1;

	if (pipe(fd))
		return KSFT_FAIL;
	if (make_partition(cg_p, ncpus - 1)) {
		ksft_print_msg("no partition root on CPU %d\n", ncpus - 1);
		ret = KSFT_SKIP;
		goto out;
	}
	pid = departing(cg_p, how, fd);
	err = departed(&pid, how, fd[0]);
	if (err) {
		ksft_print_msg("the deadline task failed (%d)\n", err);
		goto out;
	}
	if (make_partition(cg_q, ncpus - 2)) {
		ksft_print_msg("no partition root on CPU %d\n", ncpus - 2);
		ret = KSFT_SKIP;
		goto out;
	}
	/* Wait past the 0-lag time. A task that switched out sleeps in P. */
	sleep_ns(1500 * NSEC_PER_MSEC);
	ret = check_p();
out:
	reap(pid);
	remove_partition(cg_q);
	remove_partition(cg_p);
	close(fd[0]);
	if (fd[1] >= 0)
		close(fd[1]);
	return ret;
}

/*
 * Have a task outside P switch out. Move it into P before its 0-lag time,
 * then check P.
 */
static int test_move(void)
{
	int ret = KSFT_FAIL, fd[2], err;
	pid_t pid = -1;

	if (pipe(fd))
		return KSFT_FAIL;
	if (make_partition(cg_p, ncpus - 1)) {
		ksft_print_msg("no partition root on CPU %d\n", ncpus - 1);
		ret = KSFT_SKIP;
		goto out;
	}
	pid = departing(NULL, SWITCHED_OUT, fd);
	err = departed(&pid, SWITCHED_OUT, fd[0]);
	if (err) {
		ksft_print_msg("the deadline task failed (%d)\n", err);
		goto out;
	}
	if (enter(cg_p, pid)) {
		ksft_print_msg("the task cannot move into P\n");
		goto out;
	}
	/* The task wakes up and dies on P's CPU, before its 0-lag time. */
	reap(pid);
	pid = -1;
	sleep_ns(1500 * NSEC_PER_MSEC);
	ret = check_p();
out:
	reap(pid);
	remove_partition(cg_p);
	close(fd[0]);
	if (fd[1] >= 0)
		close(fd[1]);
	return ret;
}

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

int main(void)
{
	char buf[256], path[PATH_MAX + 32];
	int added;

	ksft_print_header();
	if (geteuid())
		ksft_exit_skip("needs root\n");
	ncpus = get_nprocs();
	if (ncpus < 3)
		ksft_exit_skip("needs 3 CPUs\n");
	if (read_file("/proc/sys/kernel/sched_rt_runtime_us", buf, sizeof(buf)) || atol(buf) < 0)
		ksft_exit_skip("no deadline admission control\n");
	if (find_cgroup2())
		ksft_exit_skip("cgroup v2 is not mounted\n");
	/*
	 * Enable the cpuset controller for the root's children, if it is
	 * off. It is turned off again at the end.
	 */
	snprintf(path, sizeof(path), "%s/cgroup.subtree_control", root);
	if (read_file(path, buf, sizeof(buf)))
		ksft_exit_skip("no cgroup.subtree_control\n");
	added = !strstr(buf, "cpuset");
	if (added && write_file(root, "cgroup.subtree_control", "+cpuset"))
		ksft_exit_skip("no cpuset controller\n");
	if (snprintf(cg_p, sizeof(cg_p), "%s/deadline_rebuild_p", root) >= (int)sizeof(cg_p) ||
	    snprintf(cg_q, sizeof(cg_q), "%s/deadline_rebuild_q", root) >= (int)sizeof(cg_q))
		ksft_exit_skip("cgroup v2 mount path too long\n");

	ksft_set_plan(3);
	report("dies_before_rebuild", test_rebuild(DIES));
	report("switched_out_before_rebuild", test_rebuild(SWITCHED_OUT));
	report("moved_after_switch_out", test_move());
	if (added && write_file(root, "cgroup.subtree_control", "-cpuset"))
		ksft_print_msg("the cpuset controller stays enabled\n");
	ksft_finished();
}
