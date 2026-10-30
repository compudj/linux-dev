// SPDX-License-Identifier: GPL-2.0
/*
 * Concurrency IDs read through the rseq selftest library
 * (tools/testing/selftests/rseq), for the tests of cpu.concurrency.max.
 *
 * This file is built on its own. The library declares the rseq symbols of
 * glibc itself, and <sys/rseq.h> declares them another way. The other
 * files of these tests include <sys/rseq.h>.
 *
 * The library uses one of two rseq areas:
 *  - glibc registered an area for each thread. The library uses it. glibc
 *    2.41 and later register an area of the feature size of the kernel.
 *    Older ones register 32 bytes, and ns_cid lies past them, see
 *    lib_rseq_glibc_hides_ns_cid().
 *  - glibc registered none. The library registers an area of the feature
 *    size of the kernel for each thread which calls lib_rseq_register().
 */

/* The architectures of ../rseq/rseq.h. */
#if defined(__x86_64__) || defined(__i386__) || defined(__ARMEL__) || \
	defined(__AARCH64EL__) || defined(__PPC__) || defined(__mips__) || \
	defined(__s390__) || defined(__riscv) || defined(__or1k__)

#include "../rseq/rseq.c"
#include "cpu_concurrency_rseq.h"

/* Set once the library has registered the calling thread. */
static __thread int lib_rseq_registered;

/*
 * Register the calling thread with the library. Each thread calls this
 * before lib_rseq_id() and lib_rseq_count_by_id(). Return 0, or -1 without
 * rseq.
 */
int lib_rseq_register(void)
{
	if (lib_rseq_registered)
		return 0;
	if (rseq_register_current_thread())
		return -1;
	lib_rseq_registered = 1;
	return 0;
}

/*
 * Undo lib_rseq_register(). Each thread calls this before it ends, as the
 * rseq selftests do.
 */
void lib_rseq_unregister(void)
{
	if (!lib_rseq_registered)
		return;
	rseq_unregister_current_thread();
	lib_rseq_registered = 0;
}

static unsigned int lib_rseq_id_end(int kind)
{
	return kind == LIB_RSEQ_NS_CID ? rseq_offsetofend(struct rseq_abi, ns_cid) :
					 rseq_offsetofend(struct rseq_abi, mm_cid);
}

/*
 * Return 1 if the rseq area of a registered thread has the ID @kind. The
 * calling thread need not be registered.
 */
int lib_rseq_has_id(int kind)
{
	unsigned int size = rseq_ownership ? get_rseq_kernel_feature_size() : rseq_size;

	return size >= lib_rseq_id_end(kind);
}

/*
 * Return 1 if the kernel has ns_cid and the area which glibc registered
 * is too small for it. A program then reads ns_cid through the library
 * only if it starts with GLIBC_TUNABLES=glibc.pthread.rseq=0: glibc then
 * registers no area and the library registers its own.
 */
int lib_rseq_glibc_hides_ns_cid(void)
{
	return !rseq_ownership && !lib_rseq_has_id(LIB_RSEQ_NS_CID) &&
	       get_rseq_kernel_feature_size() >= lib_rseq_id_end(LIB_RSEQ_NS_CID);
}

/* Return the ID @kind of the calling thread. */
unsigned int lib_rseq_id(int kind)
{
	return kind == LIB_RSEQ_NS_CID ? rseq_current_ns_cid() : rseq_current_mm_cid();
}

/*
 * Add 1 to @table[id], where id is the ID @kind of the calling thread.
 * rseq_addv() does it in an rseq critical section, with no lock and no
 * atomic instruction. The kernel aborts the section when the thread is
 * preempted, migrated or gets a signal between the check of the ID and
 * the store. The ID is then read again. So two threads write the same
 * entry at the same time only if they run at once with the same ID. They
 * then lose increments, and the sum of the table falls short of the
 * number of calls which returned 0.
 *
 * Set @idp to the ID. Return 0 when the entry was incremented, 1 when the
 * ID is not below @n.
 */
int lib_rseq_count_by_id(int kind, unsigned long *table, unsigned int n, unsigned int *idp)
{
	enum rseq_percpu_mode mode = kind == LIB_RSEQ_NS_CID ? RSEQ_PERCPU_NS_CID :
							       RSEQ_PERCPU_MM_CID;
	unsigned int id;

	for (;;) {
		id = lib_rseq_id(kind);
		*idp = id;
		if (id >= n)
			return 1;
		if (!rseq_addv(RSEQ_MO_RELAXED, mode, (intptr_t *)&table[id], 1, id))
			return 0;
	}
}

#else /* The library does not support this architecture. */

#include "cpu_concurrency_rseq.h"

int lib_rseq_register(void)
{
	return -1;
}

void lib_rseq_unregister(void)
{
}

int lib_rseq_has_id(int kind)
{
	return 0;
}

int lib_rseq_glibc_hides_ns_cid(void)
{
	return 0;
}

unsigned int lib_rseq_id(int kind)
{
	return 0;
}

int lib_rseq_count_by_id(int kind, unsigned long *table, unsigned int n, unsigned int *idp)
{
	*idp = 0;
	return 1;
}

#endif
