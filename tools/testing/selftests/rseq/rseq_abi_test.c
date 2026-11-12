// SPDX-License-Identifier: LGPL-2.1
/*
 * The registration ABI of rseq(2).
 *
 * A thread registers one area of memory with rseq(2). The kernel then
 * keeps the CPU number, the NUMA node and the concurrency ID of the thread
 * up to date in it.
 *
 * An area of 32 bytes, the size of the original struct rseq, gives the
 * legacy mode. A larger area must hold at least the feature size that the
 * kernel reports in the auxiliary vector, AT_RSEQ_FEATURE_SIZE. It must be
 * aligned on AT_RSEQ_ALIGN. Such a registration gives the optimized mode,
 * called V2, on architectures that support it.
 *
 * The cases check:
 * - the size, the alignment and the error number of each refused call;
 * - what the kernel writes at a registration and at an unregistration;
 * - that the IDs follow the thread from CPU to CPU;
 * - that the kernel keeps its hands off the bytes after mm_cid in the
 *   legacy mode;
 * - that a write to a read-only field kills the process in the V2 mode,
 *   and not in the legacy mode.
 *
 * The tests register their own area, so they run without the registration
 * of glibc.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stddef.h>
#include <sys/mman.h>

#include "rseq-own-area.h"
#include "../kselftest_harness.h"

/* Not a flag of rseq(2). */
#define UNKNOWN_FLAG	(1 << 30)

static char buf[4096] __attribute__((aligned(4096)));
static struct rseq_abi *rs = (struct rseq_abi *)buf;

static int getcpu_node(void)
{
	unsigned int cpu, node;

	if (syscall(__NR_getcpu, &cpu, &node, NULL))
		return -1;
	return node;
}

TEST(auxiliary_vector)
{
	unsigned long size = getauxval(AT_RSEQ_FEATURE_SIZE);
	unsigned long align = getauxval(AT_RSEQ_ALIGN);

	if (!size)
		SKIP(return, "no AT_RSEQ_FEATURE_SIZE: the kernel has no extensible rseq");

	/* The node_id and mm_cid fields came with the auxiliary vector. */
	EXPECT_GE(size, rseq_offsetofend(struct rseq_abi, mm_cid));
	/* A feature size of 32 would look like the original area size. */
	EXPECT_NE(size, RSEQ_ORIG_SIZE);
	EXPECT_GE(align, RSEQ_ORIG_SIZE);
	EXPECT_GE(align, size);
	EXPECT_EQ(align & (align - 1), 0);
	EXPECT_LE(align, sizeof(buf));
}

TEST(size_and_alignment)
{
	uint32_t v2 = rseq_v2_size();
	unsigned long align = rseq_area_align(v2);

	/* Smaller than the original area. */
	EXPECT_EQ(sys_rseq(buf, 0, 0, RSEQ_SIG), -EINVAL);
	EXPECT_EQ(sys_rseq(buf, 20, 0, RSEQ_SIG), -EINVAL);
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE - 1, 0, RSEQ_SIG), -EINVAL);

	/* The original size needs the original alignment. */
	EXPECT_EQ(sys_rseq(buf + 1, RSEQ_ORIG_SIZE, 0, RSEQ_SIG), -EINVAL);
	EXPECT_EQ(sys_rseq(buf + 16, RSEQ_ORIG_SIZE, 0, RSEQ_SIG), -EINVAL);
	ASSERT_EQ(sys_rseq(buf + 32, RSEQ_ORIG_SIZE, 0, RSEQ_SIG), 0);
	ASSERT_EQ(sys_rseq(buf + 32, RSEQ_ORIG_SIZE, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);

	if (!v2)
		SKIP(return, "the kernel has no feature beyond the original area");

	/* A larger area holds the feature size, on the reported alignment. */
	if (v2 > RSEQ_ORIG_SIZE + 1)
		EXPECT_EQ(sys_rseq(buf, v2 - 1, 0, RSEQ_SIG), -EINVAL);
	EXPECT_EQ(sys_rseq(buf + RSEQ_ORIG_SIZE, v2, 0, RSEQ_SIG), -EINVAL);
	EXPECT_EQ(sys_rseq(buf + align / 2, v2, 0, RSEQ_SIG), -EINVAL);
	ASSERT_EQ(sys_rseq(buf, v2, 0, RSEQ_SIG), 0);
	ASSERT_EQ(sys_rseq(buf, v2, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);

	/* An area larger than the feature size is fine. */
	ASSERT_EQ(sys_rseq(buf + align, 2 * align, 0, RSEQ_SIG), 0);
	ASSERT_EQ(sys_rseq(buf + align, 2 * align, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);
}

TEST(registration_errors)
{
	EXPECT_EQ(sys_rseq(NULL, 0, 0, 0), -EINVAL);
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, UNKNOWN_FLAG, RSEQ_SIG), -EINVAL);
#if defined(__LP64__) && !defined(__s390x__)
	/* No address is out of reach of a 32-bit task, or on s390. */
	EXPECT_EQ(sys_rseq((void *)-4096UL, RSEQ_ORIG_SIZE, 0, RSEQ_SIG), -EFAULT);
#endif
}

