// SPDX-License-Identifier: LGPL-2.1
/*
 * The concurrency ID a thread reports after a failed execve(2).
 *
 * sched_mm_cid_fork() makes a task a user of the CID domain of its mm.
 * In per CPU mode a new user takes no CID of its own: it borrows the one
 * of the CPU it runs on at its schedule in. The failure path of
 * execve(2) rejoins the mm of a process whose other threads are users
 * still, from a task that is running and reaches user space before any
 * schedule in, so the CID it reports has to be given to it there.
 *
 * The process under test needs more threads than its mm has CIDs for the
 * mm to be in per CPU mode. The number of CIDs follows the union of the
 * threads' allowed CPUs, which only ever grows, so the affinity is
 * narrowed before the fork(2) that creates that mm, which keeps the
 * number of threads needed small and independent of the machine.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rseq.h"
#include "kselftest.h"

#define NARROW_CPUS	2
#define THREADS		4
#define ROUNDS		100

/* Exit status of the process under test. */
#define CHILD_PASS	0
#define CHILD_FAIL	1
#define CHILD_SKIP	2
#define CHILD_ERROR	3

static int stop;

/*
 * A thread is a user of its mm's CID domain whether it has rseq
 * registered or not: it only has to exist while the execve(2) below
 * leaves the domain and rejoins it.
 */
static void *waiter(void *arg)
{
	while (!RSEQ_READ_ONCE(stop))
		(void)sched_yield();
	return NULL;
}

/*
 * A file no binfmt handler recognises: execve(2) reaches bprm_execve(),
 * and so sched_mm_cid_before_execve(), and fails with ENOEXEC in
 * search_binary_handler() before begin_new_exec() has killed the other
 * threads. A path that does not exist fails before bprm_execve().
 *
 * The directory has to allow execution, so try the test's own before the
 * temporary one. Leaves the path in @path and consumes one execve(2).
 */
static int make_unknown_format_file(char *path, size_t len)
{
	static const char *const dirs[] = { ".", "/tmp" };
	static const char junk[] = "not a binary of any known format\n";
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dirs); i++) {
		char *const av[] = { path, NULL };
		bool written;
		int fd;

		snprintf(path, len, "%s/mm_cid_execve_test.XXXXXX", dirs[i]);
		fd = mkstemp(path);
		if (fd < 0)
			continue;
		written = write(fd, junk, sizeof(junk) - 1) == (ssize_t)sizeof(junk) - 1 &&
			  !fchmod(fd, 0755);
		/* Closed before the execve(2), which would fail with ETXTBSY. */
		if (close(fd) || !written) {
			unlink(path);
			continue;
		}
		if (execve(path, av, NULL) == -1 && errno == ENOEXEC)
			return 0;
		unlink(path);
	}
	return -1;
}

/*
 * The process under test: @THREADS threads on a narrow set of CPUs, then
 * failing execve(2) calls, reading the CID each one reports back.
 */
static int run_child(void)
{
	pthread_t thread[THREADS];
	char path[PATH_MAX];
	unsigned int nr_cids = (unsigned int)get_nprocs_conf();
	unsigned int worst = 0;
	int i, started = 0, ret = CHILD_PASS;

	if (make_unknown_format_file(path, sizeof(path))) {
		fprintf(stderr, "no directory to run an unknown format file from: %s\n",
			strerror(errno));
		return CHILD_SKIP;
	}

	for (i = 0; i < THREADS; i++) {
		if (pthread_create(&thread[i], NULL, waiter, NULL))
			break;
		started++;
	}
	if (started < THREADS) {
		fprintf(stderr, "pthread_create: %s\n", strerror(errno));
		ret = CHILD_ERROR;
		goto out;
	}

	for (i = 0; i < ROUNDS; i++) {
		char *const av[] = { path, NULL };
		unsigned int cid;

		if (!execve(path, av, NULL)) {
			fprintf(stderr, "execve(%s) succeeded, expected ENOEXEC\n", path);
			ret = CHILD_ERROR;
			goto out;
		}
		if (errno != ENOEXEC) {
			fprintf(stderr, "execve(%s): %s, expected ENOEXEC\n",
				path, strerror(errno));
			ret = CHILD_ERROR;
			goto out;
		}
		cid = rseq_current_mm_cid();
		if (cid > worst)
			worst = cid;
		if (cid >= nr_cids) {
			fprintf(stderr, "mm_cid %u (0x%x) after a failed execve, round %d of %d\n",
				cid, cid, i, ROUNDS);
			ret = CHILD_FAIL;
			goto out;
		}
	}
	fprintf(stderr, "%d threads, %d failed execve, largest mm_cid %u, below %u\n",
		started + 1, ROUNDS, worst, nr_cids);
out:
	RSEQ_WRITE_ONCE(stop, 1);
	for (i = 0; i < started; i++)
		pthread_join(thread[i], NULL);
	unlink(path);
	return ret;
}

/* The CPUs of @src the process is allowed on, at most @nr of them. */
static int narrow_cpus(cpu_set_t *dst, const cpu_set_t *src, int nr)
{
	int cpu, taken = 0;

	CPU_ZERO(dst);
	for (cpu = 0; cpu < CPU_SETSIZE && taken < nr; cpu++) {
		if (!CPU_ISSET(cpu, src))
			continue;
		CPU_SET(cpu, dst);
		taken++;
	}
	return taken;
}

int main(void)
{
	cpu_set_t allowed, narrow;
	int status = 0;
	pid_t child;

	ksft_print_header();
	ksft_set_plan(1);

	if (!rseq_available())
		ksft_exit_skip("rseq is unavailable\n");
	if (!rseq_mm_cid_available())
		ksft_exit_skip("the rseq mm_cid field is unavailable\n");
	if (sched_getaffinity(0, sizeof(allowed), &allowed))
		ksft_exit_fail_msg("sched_getaffinity: %s\n", strerror(errno));
	if (!narrow_cpus(&narrow, &allowed, NARROW_CPUS))
		ksft_exit_skip("no CPU to run on\n");

	/*
	 * Narrowed before the fork(2): the child's mm takes the union of
	 * allowed CPUs from it, and a later narrowing would not shrink it.
	 */
	if (sched_setaffinity(0, sizeof(narrow), &narrow))
		ksft_exit_skip("sched_setaffinity: %s\n", strerror(errno));
	child = fork();
	if (!child)
		_exit(run_child());
	if (child < 0)
		ksft_exit_fail_msg("fork: %s\n", strerror(errno));
	if (sched_setaffinity(0, sizeof(allowed), &allowed))
		ksft_exit_fail_msg("sched_setaffinity: %s\n", strerror(errno));
	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_msg("waitpid: %s\n", strerror(errno));
	if (!WIFEXITED(status))
		ksft_exit_fail_msg("the process under test was killed by signal %d\n",
				   WTERMSIG(status));

	switch (WEXITSTATUS(status)) {
	case CHILD_PASS:
		ksft_test_result_pass("mm_cid after a failed execve\n");
		break;
	case CHILD_SKIP:
		ksft_test_result_skip("mm_cid after a failed execve\n");
		break;
	case CHILD_FAIL:
		ksft_test_result_fail("mm_cid after a failed execve\n");
		break;
	default:
		ksft_exit_fail_msg("the process under test failed to run\n");
	}
	ksft_finished();
}
