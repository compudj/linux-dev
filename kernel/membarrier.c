/*
 * Copyright (C) 2010, 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * membarrier system call
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/syscalls.h>
#include <linux/membarrier.h>
#include <linux/tick.h>
#include <linux/smp.h>
#include <linux/sched.h>

/*
 * TODO: private sched.h is needed for runqueue. Should we move the
 * sched code under kernel/sched/ ?
 */
#include "sched/sched.h"

/*
 * Bitmask made from a "or" of all commands within enum membarrier_cmd,
 * except MEMBARRIER_CMD_QUERY.
 */
#define MEMBARRIER_CMD_BITMASK	\
	(MEMBARRIER_CMD_SHARED \
	| MEMBARRIER_CMD_REGISTER_EXPEDITED \
	| MEMBARRIER_CMD_UNREGISTER_EXPEDITED)

static int membarrier_register_expedited(struct task_struct *t)
{
	struct rq *rq;

	if (t->membarrier_expedited == UINT_MAX)
		return -EOVERFLOW;
	rq = this_rq();
	raw_spin_lock(&rq->lock);
	t->membarrier_expedited++;
	raw_spin_unlock(&rq->lock);
	return 0;
}

static int membarrier_unregister_expedited(struct task_struct *t)
{
	struct rq *rq;

	if (!t->membarrier_expedited)
		return -ENOENT;
	rq = this_rq();
	raw_spin_lock(&rq->lock);
	t->membarrier_expedited--;
	raw_spin_unlock(&rq->lock);
	return 0;
}

static void memory_barrier(void *info)
{
	smp_mb();
}

static void membarrier_nohz_full_expedited(void)
{
	int cpu;

	if (!tick_nohz_full_enabled())
		return;
	for_each_cpu(cpu, tick_nohz_full_mask) {
		struct rq *rq;
		struct task_struct *t;

		rq = cpu_rq(cpu);
		raw_spin_lock(&rq->lock);
		t = rq->curr;
		if (t->membarrier_expedited) {
			int ret;

			ret = smp_call_function_single(cpu, memory_barrier,
					NULL, 1);
			WARN_ON_ONCE(ret);
		}
		raw_spin_unlock(&rq->lock);
	}
}

/**
 * sys_membarrier - issue memory barriers on a set of threads
 * @cmd:   Takes command values defined in enum membarrier_cmd.
 * @flags: Currently needs to be 0. For future extensions.
 *
 * If this system call is not implemented, -ENOSYS is returned. If the
 * command specified does not exist, or if the command argument is invalid,
 * this system call returns -EINVAL. For a given command, with flags argument
 * set to 0, this system call is guaranteed to always return the same value
 * until reboot.
 *
 * All memory accesses performed in program order from each targeted thread
 * is guaranteed to be ordered with respect to sys_membarrier(). If we use
 * the semantic "barrier()" to represent a compiler barrier forcing memory
 * accesses to be performed in program order across the barrier, and
 * smp_mb() to represent explicit memory barriers forcing full memory
 * ordering across the barrier, we have the following ordering table for
 * each pair of barrier(), sys_membarrier() and smp_mb():
 *
 * The pair ordering is detailed as (O: ordered, X: not ordered):
 *
 *                        barrier()   smp_mb() sys_membarrier()
 *        barrier()          X           X            O
 *        smp_mb()           X           O            O
 *        sys_membarrier()   O           O            O
 */
SYSCALL_DEFINE2(membarrier, int, cmd, int, flags)
{
	if (unlikely(flags))
		return -EINVAL;
	switch (cmd) {
	case MEMBARRIER_CMD_QUERY:
		return MEMBARRIER_CMD_BITMASK;
	case MEMBARRIER_CMD_SHARED:
		if (num_online_cpus() > 1) {
			synchronize_sched();
			membarrier_nohz_full_expedited();
		}
		return 0;
	case MEMBARRIER_CMD_REGISTER_EXPEDITED:
		return membarrier_register_expedited(current);
	case MEMBARRIER_CMD_UNREGISTER_EXPEDITED:
		return membarrier_unregister_expedited(current);
	default:
		return -EINVAL;
	}
}