TEST(second_registration)
{
	uint32_t other_len = rseq_v2_size();

	ASSERT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, 0, RSEQ_SIG), 0);

	/* The same area again. */
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, 0, RSEQ_SIG), -EBUSY);
	/* Another signature for the same area. */
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, 0, RSEQ_SIG + 1), -EPERM);
	/* Another area, or another length. The signature comes second. */
	EXPECT_EQ(sys_rseq(buf + 1024, RSEQ_ORIG_SIZE, 0, RSEQ_SIG), -EINVAL);
	EXPECT_EQ(sys_rseq(buf + 1024, RSEQ_ORIG_SIZE, 0, RSEQ_SIG + 1), -EINVAL);
	if (other_len)
		EXPECT_EQ(sys_rseq(buf, other_len, 0, RSEQ_SIG), -EINVAL);

	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);
}

TEST(unregistration_errors)
{
	/* Nothing is registered. */
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), -EINVAL);

	ASSERT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, 0, RSEQ_SIG), 0);

	EXPECT_EQ(sys_rseq(buf + 1024, RSEQ_ORIG_SIZE, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG),
		  -EINVAL);
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE + 32, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG),
		  -EINVAL);
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG + 1),
		  -EPERM);
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, RSEQ_ABI_FLAG_UNREGISTER | UNKNOWN_FLAG,
			   RSEQ_SIG), -EINVAL);

	/* The refused calls left the registration in place. */
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, 0, RSEQ_SIG), -EBUSY);
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);
	EXPECT_EQ(sys_rseq(buf, RSEQ_ORIG_SIZE, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), -EINVAL);
}

FIXTURE(area)
{
	uint32_t len;
	cpu_set_t allowed;
};

FIXTURE_VARIANT(area)
{
	bool v2;
};

/* The original 32 bytes: the legacy mode. */
FIXTURE_VARIANT_ADD(area, legacy)
{
	.v2 = false,
};

/* The feature size of the kernel: the V2 mode, where supported. */
FIXTURE_VARIANT_ADD(area, v2)
{
	.v2 = true,
};

FIXTURE_SETUP(area)
{
	self->len = variant->v2 ? rseq_v2_size() : RSEQ_ORIG_SIZE;
	if (!self->len)
		SKIP(return, "the kernel has no feature beyond the original area");
	/* The kernel must not trust what the area holds. */
	memset(buf, 0xff, sizeof(buf));
	/* Stay on one CPU, so that sched_getcpu() can be compared. */
	ASSERT_EQ(sched_getaffinity(0, sizeof(self->allowed), &self->allowed), 0);
	ASSERT_EQ(rseq_pin(sched_getcpu()), 0);
}

FIXTURE_TEARDOWN(area)
{
}

