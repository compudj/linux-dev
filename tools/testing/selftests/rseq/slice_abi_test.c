// SPDX-License-Identifier: LGPL-2.1
/*
 * The control interface of the rseq time slice extension.
 *
 * A thread turns the extension on with prctl(PR_RSEQ_SLICE_EXTENSION), or
 * with RSEQ_FLAG_SLICE_EXT_DEFAULT_ON at registration. The kernel shows
 * the state in two bits of the flags field of the rseq area. One says
 * that the extension is available. The other says that it is enabled.
 * User space only reads these bits.
 *
 * slice_test checks that a grant extends the time slice. The cases here
 * check the interface around it:
 * - prctl() needs a registration in the V2 mode;
 * - the arguments that prctl() refuses;
 * - the flags follow each change of state, and show nothing when the
 *   extension is turned off at boot;
 * - a fork(2) child keeps the state;
 * - a process that changed the flags is killed at the next prctl();
 * - rseq_slice_yield(2) returns 0 when no extension was granted.
 *
 * The tests register their own area, so they run without the registration
 * of glibc.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/prctl.h>

#include "rseq-own-area.h"
#include "../kselftest_harness.h"

#ifndef __NR_rseq_slice_yield
# define __NR_rseq_slice_yield		471
#endif

#ifndef PR_RSEQ_SLICE_EXTENSION
# define PR_RSEQ_SLICE_EXTENSION		79
# define PR_RSEQ_SLICE_EXTENSION_GET		1
# define PR_RSEQ_SLICE_EXTENSION_SET		2
# define PR_RSEQ_SLICE_EXT_ENABLE		0x01
#endif

#ifndef RSEQ_FLAG_SLICE_EXT_DEFAULT_ON
# define RSEQ_FLAG_SLICE_EXT_DEFAULT_ON		(1 << 1)
#endif

#define SLICE_AVAILABLE		(1U << 4)
#define SLICE_ENABLED		(1U << 5)
#define SLICE_FLAGS		(SLICE_AVAILABLE | SLICE_ENABLED)

static char buf[4096] __attribute__((aligned(4096)));
static struct rseq_abi *rs = (struct rseq_abi *)buf;

/* Call prctl(PR_RSEQ_SLICE_EXTENSION). Return its result, or -errno. */
static int slice_prctl(unsigned long op, unsigned long arg3, unsigned long arg4,
		       unsigned long arg5)
{
	int ret = prctl(PR_RSEQ_SLICE_EXTENSION, op, arg3, arg4, arg5);

	return ret < 0 ? -errno : ret;
}

static int slice_get(void)
{
	return slice_prctl(PR_RSEQ_SLICE_EXTENSION_GET, 0, 0, 0);
}

static int slice_set(unsigned long on)
{
	return slice_prctl(PR_RSEQ_SLICE_EXTENSION_SET, on, 0, 0);
}

/*
 * Register an area in the V2 mode with @flags. Return 0, or 1 if the
 * kernel has no time slice extension for this thread.
 */
static int register_v2(int flags)
{
	uint32_t len = rseq_v2_size();

	memset(buf, 0xff, sizeof(buf));
	if (!len || sys_rseq(buf, len, flags, RSEQ_SIG))
		return 1;
	if (!(rs->flags & SLICE_AVAILABLE)) {
		sys_rseq(buf, len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG);
		return 1;
	}
	return 0;
}

/* Return true if a V2 registration shows the extension as available. */
static bool slice_available(void)
{
	if (register_v2(0))
		return false;
	sys_rseq(buf, rseq_v2_size(), RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG);
	return true;
}

#define REGISTER_V2_OR_SKIP(flags)						\
	do {									\
		if (register_v2(flags))						\
			SKIP(return, "no time slice extension on this kernel");	\
	} while (0)

TEST(prctl_needs_a_registration)
{
	if (!slice_available())
		SKIP(return, "no time slice extension on this kernel");

	EXPECT_EQ(slice_get(), 0);
	EXPECT_EQ(slice_set(PR_RSEQ_SLICE_EXT_ENABLE), -ENXIO);
	EXPECT_EQ(slice_set(0), -ENXIO);
}

