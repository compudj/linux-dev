// SPDX-License-Identifier: LGPL-2.1
/*
 * What the kernel does with the rseq_cs field of a registered area.
 *
 * User space stores the address of a critical section descriptor,
 * struct rseq_cs, in rseq_cs. When the kernel preempts the thread or
 * delivers a signal to it, it reads the descriptor:
 * - If the thread was inside the critical section, it resumes at the
 *   abort address, with rseq_cs cleared. The four bytes before that
 *   address must hold the signature given at registration.
 * - If the thread was outside, the kernel clears rseq_cs.
 * - A descriptor the kernel cannot use kills the process with SIGSEGV.
 *
 * The other rseq tests run correct critical sections written in
 * assembly. The cases here give the kernel descriptors made by hand:
 * - A loop in a function of its own stands for the critical section. The
 *   abort address is another function. The signature is whatever four
 *   bytes come before that function: the test registers with them.
 * - rseq_cs is cleared when the descriptor does not cover the loop.
 * - A wrong signature, an abort address in the kernel, and a pointer
 *   that is not mapped or not a user address all kill. With a handler
 *   for SIGSEGV, the handler runs.
 * - The debug mode of rseq also kills on:
 *   - a system call inside a critical section;
 *   - a descriptor with flags;
 *   - an abort address inside the critical section;
 *   - a write to a read-only field in the legacy mode.
 *
 * The thread spins in user space until a timer interrupt delivers a
 * signal, or until another task preempts it. The kernel looks at rseq_cs
 * in both modes then.
 *
 * The tests register their own area, so they run without the registration
 * of glibc.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>

#include "rseq-own-area.h"
#include "../kselftest_harness.h"

/* A user address is never this high, on any architecture. */
#define KERNEL_ADDRESS	((uint64_t)-4096)

/* How many timer signals a child waits for before it gives up. */
#define SIGNAL_ROUNDS	20

/* Each of the two functions below starts its own page of code. */
#define CODE_PAGE	4096

/* The exit status of a child that resumed at the abort address. */
#define ABORTED		42
/* The same, but rseq_cs was not cleared. */
#define ABORTED_NOT_CLEARED	43
/* The exit status of a child whose SIGSEGV handler ran. */
#define CAUGHT_SIGSEGV	44

static char buf[4096] __attribute__((aligned(4096)));
static struct rseq_abi *rs = (struct rseq_abi *)buf;
static struct rseq_abi_cs cs __attribute__((aligned(32)));

static volatile int signals;

static void on_alarm(int sig)
{
	signals++;
}

/*
 * The critical section: a loop that ends when a signal handler has run
 * @nr times. It makes no system call and calls no function, so a timer
 * interrupt finds the thread inside it.
 */
static __attribute__((noinline, aligned(CODE_PAGE))) void spin_in_cs(int nr)
{
	while (signals < nr)
		;
}

/* The abort address. The kernel resumes the thread here. */
static __attribute__((noinline, noreturn, aligned(CODE_PAGE))) void abort_landing(void)
{
	_exit(rs->rseq_cs.ptr64 ? ABORTED_NOT_CLEARED : ABORTED);
}

/* The four bytes before the abort address: the signature to register. */
static uint32_t landing_signature(void)
{
	volatile uintptr_t landing = (uintptr_t)abort_landing;
	uint32_t sig;

	memcpy(&sig, (void *)(landing - sizeof(sig)), sizeof(sig));
	return sig;
}

/*
 * Store @rseq_cs in the area, then run spin_in_cs() until a timer
 * interrupt has delivered @nr signals. The store comes after the last
 * system call: the debug mode clears rseq_cs at the return of a system
 * call made outside the critical section.
 */
static void spin_until_signals(int nr, uint64_t rseq_cs)
{
	struct itimerval it = {
		.it_value = { .tv_usec = 10000 },
		.it_interval = { .tv_usec = 10000 },
	};
	struct itimerval off = { };

	signal(SIGALRM, on_alarm);
	signals = 0;
	setitimer(ITIMER_REAL, &it, NULL);
	rs->rseq_cs.ptr64 = rseq_cs;
	spin_in_cs(nr);
	setitimer(ITIMER_REAL, &off, NULL);
}

/* What a child stores in rseq_cs before it is interrupted. */
enum {
	CS_INSIDE,		/* covers the loop: the kernel aborts it */
	CS_OUTSIDE,		/* covers no code */
	CS_BAD_SIGNATURE,	/* covers the loop, registered with another signature */
	CS_KERNEL_ABORT,	/* covers the loop, abort address in the kernel */
	CS_NOT_MAPPED,		/* rseq_cs points at no memory */
	CS_KERNEL_POINTER,	/* rseq_cs is not a user address */
	CS_FLAGS,		/* covers the loop, flags not zero */
	CS_ABORT_INSIDE,	/* covers the loop and the abort address */
	CS_SYSCALL,		/* covers the C library, then a system call */
};

