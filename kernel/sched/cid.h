/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The concurrency ID engine's inline parts, for the context switch, and the
 * prototypes of cid.c; see cid.c.
 */
#ifndef _KERNEL_SCHED_CID_H
#define _KERNEL_SCHED_CID_H

/* Included by sched.h, after concurrency.h. */

#ifdef CONFIG_SCHED_MM_CID
static __always_inline bool cid_on_cpu(unsigned int cid)
{
	return cid & MM_CID_ONCPU;
}

static __always_inline bool cid_in_transit(unsigned int cid)
{
	return cid & MM_CID_TRANSIT;
}

static __always_inline unsigned int cpu_cid_to_cid(unsigned int cid)
{
	return cid & ~MM_CID_ONCPU;
}

static __always_inline unsigned int cid_to_cpu_cid(unsigned int cid)
{
	return cid | MM_CID_ONCPU;
}

static __always_inline unsigned int cid_to_transit_cid(unsigned int cid)
{
	return cid | MM_CID_TRANSIT;
}

static __always_inline unsigned int cid_from_transit_cid(unsigned int cid)
{
	return cid & ~MM_CID_TRANSIT;
}

static __always_inline bool cid_on_task(unsigned int cid)
{
	/* True if none of the MM_CID_ONCPU, MM_CID_TRANSIT, MM_CID_UNSET bits is set */
	return cid < MM_CID_TRANSIT;
}

static __always_inline void mm_drop_cid(struct mm_struct *mm, unsigned int cid)
{
	clear_bit(cid, mm_cidmask(mm));
}

static __always_inline void mm_unset_cid_on_task(struct task_struct *t)
{
	unsigned int cid = t->mm_cid.cid;

	t->mm_cid.cid = MM_CID_UNSET;
	if (cid_on_task(cid))
		mm_drop_cid(t->mm, cid);
}

static __always_inline void mm_drop_cid_on_cpu(struct mm_struct *mm, struct mm_cid_pcpu *pcp)
{
	/* Clear the ONCPU bit, but do not set UNSET in the per CPU storage */
	if (cid_on_cpu(pcp->cid)) {
		pcp->cid = cpu_cid_to_cid(pcp->cid);
		mm_drop_cid(mm, pcp->cid);
	}
}

static inline unsigned int __mm_get_cid(struct mm_struct *mm, unsigned int max_cids)
{
	unsigned int cid = find_first_zero_bit(mm_cidmask(mm), max_cids);

	if (cid >= max_cids)
		return MM_CID_UNSET;
	if (test_and_set_bit(cid, mm_cidmask(mm)))
		return MM_CID_UNSET;
	return cid;
}

static inline unsigned int mm_get_cid(struct mm_struct *mm)
{
	unsigned int cid = __mm_get_cid(mm, READ_ONCE(mm->mm_cid.max_cids));

	while (cid == MM_CID_UNSET) {
		cpu_relax();
		cid = __mm_get_cid(mm, num_possible_cpus());
	}
	return cid;
}

static inline unsigned int mm_cid_converge(struct mm_struct *mm, unsigned int orig_cid,
					   unsigned int max_cids)
{
	unsigned int new_cid, cid = cpu_cid_to_cid(orig_cid);

	/* Is it in the optimal CID space? */
	if (likely(cid < max_cids))
		return orig_cid;

	/* Try to find one in the optimal space. Otherwise keep the provided. */
	new_cid = __mm_get_cid(mm, max_cids);
	if (new_cid != MM_CID_UNSET) {
		mm_drop_cid(mm, cid);
		/* Preserve the ONCPU mode of the original CID */
		return new_cid | (orig_cid & MM_CID_ONCPU);
	}
	return orig_cid;
}

static __always_inline void mm_cid_update_task_cid(struct task_struct *t, unsigned int cid)
{
	if (t->mm_cid.cid != cid) {
		t->mm_cid.cid = cid;
		rseq_sched_set_ids_changed(t);
	}
}

static __always_inline void mm_cid_update_pcpu_cid(struct mm_struct *mm, unsigned int cid)
{
	__this_cpu_write(mm->mm_cid.pcpu->cid, cid);
}

