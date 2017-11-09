#ifndef _ASM_POWERPC_MEMBARRIER_H
#define _ASM_POWERPC_MEMBARRIER_H

static inline void membarrier_arch_switch_mm(struct mm_struct *prev,
		struct mm_struct *next, struct task_struct *tsk)
{
	/*
	 * Only need the full barrier when switching between processes.
	 * Barrier when switching from kernel to userspace is not
	 * required here, given that it is implied by mmdrop(). Barrier
	 * when switching from userspace to kernel is not needed after
	 * store to rq->curr.
	 */
	if (likely(!(atomic_read(&next->membarrier_state)
			& (MEMBARRIER_STATE_SWITCH_MM
			| MEMBARRIER_STATE_SHARED_EXPEDITED)) || !prev))
		return;

	/*
	 * The membarrier system call requires a full memory barrier
	 * after storing to rq->curr, before going back to user-space.
	 */
	smp_mb();
}

static inline void membarrier_arch_mm_sync_core(void)
{
}

void membarrier_arch_register_private_expedited(struct task_struct *t,
		int flags);

#endif /* _ASM_POWERPC_MEMBARRIER_H */
