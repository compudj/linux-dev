// SPDX-License-Identifier: GPL-2.0
/*
 * Check the membarrier command arguments and the lifetime of the
 * registrations.
 *
 * The test harness runs each test in its own process. This matters
 * because a membarrier registration cannot be undone.
 */
#define _GNU_SOURCE
#include <linux/membarrier.h>
#include <sys/wait.h>
#include <syscall.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>

#include "kselftest_harness.h"

#define STACK_SIZE	(64 * 1024)

#define REGISTER_CMDS						\
	(MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED |		\
	 MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED |		\
	 MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE |	\
	 MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ)

/* Pairs of registration command and command which needs it. */
static const struct {
	int register_cmd;
	int cmd;
} private_cmds[] = {
	{ MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
	  MEMBARRIER_CMD_PRIVATE_EXPEDITED },
	{ MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE,
	  MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE },
	{ MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ,
	  MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ },
};

static int sys_membarrier(int cmd, unsigned int flags, int cpu_id)
{
	return syscall(__NR_membarrier, cmd, flags, cpu_id);
}

static int query(void)
{
	return sys_membarrier(MEMBARRIER_CMD_QUERY, 0, 0);
}

static int get_registrations(void)
{
	return sys_membarrier(MEMBARRIER_CMD_GET_REGISTRATIONS, 0, 0);
}

/* Each command is either in the query mask or returns -EINVAL. */
TEST(query_matches_commands)
{
	int mask = query(), bit, ret;

	ASSERT_GE(mask, 0);
	for (bit = 0; bit < 32; bit++) {
		int cmd = (int)(1U << bit);

		errno = 0;
		ret = sys_membarrier(cmd, 0, 0);
		if (mask & cmd) {
			EXPECT_NE(errno, EINVAL) {
				TH_LOG("command 0x%x is in the query mask", cmd);
			}
		} else {
			EXPECT_EQ(ret, -1);
			EXPECT_EQ(errno, EINVAL) {
				TH_LOG("command 0x%x is not in the query mask", cmd);
			}
		}
	}
}

/* A command is a single bit. */
TEST(invalid_commands)
{
	static const int cmds[] = {
		-1, INT_MAX,
		MEMBARRIER_CMD_GLOBAL | MEMBARRIER_CMD_GLOBAL_EXPEDITED,
		MEMBARRIER_CMD_PRIVATE_EXPEDITED |
			MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		EXPECT_EQ(sys_membarrier(cmds[i], 0, 0), -1);
		EXPECT_EQ(errno, EINVAL);
	}
}

/* Only the rseq command accepts a flag: MEMBARRIER_CMD_FLAG_CPU. */
TEST(invalid_flags)
{
	static const unsigned int flags[] = {
		MEMBARRIER_CMD_FLAG_CPU, 1U << 1, 1U << 31, ~0U,
		MEMBARRIER_CMD_FLAG_CPU | (1U << 1),
	};
	int mask = query(), bit, i;

	ASSERT_GE(mask, 0);
	/* Bit -1 stands for MEMBARRIER_CMD_QUERY, which is 0. */
	for (bit = -1; bit < 32; bit++) {
		int cmd = bit < 0 ? MEMBARRIER_CMD_QUERY : (int)(1U << bit);

		if (cmd && !(mask & cmd))
			continue;
		for (i = 0; i < ARRAY_SIZE(flags); i++) {
			if (cmd == MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ &&
			    flags[i] == MEMBARRIER_CMD_FLAG_CPU)
				continue;
			EXPECT_EQ(sys_membarrier(cmd, flags[i], 0), -1);
			EXPECT_EQ(errno, EINVAL) {
				TH_LOG("command 0x%x flags 0x%x", cmd, flags[i]);
			}
		}
	}
}

/* A private command needs its own registration. */
TEST(private_needs_registration)
{
	int mask = query(), registered = get_registrations(), i;

	ASSERT_GE(mask, 0);
	ASSERT_GE(registered, 0);
	for (i = 0; i < ARRAY_SIZE(private_cmds); i++) {
		if (!(mask & private_cmds[i].cmd))
			continue;
		/* The C library may have registered already. */
		if (registered & private_cmds[i].register_cmd)
			continue;
		EXPECT_EQ(sys_membarrier(private_cmds[i].cmd, 0, 0), -1);
		EXPECT_EQ(errno, EPERM);
	}
	if ((mask & MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ) &&
	    !(registered & MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ)) {
		EXPECT_EQ(sys_membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
					 MEMBARRIER_CMD_FLAG_CPU, 0), -1);
		EXPECT_EQ(errno, EPERM);
	}
}

/*
 * Issue one registration command. It must add its own bit to the
 * registrations, and no other bit. It must allow its own command, and
 * no other command.
 */
static void register_one(struct __test_metadata *_metadata, int register_cmd)
{
	int mask = query(), before = get_registrations(), i;

	ASSERT_GE(mask, 0);
	ASSERT_GE(before, 0);
	if (!(mask & register_cmd))
		SKIP(return, "command 0x%x is not available", register_cmd);
	ASSERT_EQ(sys_membarrier(register_cmd, 0, 0), 0);
	EXPECT_EQ(get_registrations(), before | register_cmd);
	/* A second registration succeeds and changes nothing. */
	ASSERT_EQ(sys_membarrier(register_cmd, 0, 0), 0);
	EXPECT_EQ(get_registrations(), before | register_cmd);

	for (i = 0; i < ARRAY_SIZE(private_cmds); i++) {
		if (!(mask & private_cmds[i].cmd))
			continue;
		if ((before | register_cmd) & private_cmds[i].register_cmd) {
			EXPECT_EQ(sys_membarrier(private_cmds[i].cmd, 0, 0), 0);
		} else {
			EXPECT_EQ(sys_membarrier(private_cmds[i].cmd, 0, 0), -1);
			EXPECT_EQ(errno, EPERM);
		}
	}
}

