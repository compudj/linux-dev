/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The concurrency ID engine's inline parts, for the context switch, and the
 * prototypes of cid.c; see cid.c.
 */
#ifndef _KERNEL_SCHED_CID_H
#define _KERNEL_SCHED_CID_H

/* Included by sched.h, after concurrency.h. */

#ifdef CONFIG_SCHED_MM_CID
/*
 * The concurrency ID engine, written against a cid domain: the mm of a
 * process (mm_cid). A domain hands out IDs to the tasks of its users that
 * run at the same time, either owned by the task for its lifetime
 * (per-task mode, few users) or owned by the CPU while a user runs on it
 * (per-CPU mode).
 */
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

/* The mm a domain is embedded in. */
static inline struct mm_struct *cid_domain_mm(struct cid_domain *dom)
{
	return container_of(dom, struct mm_struct, mm_cid);
}

/* The per-task user state of @t in @dom. */
static __always_inline struct cid_user *cid_task_user(struct cid_domain *dom, struct task_struct *t)
{
	return &t->mm_cid;
}

/* The task a user state belongs to. */
static inline struct task_struct *cid_user_task(struct cid_domain *dom, struct cid_user *u)
{
	return container_of(u, struct task_struct, mm_cid);
}

/* Is @t an active user of @dom? */
static __always_inline bool cid_task_in_domain(struct task_struct *t, struct cid_domain *dom)
{
	return t->mm == cid_domain_mm(dom) && t->mm_cid.active;
}

static __always_inline void cid_drop(struct cid_domain *dom, unsigned int cid)
{
	clear_bit(cid, dom->cidmask);
}

static __always_inline void cid_unset_on_task(struct cid_domain *dom, struct cid_user *u)
{
	unsigned int cid = u->cid;

	u->cid = MM_CID_UNSET;
	if (cid_on_task(cid))
		cid_drop(dom, cid);
}

static __always_inline void cid_drop_on_cpu(struct cid_domain *dom, struct cid_pcpu *pcp)
{
	/* Clear the ONCPU bit, but do not set UNSET in the per CPU storage */
	if (cid_on_cpu(pcp->cid)) {
		pcp->cid = cpu_cid_to_cid(pcp->cid);
		cid_drop(dom, pcp->cid);
	}
}

static inline unsigned int __cid_get(struct cid_domain *dom, unsigned int max_cids)
{
	unsigned int cid = find_first_zero_bit(dom->cidmask, max_cids);

	if (cid >= max_cids)
		return MM_CID_UNSET;
	if (test_and_set_bit(cid, dom->cidmask))
		return MM_CID_UNSET;
	return cid;
}

static inline unsigned int cid_get(struct cid_domain *dom)
{
	unsigned int cid = __cid_get(dom, READ_ONCE(dom->max_cids));

	while (cid == MM_CID_UNSET) {
		cpu_relax();
		cid = __cid_get(dom, num_possible_cpus());
	}
	return cid;
}

static inline unsigned int cid_converge(struct cid_domain *dom, unsigned int orig_cid,
					unsigned int max_cids)
{
	unsigned int new_cid, cid = cpu_cid_to_cid(orig_cid);

	/* Is it in the optimal CID space? */
	if (likely(cid < max_cids))
		return orig_cid;

	/* Try to find one in the optimal space. Otherwise keep the provided. */
	new_cid = __cid_get(dom, max_cids);
	if (new_cid != MM_CID_UNSET) {
		cid_drop(dom, cid);
		/* Preserve the ONCPU mode of the original CID */
		return new_cid | (orig_cid & MM_CID_ONCPU);
	}
	return orig_cid;
}

static __always_inline void cid_update_task(struct task_struct *t, struct cid_user *u,
					    unsigned int cid)
{
	if (u->cid != cid) {
		u->cid = cid;
		rseq_sched_set_ids_changed(t);
	}
}

static __always_inline void cid_update_pcpu(struct cid_domain *dom, unsigned int cid)
{
	__this_cpu_write(dom->pcpu->cid, cid);
}