TEST_F(area, fields_after_registration)
{
	int i;

	ASSERT_EQ(sys_rseq(buf, self->len, 0, RSEQ_SIG), 0);

	EXPECT_EQ(rs->cpu_id_start, sched_getcpu());
	EXPECT_EQ(rs->cpu_id, sched_getcpu());
	EXPECT_EQ(rs->node_id, getcpu_node());
	EXPECT_LT(rs->mm_cid, sysconf(_SC_NPROCESSORS_CONF));
	/* A stale pointer must not survive: its first use would kill. */
	EXPECT_EQ(rs->rseq_cs.ptr64, 0);

	/* A legacy user may keep its own data after mm_cid. */
	if (!variant->v2) {
		for (i = rseq_offsetofend(struct rseq_abi, mm_cid); i < RSEQ_ORIG_SIZE; i++)
			EXPECT_EQ((unsigned char)buf[i], 0xff);
	}
	/* The kernel writes nothing after the area. */
	for (i = self->len; i < 2 * (int)self->len; i++)
		EXPECT_EQ((unsigned char)buf[i], 0xff);

	EXPECT_EQ(sys_rseq(buf, self->len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);
}

/*
 * User space built for a later kernel can register a larger area than
 * this kernel knows. The kernel must leave the bytes after its feature
 * size alone: they belong to fields it does not have.
 */
TEST(larger_area_keeps_its_tail)
{
	uint32_t v2 = rseq_v2_size();
	unsigned long len = 4 * rseq_area_align(v2);
	unsigned long i;

	if (!v2)
		SKIP(return, "the kernel has no feature beyond the original area");
	ASSERT_LE(len, sizeof(buf));

	memset(buf, 0xff, sizeof(buf));
	ASSERT_EQ(sys_rseq(buf, len, 0, RSEQ_SIG), 0);
	ASSERT_EQ(rseq_pin(sched_getcpu()), 0);
	EXPECT_EQ(rs->cpu_id, sched_getcpu());
	for (i = v2; i < len; i++)
		EXPECT_EQ((unsigned char)buf[i], 0xff);
	ASSERT_EQ(sys_rseq(buf, len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);
	for (i = v2; i < len; i++)
		EXPECT_EQ((unsigned char)buf[i], 0xff);
}

TEST_F(area, fields_after_unregistration)
{
	ASSERT_EQ(sys_rseq(buf, self->len, 0, RSEQ_SIG), 0);
	ASSERT_EQ(sys_rseq(buf, self->len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);

	/* A possible CPU number, and the mark of an area not in use. */
	EXPECT_EQ(rs->cpu_id_start, 0);
	EXPECT_EQ(rs->cpu_id, (uint32_t)RSEQ_ABI_CPU_ID_UNINITIALIZED);
	EXPECT_EQ(rs->node_id, 0);
	EXPECT_EQ(rs->mm_cid, 0);

	/* The area can be registered again. */
	EXPECT_EQ(sys_rseq(buf, self->len, 0, RSEQ_SIG), 0);
	EXPECT_EQ(rs->cpu_id, sched_getcpu());
	EXPECT_EQ(sys_rseq(buf, self->len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);
}

TEST_F(area, ids_follow_the_cpu)
{
	int cpu, visited = 0;

	ASSERT_EQ(sys_rseq(buf, self->len, 0, RSEQ_SIG), 0);

	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, &self->allowed) || rseq_pin(cpu))
			continue;
		visited++;
		EXPECT_EQ(rs->cpu_id_start, cpu);
		EXPECT_EQ(rs->cpu_id, cpu);
		EXPECT_EQ(rs->node_id, getcpu_node());
		EXPECT_LT(rs->mm_cid, sysconf(_SC_NPROCESSORS_CONF));
	}
	EXPECT_GT(visited, 0);

	EXPECT_EQ(sys_rseq(buf, self->len, RSEQ_ABI_FLAG_UNREGISTER, RSEQ_SIG), 0);
}

/* What a child of the read-only cases does to the area. */
struct tamper {
	uint32_t len;
	int what;
};

enum {
	TAMPER_NONE,
	TAMPER_CPU_ID_START,
	TAMPER_CPU_ID,
	TAMPER_NODE_ID,
	TAMPER_MM_CID,
	TAMPER_UNMAP,
	TAMPER_PROTECT,
};

/*
 * Register an area, damage it as @arg says, then change CPU. The kernel
 * writes the IDs on the way back to user space. Return 0 if the process
 * is still alive then.
 */
static int tamper_child(void *arg)
{
	struct tamper *t = arg;
	struct rseq_abi *area;

	area = mmap(NULL, sizeof(buf), PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED || sys_rseq(area, t->len, 0, RSEQ_SIG))
		return 2;

	switch (t->what) {
	case TAMPER_CPU_ID_START:
		area->cpu_id_start ^= 1;
		break;
	case TAMPER_CPU_ID:
		area->cpu_id ^= 1;
		break;
	case TAMPER_NODE_ID:
		area->node_id ^= 1;
		break;
	case TAMPER_MM_CID:
		area->mm_cid ^= 1;
		break;
	case TAMPER_UNMAP:
		if (munmap(area, sizeof(buf)))
			return 2;
		break;
	case TAMPER_PROTECT:
		if (mprotect(area, sizeof(buf), PROT_READ))
			return 2;
		break;
	}
	if (rseq_migrate_and_back())
		return 3;
	return 0;
}

TEST_F(area, area_must_stay_writable)
{
	struct tamper t = { .len = self->len };
	int ret;

	ASSERT_EQ(sched_setaffinity(0, sizeof(self->allowed), &self->allowed), 0);
	t.what = TAMPER_NONE;
	ret = rseq_run_child(tamper_child, &t);
	if (ret == 3)
		SKIP(return, "one CPU: the thread cannot change CPU");
	ASSERT_EQ(ret, 0);

	t.what = TAMPER_UNMAP;
	EXPECT_EQ(rseq_run_child(tamper_child, &t), -SIGSEGV);
	t.what = TAMPER_PROTECT;
	EXPECT_EQ(rseq_run_child(tamper_child, &t), -SIGSEGV);
}

/*
 * User space only reads cpu_id_start, cpu_id, node_id and mm_cid. The V2
 * mode checks them before each update and kills a process that changed
 * one. The legacy mode writes over the change.
 */
TEST_F(area, read_only_fields)
{
	struct tamper t = { .len = self->len };
	int ret, expect;

	ASSERT_EQ(sched_setaffinity(0, sizeof(self->allowed), &self->allowed), 0);
	t.what = TAMPER_NONE;
	ret = rseq_run_child(tamper_child, &t);
	if (ret == 3)
		SKIP(return, "one CPU: the thread cannot change CPU");
	ASSERT_EQ(ret, 0);

	/* The debug mode checks the fields in the legacy mode too. */
	if ((variant->v2 && RSEQ_ARCH_HAS_V2) || rseq_debug_mode() == 1)
		expect = -SIGSEGV;
	else
		expect = 0;
	for (t.what = TAMPER_CPU_ID_START; t.what <= TAMPER_MM_CID; t.what++) {
		ret = rseq_run_child(tamper_child, &t);
		EXPECT_EQ(ret, expect) {
			TH_LOG("field %d of an area of %u bytes", t.what, t.len);
		}
	}
}

int main(int argc, char **argv)
{
	rseq_reexec_without_libc(argv);
	return test_harness_run(argc, argv);
}