TEST(register_global_expedited)
{
	register_one(_metadata, MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED);
}

TEST(register_private_expedited)
{
	register_one(_metadata, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED);
}

TEST(register_private_expedited_sync_core)
{
	register_one(_metadata,
		     MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE);
}

TEST(register_private_expedited_rseq)
{
	register_one(_metadata, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ);
}

/* Register every command, one at a time. Return the registrations. */
static int register_all(struct __test_metadata *_metadata)
{
	int mask = query(), expected = get_registrations(), bit;

	for (bit = 0; bit < 31; bit++) {
		int cmd = 1 << bit;

		if (!(cmd & REGISTER_CMDS & mask))
			continue;
		ASSERT_EQ(sys_membarrier(cmd, 0, 0), 0);
		expected |= cmd;
		EXPECT_EQ(get_registrations(), expected);
	}
	return expected;
}

TEST(register_all_commands)
{
	register_all(_metadata);
}

static void *thread_pause(void *arg)
{
	for (;;)
		pause();
	return NULL;
}

/*
 * The rseq command accepts any CPU number. It ignores a CPU which does
 * not exist. A negative CPU number selects all CPUs.
 *
 * The kernel only looks at the CPU number of a multi-threaded process,
 * so keep a second thread alive.
 */
TEST(rseq_cpu_argument)
{
	static const int cpus[] = { 0, INT_MAX, -1, INT_MIN };
	pthread_t thread;
	int i;

	if (!(query() & MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ))
		SKIP(return, "the rseq command is not available");
	ASSERT_EQ(pthread_create(&thread, NULL, thread_pause, NULL), 0);
	ASSERT_EQ(sys_membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ,
				 0, 0), 0);
	EXPECT_EQ(sys_membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
				 MEMBARRIER_CMD_FLAG_CPU, sched_getcpu()), 0);
	for (i = 0; i < ARRAY_SIZE(cpus); i++) {
		EXPECT_EQ(sys_membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
					 MEMBARRIER_CMD_FLAG_CPU, cpus[i]), 0) {
			TH_LOG("cpu_id %d", cpus[i]);
		}
	}
	/* Without the flag, the CPU argument is ignored. */
	EXPECT_EQ(sys_membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
				 0, INT_MAX), 0);
}

/* A forked child inherits the registrations. */
TEST(fork_inherits_registrations)
{
	int expected = register_all(_metadata), status;
	pid_t pid;

	pid = fork();
	ASSERT_GE(pid, 0);
	if (!pid)
		_exit(get_registrations() != expected);
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
}

/*
 * exec() drops the registrations. The new program image starts with
 * the registrations which this test program had when it started.
 */
static int initial_registrations;

TEST(exec_drops_registrations)
{
	char expected[16];
	int status;
	pid_t pid;

	register_all(_metadata);
	snprintf(expected, sizeof(expected), "%d", initial_registrations);
	pid = fork();
	ASSERT_GE(pid, 0);
	if (!pid) {
		execl("/proc/self/exe", "membarrier_test_cmd",
		      "--expect-registrations", expected, NULL);
		_exit(2);
	}
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_TRUE(WIFEXITED(status));
	if (WEXITSTATUS(status) == 2)
		SKIP(return, "cannot exec /proc/self/exe");
	EXPECT_EQ(WEXITSTATUS(status), 0);
}

static int clone_vm_fn(void *arg)
{
	int cmd = *(int *)arg;

	return sys_membarrier(cmd, 0, 0) ? 1 : 0;
}

/*
 * The registrations belong to the mm. A task of another thread group
 * which shares the mm (CLONE_VM without CLONE_THREAD) registers for
 * both thread groups.
 */
TEST(clone_vm_shares_registrations)
{
	int cmd = MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
	int before = get_registrations(), status;
	char *stack;
	pid_t pid;

	ASSERT_GE(before, 0);
	stack = malloc(STACK_SIZE);
	ASSERT_NE(stack, NULL);
	pid = clone(clone_vm_fn, stack + STACK_SIZE, CLONE_VM | SIGCHLD, &cmd);
	ASSERT_GE(pid, 0);
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	EXPECT_EQ(get_registrations(), before | cmd);
	EXPECT_EQ(sys_membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0), 0);
	free(stack);
}

int main(int argc, char **argv)
{
	/* New program image of exec_drops_registrations. */
	if (argc == 3 && !strcmp(argv[1], "--expect-registrations"))
		return get_registrations() != atoi(argv[2]);

	if (query() < 0)
		ksft_exit_skip("sys membarrier (CONFIG_MEMBARRIER) is disabled.\n");
	initial_registrations = get_registrations();
	if (initial_registrations < 0)
		ksft_exit_skip("MEMBARRIER_CMD_GET_REGISTRATIONS is not available.\n");
	return test_harness_run(argc, argv);
}