struct cs_case {
	uint32_t len;
	int what;
	bool catch_sigsegv;
};

static void on_sigsegv(int sig)
{
	_exit(CAUGHT_SIGSEGV);
}

/* A descriptor for the loop of spin_in_cs(). */
static void cover_the_loop(void)
{
	cs.version = 0;
	cs.flags = 0;
	cs.start_ip = (uintptr_t)spin_in_cs;
	cs.post_commit_offset = CODE_PAGE;
	cs.abort_ip = (uintptr_t)abort_landing;
}

/*
 * Register, store what @arg says in rseq_cs, and get interrupted. Return
 * 0 if rseq_cs is cleared afterwards, 1 if it is not, 2 on an error. A
 * child that the kernel aborts exits from abort_landing().
 */
static int cs_child(void *arg)
{
	uint64_t rseq_cs = (uintptr_t)&cs;
	uint32_t sig = landing_signature();
	struct cs_case *c = arg;
	uintptr_t lo, hi;
	void *hole;

	if (c->what == CS_BAD_SIGNATURE)
		sig = ~sig;
	if (c->catch_sigsegv)
		signal(SIGSEGV, on_sigsegv);
	memset(buf, 0, sizeof(buf));
	if (sys_rseq(buf, c->len, 0, sig))
		return 2;

	cover_the_loop();
	switch (c->what) {
	case CS_INSIDE:
	case CS_BAD_SIGNATURE:
		break;
	case CS_OUTSIDE:
		/* One byte of the first page: no code runs there. */
		cs.start_ip = 0;
		cs.post_commit_offset = 1;
		break;
	case CS_KERNEL_ABORT:
		cs.abort_ip = KERNEL_ADDRESS;
		break;
	case CS_NOT_MAPPED:
		hole = mmap(NULL, sizeof(buf), PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (hole == MAP_FAILED || munmap(hole, sizeof(buf)))
			return 2;
		rseq_cs = (uintptr_t)hole;
		break;
	case CS_KERNEL_POINTER:
		rseq_cs = KERNEL_ADDRESS;
		break;
	case CS_FLAGS:
		cs.flags = RSEQ_ABI_CS_FLAG_NO_RESTART_ON_PREEMPT;
		break;
	case CS_ABORT_INSIDE:
		lo = (uintptr_t)spin_in_cs;
		hi = (uintptr_t)abort_landing;
		cs.start_ip = lo < hi ? lo : hi;
		cs.post_commit_offset = (lo < hi ? hi - lo : lo - hi) + CODE_PAGE;
		break;
	case CS_SYSCALL:
		/* From the first page up to the stack: the C library is in. */
		cs.start_ip = CODE_PAGE;
		cs.post_commit_offset = (uintptr_t)&hole - CODE_PAGE;
		rs->rseq_cs.ptr64 = rseq_cs;
		getppid();
		return 0;
	}
	spin_until_signals(SIGNAL_ROUNDS, rseq_cs);
	return !!rs->rseq_cs.ptr64;
}

FIXTURE(area)
{
	uint32_t len;
};

FIXTURE_VARIANT(area)
{
	bool v2;
};

FIXTURE_VARIANT_ADD(area, legacy)
{
	.v2 = false,
};

FIXTURE_VARIANT_ADD(area, v2)
{
	.v2 = true,
};

FIXTURE_SETUP(area)
{
	self->len = variant->v2 ? rseq_v2_size() : RSEQ_ORIG_SIZE;
	if (!self->len)
		SKIP(return, "the kernel has no feature beyond the original area");
}

FIXTURE_TEARDOWN(area)
{
}

TEST_F(area, cleared_when_outside_at_a_signal)
{
	struct cs_case c = { .len = self->len, .what = CS_OUTSIDE };

	EXPECT_EQ(rseq_run_child(cs_child, &c), 0);
}

/* A task of the same CPU that spins, so that the tick preempts. */
static pid_t start_competitor(int cpu)
{
	pid_t pid = fork();

	if (!pid) {
		if (rseq_pin(cpu))
			_exit(1);
		for (;;)
			;
	}
	return pid;
}

static int preempted_child(void *arg)
{
	struct cs_case *c = arg;
	struct timespec start, now;
	pid_t competitor;
	int ret = 1;

	memset(buf, 0, sizeof(buf));
	if (rseq_pin(sched_getcpu()) || sys_rseq(buf, c->len, 0, RSEQ_SIG))
		return 2;
	competitor = start_competitor(sched_getcpu());
	if (competitor < 0)
		return 2;

	cs.version = 0;
	cs.flags = 0;
	cs.start_ip = 0;
	cs.post_commit_offset = 1;
	cs.abort_ip = 0;
	rs->rseq_cs.ptr64 = (uintptr_t)&cs;
	/* Spin without a system call until the kernel has preempted. */
	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		if (!rs->rseq_cs.ptr64) {
			ret = 0;
			break;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (now.tv_sec - start.tv_sec < 5);

	kill(competitor, SIGKILL);
	waitpid(competitor, NULL, 0);
	return ret;
}

TEST_F(area, cleared_when_outside_at_a_preemption)
{
	struct cs_case c = { .len = self->len, .what = CS_OUTSIDE };

	EXPECT_EQ(rseq_run_child(preempted_child, &c), 0);
}

TEST_F(area, aborted_when_inside_at_a_signal)
{
	struct cs_case c = { .len = self->len, .what = CS_INSIDE };

	EXPECT_EQ(rseq_run_child(cs_child, &c), ABORTED);
}

TEST_F(area, wrong_signature_kills)
{
	struct cs_case c = { .len = self->len, .what = CS_BAD_SIGNATURE };

	EXPECT_EQ(rseq_run_child(cs_child, &c), -SIGSEGV);
}

/*
 * The kernel sends SIGSEGV for a descriptor it cannot use. A handler for
 * SIGSEGV runs, as for any other fault. The kernel must neither jump to
 * the abort address nor send the signal for ever.
 */
TEST_F(area, wrong_signature_with_a_handler)
{
	struct cs_case c = {
		.len = self->len,
		.what = CS_BAD_SIGNATURE,
		.catch_sigsegv = true,
	};

	EXPECT_EQ(rseq_run_child(cs_child, &c), CAUGHT_SIGSEGV);
}

TEST_F(area, abort_address_in_the_kernel_kills)
{
	struct cs_case c = { .len = self->len, .what = CS_KERNEL_ABORT };

	EXPECT_EQ(rseq_run_child(cs_child, &c), -SIGSEGV);
}

TEST_F(area, descriptor_not_mapped_kills)
{
	struct cs_case c = { .len = self->len, .what = CS_NOT_MAPPED };

	EXPECT_EQ(rseq_run_child(cs_child, &c), -SIGSEGV);
}

TEST_F(area, descriptor_in_the_kernel_kills)
{
	struct cs_case c = { .len = self->len, .what = CS_KERNEL_POINTER };

	EXPECT_EQ(rseq_run_child(cs_child, &c), -SIGSEGV);
}

/* Write @on to the debug switch. Return 0, or -1. */
static int set_debug_mode(int on)
{
	int fd = open(RSEQ_DEBUGFS_DEBUG, O_WRONLY);
	int ret;

	if (fd < 0)
		return -1;
	ret = write(fd, on ? "1" : "0", 1) == 1 ? 0 : -1;
	close(fd);
	return ret;
}

static int tampered_child(void *arg)
{
	struct cs_case *c = arg;

	memset(buf, 0, sizeof(buf));
	if (sys_rseq(buf, c->len, 0, RSEQ_SIG))
		return 2;
	rs->cpu_id_start ^= 1;
	if (rseq_migrate_and_back())
		return 3;
	return 0;
}

/*
 * The debug mode is a switch for the whole system. The case turns it on,
 * and puts it back as it was.
 */
TEST_F(area, debug_mode_kills_more)
{
	struct cs_case c = { .len = self->len };
	int was = rseq_debug_mode(), ret;

	if (was < 0 || set_debug_mode(1))
		SKIP(return, "the debug switch of rseq needs root and debugfs");

	/* A critical section must make no system call. */
	c.what = CS_SYSCALL;
	EXPECT_EQ(rseq_run_child(cs_child, &c), -SIGSEGV);
	/* The flags of a descriptor are not supported any more. */
	c.what = CS_FLAGS;
	EXPECT_EQ(rseq_run_child(cs_child, &c), -SIGSEGV);
	/* The abort address must be outside the critical section. */
	c.what = CS_ABORT_INSIDE;
	EXPECT_EQ(rseq_run_child(cs_child, &c), -SIGSEGV);
	/* The read-only fields are checked in the legacy mode too. */
	ret = rseq_run_child(tampered_child, &c);
	if (ret != 3)
		EXPECT_EQ(ret, -SIGSEGV);
	/* Correct descriptors still work. */
	c.what = CS_OUTSIDE;
	EXPECT_EQ(rseq_run_child(cs_child, &c), 0);
	c.what = CS_INSIDE;
	EXPECT_EQ(rseq_run_child(cs_child, &c), ABORTED);

	EXPECT_EQ(set_debug_mode(was), 0);

	/* Without the debug mode, the first three go unnoticed. */
	if (!was) {
		c.what = CS_SYSCALL;
		EXPECT_EQ(rseq_run_child(cs_child, &c), 0);
		c.what = CS_FLAGS;
		EXPECT_EQ(rseq_run_child(cs_child, &c), ABORTED);
		c.what = CS_ABORT_INSIDE;
		EXPECT_EQ(rseq_run_child(cs_child, &c), ABORTED);
	}
}

int main(int argc, char **argv)
{
	rseq_reexec_without_libc(argv);
	return test_harness_run(argc, argv);
}
