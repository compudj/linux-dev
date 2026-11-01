// SPDX-License-Identifier: GPL-2.0
/*
 * A SCHED_DEADLINE task switched to SCHED_OTHER and back:
 *
 *  - throttled: switched out after its runtime ran out, and back after
 *    its deadline but before its 0-lag time, it must run again, the
 *    replenishment it missed while out of SCHED_DEADLINE or not;
 *  - asleep past its 0-lag time: its bandwidth must leave the root domain
 *    when it leaves SCHED_DEADLINE, or the switch back adds it again and
 *    sched_setattr() soon refuses SCHED_DEADLINE with EBUSY.
 *
 * On a kernel that leaks, the second case leaves the root domain without
 * bandwidth for new SCHED_DEADLINE tasks until a reboot.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../kselftest.h"

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE	6
#endif

#define NSEC_PER_USEC	1000ULL
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

/* Written by the child, read by the parent. */
struct shared {
	uint64_t stamp;		/* last time the child ran */
	uint64_t run_start;	/* when it last resumed after a gap */
	uint64_t runs;		/* resumptions after a gap */
	uint64_t sleeps;	/* sleeps begun */
};

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

static void sleep_until(uint64_t t)
{
	struct timespec ts = { .tv_sec = t / NSEC_PER_SEC, .tv_nsec = t % NSEC_PER_SEC };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

static int set_deadline(pid_t pid, uint64_t runtime, uint64_t period)
{
	struct dl_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_DEADLINE,
		.sched_runtime = runtime,
		.sched_deadline = period,
		.sched_period = period,
	};

	return syscall(__NR_sched_setattr, pid, &attr, 0) ? -errno : 0;
}

static int set_other(pid_t pid)
{
	struct sched_param sp = { .sched_priority = 0 };

	return sched_setscheduler(pid, SCHED_OTHER, &sp) ? -errno : 0;
}

static struct shared *sh;

static void reap(pid_t pid)
{
	int status;

	if (pid <= 0)
		return;
	if (kill(pid, SIGKILL) && errno != ESRCH)
		ksft_print_msg("kill %d: %s\n", pid, strerror(errno));
	/* A task lost in SCHED_DEADLINE runs again, and dies, once out of it. */
	set_other(pid);			/* gone already: nothing to switch */
	while (waitpid(pid, &status, 0) != pid) {
		if (errno == EINTR)
			continue;
		ksft_print_msg("waitpid %d: %s\n", pid, strerror(errno));
		break;
	}
}

/* The child died before the case was done: @status says whether it skips. */
static int gone(int status)
{
	if (WIFEXITED(status) && WEXITSTATUS(status) == KSFT_SKIP) {
		ksft_print_msg("SCHED_DEADLINE refused\n");
		return KSFT_SKIP;
	}
	if (WIFSIGNALED(status))
		ksft_print_msg("the task died of signal %d\n", WTERMSIG(status));
	else
		ksft_print_msg("the task exited with %d\n", WEXITSTATUS(status));
	return KSFT_FAIL;
}

/* Spins: runs out of its runtime at once, and is throttled. */
static void spinner(void)
{
	uint64_t last = now_ns();

	for (;;) {
		uint64_t t = now_ns();

		if (t - last > 2 * NSEC_PER_MSEC) {
			__atomic_store_n(&sh->run_start, t, __ATOMIC_RELAXED);
			__atomic_fetch_add(&sh->runs, 1, __ATOMIC_RELEASE);
		}
		__atomic_store_n(&sh->stamp, t, __ATOMIC_RELEASE);
		last = t;
	}
}

#define LOST_RUNTIME	(10 * NSEC_PER_MSEC)
#define LOST_PERIOD	(2 * NSEC_PER_SEC)
#define LOST_MARGIN	(200 * NSEC_PER_USEC)

/*
 * The window between the deadline and the 0-lag time is the overrun
 * scaled by period/runtime: 200 here, so that an overrun of 2us leaves
 * 400us. A runtime longer than a tick keeps an overrun caught at the tick
 * from carrying past the next replenishment, which comes a period after
 * the last. The switch back comes LOST_MARGIN after the deadline, once
 * the replenishment timer has fired on a task out of SCHED_DEADLINE.
 */