static __always_inline void cid_from_cpu(struct cid_domain *dom, struct task_struct *t,
					 struct cid_user *u, unsigned int cpu_cid,
					 unsigned int mode)
{
	unsigned int max_cids, tcid = u->cid;

	max_cids = READ_ONCE(dom->max_cids);
	/* Optimize for the common case where both have the ONCPU bit set */
	if (likely(cid_on_cpu(cpu_cid & tcid))) {
		if (likely(cpu_cid_to_cid(cpu_cid) < max_cids)) {
			cid_update_task(t, u, cpu_cid);
			return;
		}
		/* Try to converge into the optimal CID space */
		cpu_cid = cid_converge(dom, cpu_cid, max_cids);
	} else {
		/* Hand over or drop the task owned CID */
		if (cid_on_task(tcid)) {
			if (cid_on_cpu(cpu_cid))
				cid_unset_on_task(dom, u);
			else
				cpu_cid = cid_to_cpu_cid(tcid);
		}
		/* Still nothing, allocate a new one */
		if (!cid_on_cpu(cpu_cid))
			cpu_cid = cid_to_cpu_cid(cid_get(dom));

		/* Handle the transition mode flag if required */
		if (mode & MM_CID_TRANSIT)
			cpu_cid = cpu_cid_to_cid(cpu_cid) | MM_CID_TRANSIT;
	}
	cid_update_pcpu(dom, cpu_cid);
	cid_update_task(t, u, cpu_cid);
}

static __always_inline void cid_from_task(struct cid_domain *dom, struct task_struct *t,
					  struct cid_user *u, unsigned int cpu_cid,
					  unsigned int mode)
{
	unsigned int max_cids, tcid = u->cid;

	max_cids = READ_ONCE(dom->max_cids);
	/* Optimize for the common case, where both have the ONCPU bit clear */
	if (likely(cid_on_task(tcid | cpu_cid))) {
		if (likely(tcid < max_cids)) {
			cid_update_pcpu(dom, tcid);
			return;
		}
		/* Try to converge into the optimal CID space */
		tcid = cid_converge(dom, tcid, max_cids);
	} else {
		/* Hand over or drop the CPU owned CID */
		if (cid_on_cpu(cpu_cid)) {
			if (cid_on_task(tcid))
				cid_drop_on_cpu(dom, this_cpu_ptr(dom->pcpu));
			else
				tcid = cpu_cid_to_cid(cpu_cid);
		}
		/* Still nothing, allocate a new one */
		if (!cid_on_task(tcid))
			tcid = cid_get(dom);
		/* Set the transition mode flag if required */
		tcid |= mode & MM_CID_TRANSIT;
	}
	cid_update_pcpu(dom, tcid);
	cid_update_task(t, u, tcid);
}

static __always_inline void __cid_schedin(struct cid_domain *dom, struct task_struct *next,
					  struct cid_user *u)
{
	unsigned int cpu_cid, mode;

	cpu_cid = __this_cpu_read(dom->pcpu->cid);
	mode = READ_ONCE(dom->mode);
	if (likely(!cid_on_cpu(mode)))
		cid_from_task(dom, next, u, cpu_cid, mode);
	else
		cid_from_cpu(dom, next, u, cpu_cid, mode);
}

static __always_inline void __cid_schedout(struct cid_domain *dom, struct task_struct *prev,
					   struct cid_user *u)
{
	unsigned int mode, cid;

	/* During mode transitions CIDs are temporary and need to be dropped */
	if (likely(!cid_in_transit(u->cid)))
		return;

	mode = READ_ONCE(dom->mode);
	cid = cid_from_transit_cid(u->cid);

	/*
	 * If transition mode is done, transfer ownership when the CID is
	 * within the convergence range to optimize the next schedule in.
	 */
	if (!cid_in_transit(mode) && cid < READ_ONCE(dom->max_cids)) {
		if (cid_on_cpu(mode))
			cid = cid_to_cpu_cid(cid);

		/* Update both so that the next schedule in goes into the fast path */
		cid_update_pcpu(dom, cid);
		u->cid = cid;
	} else {
		cid_drop(dom, cid);
		u->cid = MM_CID_UNSET;
	}
}

static __always_inline void mm_cid_schedin(struct task_struct *next)
{
	if (!next->mm_cid.active)
		return;
	__cid_schedin(&next->mm->mm_cid, next, &next->mm_cid);
}

static __always_inline void mm_cid_schedout(struct task_struct *prev)
{
	/* An inactive user holds no ID: nothing to drop, nothing in transit. */
	if (likely(prev->mm_cid.cid == MM_CID_UNSET))
		return;
	__cid_schedout(&prev->mm->mm_cid, prev, &prev->mm_cid);
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