TEST(prctl_needs_the_v2_mode)
{
	if (slice_get() < 0)
		SKIP(return, "no time slice extension on this kernel");

	memset(buf, 0, sizeof(buf));
	ASSERT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, 0, RSEQ_SIG), 0);

	/* A legacy registration shows no flag, and cannot be enabled. */
	EXPECT_EQ(rs->flags & SLICE_FLAGS, 0);
	EXPECT_EQ(slice_get(), 0);
	EXPECT_LT(slice_set(PR_RSEQ_SLICE_EXT_ENABLE), 0);
	EXPECT_EQ(slice_get(), 0);
	EXPECT_EQ(rs->flags & SLICE_FLAGS, 0);
}

/* The registration flag is accepted, and does nothing, in the legacy mode. */
TEST(registration_flag_in_the_legacy_mode)
{
	if (slice_get() < 0)
		SKIP(return, "no time slice extension on this kernel");

	memset(buf, 0, sizeof(buf));
	ASSERT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, RSEQ_FLAG_SLICE_EXT_DEFAULT_ON, RSEQ_SIG), 0);
	EXPECT_EQ(rs->flags & SLICE_FLAGS, 0);
	EXPECT_EQ(slice_get(), 0);
}

/*
 * The flag in the area and prctl() agree. A kernel booted with
 * rseq_slice_ext=0 shows no flag, and refuses to enable.
 */
TEST(flag_tells_what_prctl_accepts)
{
	uint32_t len = rseq_v2_size();

	if (!len || slice_get() < 0)
		SKIP(return, "no time slice extension on this kernel");

	memset(buf, 0, sizeof(buf));
	ASSERT_EQ(sys_rseq(buf, len, 0, RSEQ_SIG), 0);
	if (rs->flags & SLICE_AVAILABLE) {
		EXPECT_EQ(slice_set(PR_RSEQ_SLICE_EXT_ENABLE), 0);
	} else {
		EXPECT_LT(slice_set(PR_RSEQ_SLICE_EXT_ENABLE), 0);
		EXPECT_EQ(rs->flags & SLICE_FLAGS, 0);
	}
}

TEST(prctl_arguments)
{
	REGISTER_V2_OR_SKIP(0);

	/* Bits other than the enable bit. */
	EXPECT_EQ(slice_set(2), -EINVAL);
	EXPECT_EQ(slice_set(PR_RSEQ_SLICE_EXT_ENABLE | 2), -EINVAL);
	/* The unused arguments must be zero. */
	EXPECT_EQ(slice_prctl(PR_RSEQ_SLICE_EXTENSION_SET, PR_RSEQ_SLICE_EXT_ENABLE, 1, 0),
		  -EINVAL);
	EXPECT_EQ(slice_prctl(PR_RSEQ_SLICE_EXTENSION_SET, PR_RSEQ_SLICE_EXT_ENABLE, 0, 1),
		  -EINVAL);
	EXPECT_EQ(slice_prctl(PR_RSEQ_SLICE_EXTENSION_GET, 1, 0, 0), -EINVAL);
	EXPECT_EQ(slice_prctl(PR_RSEQ_SLICE_EXTENSION_GET, 0, 1, 0), -EINVAL);
	EXPECT_EQ(slice_prctl(PR_RSEQ_SLICE_EXTENSION_GET, 0, 0, 1), -EINVAL);
	/* Not an operation. */
	EXPECT_EQ(slice_prctl(0, 0, 0, 0), -EINVAL);
	EXPECT_EQ(slice_prctl(3, 0, 0, 0), -EINVAL);

	/* None of the refused calls changed the state. */
	EXPECT_EQ(slice_get(), 0);
	EXPECT_EQ(rs->flags & SLICE_FLAGS, SLICE_AVAILABLE);
}