static int test_throttled_round_trip(void)
{
	int attempt, ret = KSFT_PASS, err;
	pid_t pid;

	memset(sh, 0, sizeof(*sh));
	pid = fork();
	if (pid < 0) {
		ksft_print_msg("fork: %s\n", strerror(errno));
		return KSFT_FAIL;
	}
	if (!pid) {
		if (set_deadline(0, LOST_RUNTIME, LOST_PERIOD))
			_exit(KSFT_SKIP);
		spinner();
	}
	for (attempt = 0; attempt < 3 && ret == KSFT_PASS; attempt++) {
		uint64_t runs, r, s, t;

		/* A resumption at a replenishment, D, then its throttle. */
		runs = __atomic_load_n(&sh->runs, __ATOMIC_ACQUIRE);
		t = now_ns();
		while (__atomic_load_n(&sh->runs, __ATOMIC_ACQUIRE) == runs) {
			if (now_ns() - t > 3 * LOST_PERIOD) {
				int status;

				if (waitpid(pid, &status, WNOHANG) == pid)
					return gone(status);
				ksft_print_msg("the task did not resume within 3 periods\n");
				reap(pid);
				return KSFT_FAIL;
			}
			sleep_ns(NSEC_PER_MSEC);
		}
		r = __atomic_load_n(&sh->run_start, __ATOMIC_RELAXED);
		do {
			s = __atomic_load_n(&sh->stamp, __ATOMIC_ACQUIRE);
			sleep_ns(2 * NSEC_PER_MSEC);
		} while (__atomic_load_n(&sh->stamp, __ATOMIC_ACQUIRE) != s);
		/* Throttled until D + period: out, and back after that. */
		err = set_other(pid);
		if (!err) {
			sleep_until(r + LOST_PERIOD + LOST_MARGIN);
			err = set_deadline(pid, LOST_RUNTIME, LOST_PERIOD);
		}
		if (err) {
			ksft_print_msg("policy switch: %s\n", strerror(-err));
			ret = KSFT_FAIL;
			break;
		}
		sleep_ns(10 * NSEC_PER_MSEC);
		s = __atomic_load_n(&sh->stamp, __ATOMIC_ACQUIRE);
		t = now_ns();
		while (__atomic_load_n(&sh->stamp, __ATOMIC_ACQUIRE) == s &&
		       now_ns() - t < LOST_PERIOD + LOST_PERIOD / 2)
			sleep_ns(NSEC_PER_MSEC);
		if (__atomic_load_n(&sh->stamp, __ATOMIC_ACQUIRE) == s) {
			ksft_print_msg("throttled, switched out and back: never ran again\n");
			ret = KSFT_FAIL;
		}
	}
	reap(pid);
	return ret;
}

#define LEAK_RUNTIME	(9 * NSEC_PER_MSEC)
#define LEAK_PERIOD	(10 * NSEC_PER_MSEC)

/* Runs 0.2ms, sleeps 50ms. */
static void sleeper(void)
{
	for (;;) {
		uint64_t end = now_ns() + 200 * NSEC_PER_USEC;

		while (now_ns() < end)
			;
		__atomic_fetch_add(&sh->sleeps, 1, __ATOMIC_RELEASE);
		sleep_ns(50 * NSEC_PER_MSEC);
	}
}

/*
 * 90% of a CPU per round trip: a leak fills the root domain within about
 * as many round trips as it has CPUs.
 */
static int test_asleep_round_trip(void)
{
	int i, trips = get_nprocs() * 10 / 9 + 3, ret = KSFT_PASS, err;
	pid_t pid;
	FILE *f;

	f = fopen("/proc/sys/kernel/sched_rt_runtime_us", "r");
	if (f) {
		long rt = 0;

		if (fscanf(f, "%ld", &rt) == 1 && rt < 0) {
			fclose(f);
			ksft_print_msg("no admission control: a leak cannot be seen\n");
			return KSFT_SKIP;
		}
		fclose(f);
	}
	memset(sh, 0, sizeof(*sh));
	pid = fork();
	if (pid < 0) {
		ksft_print_msg("fork: %s\n", strerror(errno));
		return KSFT_FAIL;
	}
	if (!pid) {
		if (set_deadline(0, LEAK_RUNTIME, LEAK_PERIOD))
			_exit(KSFT_SKIP);
		sleeper();
	}
	for (i = 0; i < trips; i++) {
		uint64_t sleeps = __atomic_load_n(&sh->sleeps, __ATOMIC_ACQUIRE);
		uint64_t t = now_ns();

		while (__atomic_load_n(&sh->sleeps, __ATOMIC_ACQUIRE) == sleeps) {
			if (now_ns() - t > NSEC_PER_SEC) {
				int status;

				if (waitpid(pid, &status, WNOHANG) == pid)
					return gone(status);
				ksft_print_msg("the task did not run within 1s\n");
				reap(pid);
				return KSFT_FAIL;
			}
			sleep_ns(NSEC_PER_MSEC);
		}
		/* Asleep, well past its 0-lag time. */
		sleep_ns(5 * NSEC_PER_MSEC);
		err = set_other(pid);
		if (!err)
			err = set_deadline(pid, LEAK_RUNTIME, LEAK_PERIOD);
		if (err) {
			ksft_print_msg("switch back refused after %d of %d round trips: %s\n",
				       i, trips, strerror(-err));
			ret = KSFT_FAIL;
			break;
		}
	}
	reap(pid);
	return ret;
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
	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	ksft_print_header();
	ksft_set_plan(2);
	/* The first one first: the second fills the root domain where it leaks. */
	report("throttled_round_trip", test_throttled_round_trip());
	report("asleep_round_trip", test_asleep_round_trip());
	ksft_finished();
}
