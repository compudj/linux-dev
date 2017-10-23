#ifndef _ASM_X86_MEMBARRIER_H
#define _ASM_X86_MEMBARRIER_H

#include <asm/processor.h>

static inline void membarrier_arch_switch_mm(struct mm_struct *prev,
		struct mm_struct *next, struct task_struct *tsk)
{
}

#ifdef CONFIG_X86_32
static inline void membarrier_arch_mm_sync_core(struct mm_struct *mm)
{
}
static inline
void membarrier_arch_register_private_expedited(struct task_struct *t,
		int flags);
#else
/*
 * x86-64 implements return to user-space through sysret, which is not a
 * core-serializing instruction. Therefore, we need an explicit core
 * serializing instruction after going from kernel thread back to
 * user-space thread (active_mm moved back to current mm).
 */
static inline void membarrier_arch_mm_sync_core(struct mm_struct *mm)
{
	if (likely(!(atomic_read(&mm->membarrier_state) &
			MEMBARRIER_STATE_SYNC_CORE)))
		return;
	sync_core();
}
void membarrier_arch_register_private_expedited(struct task_struct *t,
		int flags);
#endif

#endif /* _ASM_X86_MEMBARRIER_H */