TEST(flags_follow_the_state)
{
	REGISTER_V2_OR_SKIP(0);

	/* A registration starts disabled, with no request and no grant. */
	EXPECT_EQ(rs->flags & SLICE_FLAGS, SLICE_AVAILABLE);
	EXPECT_EQ(slice_get(), 0);
	EXPECT_EQ(rs->slice_ctrl.all, 0);

	EXPECT_EQ(slice_set(PR_RSEQ_SLICE_EXT_ENABLE), 0);
	EXPECT_EQ(rs->flags & SLICE_FLAGS, SLICE_FLAGS);
	EXPECT_EQ(slice_get(), PR_RSEQ_SLICE_EXT_ENABLE);
	/* Enabling twice is fine. */
	EXPECT_EQ(slice_set(PR_RSEQ_SLICE_EXT_ENABLE), 0);
	EXPECT_EQ(slice_get(), PR_RSEQ_SLICE_EXT_ENABLE);

	EXPECT_EQ(slice_set(0), 0);
	EXPECT_EQ(rs->flags & SLICE_FLAGS, SLICE_AVAILABLE);
	EXPECT_EQ(slice_get(), 0);
	EXPECT_EQ(slice_set(0), 0);
}

TEST(registration_flag_enables)
{
	REGISTER_V2_OR_SKIP(RSEQ_FLAG_SLICE_EXT_DEFAULT_ON);

	EXPECT_EQ(rs->flags & SLICE_FLAGS, SLICE_FLAGS);
	EXPECT_EQ(slice_get(), PR_RSEQ_SLICE_EXT_ENABLE);
	EXPECT_EQ(rs->slice_ctrl.all, 0);

	EXPECT_EQ(slice_set(0), 0);
	EXPECT_EQ(rs->flags & SLICE_FLAGS, SLICE_AVAILABLE);
}

/* The state belongs to the registration: a new one starts disabled. */
TEST(state_ends_with_the_registration)
{
	uint32_t len = rseq_v2_size();

	REGISTER_V2_OR_SKIP(RSEQ_FLAG_SLICE_EXT_DEFAULT_ON);

	ASSERT_EQ(sys_rseq(buf, len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);
	EXPECT_EQ(slice_get(), 0);
	EXPECT_EQ(slice_set(0), -ENXIO);

	ASSERT_EQ(sys_rseq(buf, len, 0, RSEQ_SIG), 0);
	EXPECT_EQ(rs->flags & SLICE_FLAGS, SLICE_AVAILABLE);
	EXPECT_EQ(slice_get(), 0);
}

static int child_reads_state(void *arg)
{
	return slice_get() == PR_RSEQ_SLICE_EXT_ENABLE &&
	       (rs->flags & SLICE_FLAGS) == SLICE_FLAGS ? 0 : 1;
}

TEST(fork_child_keeps_the_state)
{
	REGISTER_V2_OR_SKIP(RSEQ_FLAG_SLICE_EXT_DEFAULT_ON);

	EXPECT_EQ(rseq_run_child(child_reads_state, NULL), 0);
}

static int child_changes_flags(void *arg)
{
	rs->flags &= ~SLICE_AVAILABLE;
	return slice_set(PR_RSEQ_SLICE_EXT_ENABLE) ? 1 : 0;
}

/* The flags are read-only: the kernel checks them when it changes them. */
TEST(changed_flags_kill)
{
	REGISTER_V2_OR_SKIP(0);

	EXPECT_EQ(rseq_run_child(child_changes_flags, NULL), -SIGSEGV);
}

TEST(yield_without_a_grant)
{
	long ret;

	REGISTER_V2_OR_SKIP(RSEQ_FLAG_SLICE_EXT_DEFAULT_ON);

	ret = syscall(__NR_rseq_slice_yield);
	if (ret < 0 && errno == ENOSYS)
		SKIP(return, "no rseq_slice_yield(2)");
	EXPECT_EQ(ret, 0);
	/* A request alone is no grant. */
	rs->slice_ctrl.request = 1;
	EXPECT_EQ(syscall(__NR_rseq_slice_yield), 0);
	EXPECT_EQ(rs->slice_ctrl.granted, 0);
	rs->slice_ctrl.request = 0;
}

int main(int argc, char **argv)
{
	rseq_reexec_without_libc(argv);
	return test_harness_run(argc, argv);
}
