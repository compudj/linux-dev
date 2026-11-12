// SPDX-License-Identifier: LGPL-2.1
/*
 * What becomes of an rseq registration at fork(2), clone(2) and execve(2).
 *
 * - A fork(2) child has its own copy of the memory. It inherits the
 *   registration, and the kernel keeps the IDs of the child up to date in
 *   that copy.
 * - A task that shares the memory of its parent, a thread or a vfork(2)
 *   child, starts without a registration. A vfork(2) child runs on the
 *   thread area of its parent, so it sees an rseq area that the kernel
 *   does not update for it.
 * - execve(2) drops the registration: the new program has no such area.
 *
 * ptrace(2) reports the registration of a tracee. Checkpoint and restore
 * tools use that to register the same area again.
 *
 * The tests register their own area, so they run without the registration
 * of glibc.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <sys/ptrace.h>

#include "rseq-own-area.h"
#include "../kselftest_harness.h"

#ifndef PTRACE_GET_RSEQ_CONFIGURATION
# define PTRACE_GET_RSEQ_CONFIGURATION	0x420f
#endif

/* struct ptrace_rseq_configuration of <linux/ptrace.h>. */
struct rseq_configuration {
	uint64_t pointer;
	uint32_t size;
	uint32_t signature;
	uint32_t flags;
	uint32_t pad;
};

#define AFTER_EXEC	"--after-exec"

static char buf[4096] __attribute__((aligned(4096)));
static char buf2[4096] __attribute__((aligned(4096)));
static struct rseq_abi *rs = (struct rseq_abi *)buf;
static uint32_t len;

/* Visit each allowed CPU. Return 0 if the area follows, or 1. */
static int ids_follow(struct rseq_abi *area)
{
	cpu_set_t allowed;
	int cpu, ret = 0;

	if (sched_getaffinity(0, sizeof(allowed), &allowed))
		return 1;
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, &allowed) || rseq_pin(cpu))
			continue;
		if (area->cpu_id_start != (uint32_t)cpu || area->cpu_id != (uint32_t)cpu ||
		    area->mm_cid >= (uint32_t)sysconf(_SC_NPROCESSORS_CONF))
			ret = 1;
	}
	if (sched_setaffinity(0, sizeof(allowed), &allowed))
		return 1;
	return ret;
}

FIXTURE(registered)
{
};

FIXTURE_VARIANT(registered)
{
	bool v2;
};

FIXTURE_VARIANT_ADD(registered, legacy)
{
	.v2 = false,
};

FIXTURE_VARIANT_ADD(registered, v2)
{
	.v2 = true,
};

FIXTURE_SETUP(registered)
{
	len = variant->v2 ? rseq_v2_size() : RSEQ_ORIG_SIZE;
	if (!len)
		SKIP(return, "the kernel has no feature beyond the original area");
	memset(buf, 0, sizeof(buf));
	ASSERT_EQ(sys_rseq(buf, len, 0, RSEQ_SIG), 0);
}

FIXTURE_TEARDOWN(registered)
{
}

static int fork_child(void *arg)
{
	/* The registration came with the copy of the memory. */
	if (sys_rseq(buf, len, 0, RSEQ_SIG) != -EBUSY)
		return 1;
	if (ids_follow(rs))
		return 2;
	if (sys_rseq(buf, len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG))
		return 3;
	return 0;
}

TEST_F(registered, fork_child_inherits)
{
	EXPECT_EQ(rseq_run_child(fork_child, NULL), 0);
	/* The child unregistered its copy, not the area of the parent. */
	EXPECT_EQ(sys_rseq(buf, len, 0, RSEQ_SIG), -EBUSY);
	EXPECT_EQ(ids_follow(rs), 0);
}