static __always_inline void mm_cid_from_cpu(struct task_struct *t, unsigned int cpu_cid,
					    unsigned int mode)
{
	unsigned int max_cids, tcid = t->mm_cid.cid;
	struct mm_struct *mm = t->mm;

	max_cids = READ_ONCE(mm->mm_cid.max_cids);
	/* Optimize for the common case where both have the ONCPU bit set */
	if (likely(cid_on_cpu(cpu_cid & tcid))) {
		if (likely(cpu_cid_to_cid(cpu_cid) < max_cids)) {
			mm_cid_update_task_cid(t, cpu_cid);
			return;
		}
		/* Try to converge into the optimal CID space */
		cpu_cid = mm_cid_converge(mm, cpu_cid, max_cids);
	} else {
		/* Hand over or drop the task owned CID */
		if (cid_on_task(tcid)) {
			if (cid_on_cpu(cpu_cid))
				mm_unset_cid_on_task(t);
			else
				cpu_cid = cid_to_cpu_cid(tcid);
		}
		/* Still nothing, allocate a new one */
		if (!cid_on_cpu(cpu_cid))
			cpu_cid = cid_to_cpu_cid(mm_get_cid(mm));

		/* Handle the transition mode flag if required */
		if (mode & MM_CID_TRANSIT)
			cpu_cid = cpu_cid_to_cid(cpu_cid) | MM_CID_TRANSIT;
	}
	mm_cid_update_pcpu_cid(mm, cpu_cid);
	mm_cid_update_task_cid(t, cpu_cid);
}

static __always_inline void mm_cid_from_task(struct task_struct *t, unsigned int cpu_cid,
					     unsigned int mode)
{
	unsigned int max_cids, tcid = t->mm_cid.cid;
	struct mm_struct *mm = t->mm;

	max_cids = READ_ONCE(mm->mm_cid.max_cids);
	/* Optimize for the common case, where both have the ONCPU bit clear */
	if (likely(cid_on_task(tcid | cpu_cid))) {
		if (likely(tcid < max_cids)) {
			mm_cid_update_pcpu_cid(mm, tcid);
			return;
		}
		/* Try to converge into the optimal CID space */
		tcid = mm_cid_converge(mm, tcid, max_cids);
	} else {
		/* Hand over or drop the CPU owned CID */
		if (cid_on_cpu(cpu_cid)) {
			if (cid_on_task(tcid))
				mm_drop_cid_on_cpu(mm, this_cpu_ptr(mm->mm_cid.pcpu));
			else
				tcid = cpu_cid_to_cid(cpu_cid);
		}
		/* Still nothing, allocate a new one */
		if (!cid_on_task(tcid))
			tcid = mm_get_cid(mm);
		/* Set the transition mode flag if required */
		tcid |= mode & MM_CID_TRANSIT;
	}
	mm_cid_update_pcpu_cid(mm, tcid);
	mm_cid_update_task_cid(t, tcid);
}

static __always_inline void mm_cid_schedin(struct task_struct *next)
{
	struct mm_struct *mm = next->mm;
	unsigned int cpu_cid, mode;

	if (!next->mm_cid.active)
		return;

	cpu_cid = __this_cpu_read(mm->mm_cid.pcpu->cid);
	mode = READ_ONCE(mm->mm_cid.mode);
	if (likely(!cid_on_cpu(mode)))
		mm_cid_from_task(next, cpu_cid, mode);
	else
		mm_cid_from_cpu(next, cpu_cid, mode);
}

static __always_inline void mm_cid_schedout(struct task_struct *prev)
{
	struct mm_struct *mm = prev->mm;
	unsigned int mode, cid;

	/* During mode transitions CIDs are temporary and need to be dropped */
	if (likely(!cid_in_transit(prev->mm_cid.cid)))
		return;

	mode = READ_ONCE(mm->mm_cid.mode);
	cid = cid_from_transit_cid(prev->mm_cid.cid);

	/*
	 * If transition mode is done, transfer ownership when the CID is
	 * within the convergence range to optimize the next schedule in.
	 */
	if (!cid_in_transit(mode) && cid < READ_ONCE(mm->mm_cid.max_cids)) {
		if (cid_on_cpu(mode))
			cid = cid_to_cpu_cid(cid);

		/* Update both so that the next schedule in goes into the fast path */
		mm_cid_update_pcpu_cid(mm, cid);
		prev->mm_cid.cid = cid;
	} else {
		mm_drop_cid(mm, cid);
		prev->mm_cid.cid = MM_CID_UNSET;
	}
}

static inline void mm_cid_switch_to(struct task_struct *prev, struct task_struct *next)
{
	mm_cid_schedout(prev);
	mm_cid_schedin(next);
}

/* Called from outside cid.c. */
void mm_update_cpus_allowed(struct mm_struct *mm, const struct cpumask *affmsk);

void sched_mm_cid_fork(struct task_struct *t);
#else /* !CONFIG_SCHED_MM_CID */
static inline void mm_cid_switch_to(struct task_struct *prev, struct task_struct *next) { }
static inline void mm_update_cpus_allowed(struct mm_struct *mm, const struct cpumask *affmsk) { }

static inline void sched_mm_cid_fork(struct task_struct *t) { }
#endif /* !CONFIG_SCHED_MM_CID */

#endif /* _KERNEL_SCHED_CID_H */
