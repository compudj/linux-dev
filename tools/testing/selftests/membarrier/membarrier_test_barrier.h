/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MEMBARRIER_TEST_BARRIER_H
#define MEMBARRIER_TEST_BARRIER_H

#include <linux/types.h>

/* Counters of a thread. */
enum {
	/* Memory barrier of the membarrier IPI handler. */
	MBT_IPI,
	/* Memory barriers of the membarrier system call. */
	MBT_SYSCALL,
	/* Memory barrier of the scheduler after the store to rq->curr. */
	MBT_SWITCH_MM,
	/* The thread was scheduled out. */
	MBT_SCHED_OUT,
	/*
	 * The thread was scheduled in after a task of another mm, or
	 * after a kernel thread.
	 */
	MBT_SCHED_IN,
	/*
	 * The thread was scheduled in as above, then scheduled out, and
	 * the scheduler executed no memory barrier in between.
	 */
	MBT_SCHED_IN_MISSED,
	MBT_NR_COUNTERS,
};

struct mbt_counters {
	__u64 c[16];
};

/* Argument of the register_thread program. */
struct mbt_register {
	__u32 slot;
};

#define MBT_MAX_SLOTS		512
#define MBT_MAX_SITES		64

/* Attach cookie of a probe: counter mask, flags, index of the site. */
#define MBT_COOKIE_MASK		0xff
/* Count for the thread which is scheduled in, not for current. */
#define MBT_COOKIE_NEXT		0x100
#define MBT_COOKIE_SITE_SHIFT	16

#endif /* MEMBARRIER_TEST_BARRIER_H */