static void *thread_fn(void *arg)
{
	struct rseq_abi *own = (struct rseq_abi *)buf2;
	long ret = 0;

	/* No registration to remove, not even the one of the creator. */
	if (sys_rseq(buf, len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG) != -EINVAL)
		ret |= 1;
	memset(buf2, 0, sizeof(buf2));
	if (sys_rseq(buf2, len, 0, RSEQ_SIG))
		return (void *)(ret | 2);
	if (ids_follow(own))
		ret |= 4;
	if (sys_rseq(buf2, len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG))
		ret |= 8;
	return (void *)ret;
}

TEST_F(registered, thread_starts_unregistered)
{
	pthread_t thread;
	void *ret;

	ASSERT_EQ(pthread_create(&thread, NULL, thread_fn, NULL), 0);
	ASSERT_EQ(pthread_join(thread, &ret), 0);
	EXPECT_EQ((long)ret, 0);
	EXPECT_EQ(sys_rseq(buf, len, 0, RSEQ_SIG), -EBUSY);
	EXPECT_EQ(ids_follow(rs), 0);
}

TEST_F(registered, vfork_child_starts_unregistered)
{
	int status;
	pid_t pid;

	pid = vfork();
	ASSERT_GE(pid, 0);
	if (!pid) {
		/* Only system calls here: the child borrows this stack. */
		_exit(sys_rseq(buf, len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG) != -EINVAL);
	}
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	EXPECT_EQ(sys_rseq(buf, len, 0, RSEQ_SIG), -EBUSY);
	EXPECT_EQ(ids_follow(rs), 0);
}

static int exec_child(void *arg)
{
	char *const argv[] = { "rseq_inherit_test", AFTER_EXEC, NULL };

	execv("/proc/self/exe", argv);
	return 9;
}

/* The program after the execve(2). Return 0 if nothing is registered. */
static int after_exec(void)
{
	if (sys_rseq(buf, RSEQ_ORIG_SIZE, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG) != -EINVAL)
		return 1;
	if (sys_rseq(buf, RSEQ_ORIG_SIZE, 0, RSEQ_SIG))
		return 2;
	return 0;
}

TEST_F(registered, execve_drops_the_registration)
{
	int ret = rseq_run_child(exec_child, NULL);

	if (ret == 9)
		SKIP(return, "cannot run /proc/self/exe");
	EXPECT_EQ(ret, 0);
}

static int stopped_child(void *arg)
{
	if (arg && sys_rseq(buf, len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG))
		return 1;
	raise(SIGSTOP);
	return 0;
}

/* Read the rseq configuration of a child that is, or is not, registered. */
static int traced_configuration(bool unregister, struct rseq_configuration *conf)
{
	int status, ret = -1;
	pid_t pid;

	fflush(NULL);
	pid = fork();
	if (pid < 0)
		return -1;
	if (!pid)
		_exit(stopped_child(unregister ? buf : NULL));
	if (waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status) &&
	    !ptrace(PTRACE_SEIZE, pid, 0, 0)) {
		memset(conf, 0xff, sizeof(*conf));
		ret = ptrace(PTRACE_GET_RSEQ_CONFIGURATION, pid, sizeof(*conf), conf);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return ret;
}

TEST_F(registered, ptrace_reports_the_registration)
{
	struct rseq_configuration conf;

	ASSERT_EQ(traced_configuration(false, &conf), sizeof(conf));
	EXPECT_EQ(conf.pointer, (uintptr_t)buf);
	EXPECT_EQ(conf.size, len);
	EXPECT_EQ(conf.signature, RSEQ_SIG);
	EXPECT_EQ(conf.flags, 0);
	EXPECT_EQ(conf.pad, 0);

	ASSERT_EQ(traced_configuration(true, &conf), sizeof(conf));
	EXPECT_EQ(conf.pointer, 0);
	EXPECT_EQ(conf.size, 0);
	EXPECT_EQ(conf.signature, 0);
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], AFTER_EXEC))
		return after_exec();
	rseq_reexec_without_libc(argv);
	return test_harness_run(argc, argv);
}
