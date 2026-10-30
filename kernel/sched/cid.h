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
 * process (mm_cid) or the IPC namespace of a task (ns_cid), see
 * Documentation/scheduler/sched-max-concurrency.rst. A domain hands out
 * IDs below its bound to the tasks of its users that run at the same
 * time, either owned by the task for its lifetime (per-task mode, few
 * users) or owned by the CPU while a user runs on it (per-CPU mode).
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

/* The mm a domain of the mm kind is embedded in. */
static inline struct mm_struct *cid_domain_mm(struct cid_domain *dom)
{
	return container_of(dom, struct mm_struct, mm_cid);
}

/* The per-task user state of @t in @dom. */
static __always_inline struct cid_user *cid_task_user(struct cid_domain *dom, struct task_struct *t)
{
	return dom->kind == CID_DOMAIN_MM ? &t->mm_cid : &t->ns_cid;
}

/* The task a user state belongs to. */
static inline struct task_struct *cid_user_task(struct cid_domain *dom, struct cid_user *u)
{
	if (dom->kind == CID_DOMAIN_MM)
		return container_of(u, struct task_struct, mm_cid);
	return container_of(u, struct task_struct, ns_cid);
}

/* Is @t an active user of @dom? */
static __always_inline bool cid_task_in_domain(struct task_struct *t, struct cid_domain *dom)
{
	if (dom->kind == CID_DOMAIN_MM)
		return t->mm == cid_domain_mm(dom) && t->mm_cid.active;
	return t->ns_dom == dom && t->ns_cid.active;
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

/*
 * The domains whose CID each CPU owns, for the limit to drop them when a
 * CPU leaves a group's used set (conc_slot_removed()) without walking the
 * group's tasks. A record is on the list of its CPU while its CID has the
 * ONCPU bit; every write of the field goes through here, under the rq lock
 * of pcp->cpu, which the readers take too.
 */
DECLARE_PER_CPU(struct list_head, cid_cpu_doms);
static __always_inline void cid_pcpu_write(struct cid_pcpu *pcp, unsigned int cid)
{
	bool had = cid_on_cpu(pcp->cid), has = cid_on_cpu(cid);

	if (has && !had)
		list_add(&pcp->node, per_cpu_ptr(&cid_cpu_doms, pcp->cpu));
	else if (had && !has)
		list_del_init(&pcp->node);
	pcp->cid = cid;
}
static __always_inline void cid_drop_on_cpu(struct cid_domain *dom, struct cid_pcpu *pcp)
{
	/* Clear the ONCPU bit, but do not set UNSET in the per CPU storage */
	if (cid_on_cpu(pcp->cid)) {
		cid_pcpu_write(pcp, cpu_cid_to_cid(pcp->cid));
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

#ifdef CONFIG_SCHED_MAX_CONCURRENCY
extern void conc_cid_violation(struct cid_domain *dom, unsigned int cid);
#else /* !CONFIG_SCHED_MAX_CONCURRENCY */
static inline void conc_cid_violation(struct cid_domain *dom, unsigned int cid) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY */
static inline unsigned int cid_get(struct cid_domain *dom)
{
	unsigned int cid = __cid_get(dom, READ_ONCE(dom->max_cids));

	while (cid == MM_CID_UNSET) {
		cpu_relax();
		cid = __cid_get(dom, num_possible_cpus());
	}
	/* The concurrency bound must never be exceeded, see sched-max-concurrency.rst. */
	if (unlikely(READ_ONCE(dom->bound) && cid >= READ_ONCE(dom->bound)))
		conc_cid_violation(dom, cid);
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

/*
 * Under a concurrency bound, a CID handed over between task and CPU, or
 * the one a CPU owns found by a user with none, converges below max_cids
 * at once, as the fast paths make the ones they keep converge: a bound is
 * no target to reach at a later schedule in, see sched-max-concurrency.rst.
 * Without one, the next schedule in converges it, as it always did.
 */
static __always_inline unsigned int cid_converge_bounded(struct cid_domain *dom,
							 unsigned int orig_cid,
							 unsigned int max_cids)
{
	unsigned int bound = READ_ONCE(dom->bound), cid;

	if (likely(!bound) || cpu_cid_to_cid(orig_cid) < max_cids)
		return orig_cid;
	cid = cid_converge(dom, orig_cid, max_cids);
	if (unlikely(cpu_cid_to_cid(cid) >= bound))
		conc_cid_violation(dom, cpu_cid_to_cid(cid));
	return cid;
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
	cid_pcpu_write(this_cpu_ptr(dom->pcpu), cid);
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
		if (unlikely(READ_ONCE(dom->bound) && cpu_cid_to_cid(cpu_cid) >= READ_ONCE(dom->bound)))
			conc_cid_violation(dom, cpu_cid_to_cid(cpu_cid));
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
		else
			cpu_cid = cid_converge_bounded(dom, cpu_cid, max_cids);

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
		if (unlikely(READ_ONCE(dom->bound) && tcid >= READ_ONCE(dom->bound)))
			conc_cid_violation(dom, tcid);
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
		else
			tcid = cid_converge_bounded(dom, tcid, max_cids);
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

#ifdef CONFIG_SCHED_MAX_CONCURRENCY_DEBUG
extern void conc_debug_cid_kept(struct cid_domain *dom, struct task_struct *prev);
#else /* !CONFIG_SCHED_MAX_CONCURRENCY_DEBUG */
static inline void conc_debug_cid_kept(struct cid_domain *dom, struct task_struct *prev) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY_DEBUG */
#ifdef CONFIG_SCHED_MAX_CONCURRENCY
extern void conc_cid_left_cpu(struct cid_domain *dom, struct task_struct *prev);
/* A CPU-owned ID does not outlive the group's presence on the CPU. */
static __always_inline void conc_cid_schedout(struct cid_domain *dom, struct task_struct *prev,
					    struct cid_user *u)
{
	if (static_branch_unlikely(&sched_conc_enabled) && cid_on_cpu(u->cid))
		conc_cid_left_cpu(dom, prev);
}
#else /* !CONFIG_SCHED_MAX_CONCURRENCY */
static __always_inline void conc_cid_schedout(struct cid_domain *dom, struct task_struct *prev,
					    struct cid_user *u) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY */
static __always_inline void __cid_schedout(struct cid_domain *dom, struct task_struct *prev,
					   struct cid_user *u)
{
	unsigned int mode, cid;

	/* During mode transitions CIDs are temporary and need to be dropped */
	if (likely(!cid_in_transit(u->cid))) {
		conc_cid_schedout(dom, prev, u);
		return;
	}

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
		/* Now CPU-owned: not past the group's presence here, as above. */
		conc_cid_schedout(dom, prev, u);
	} else {
		cid_drop(dom, cid);
		u->cid = MM_CID_UNSET;
	}
}

static __always_inline void mm_cid_schedin(struct task_struct *next)
{
	if (!next->mm_cid.active || conc_task_unplaced(next))
		return;
	__cid_schedin(&next->mm->mm_cid, next, &next->mm_cid);
}

static __always_inline void mm_cid_schedout(struct task_struct *prev)
{
	/* An inactive user holds no ID: nothing to drop, nothing in transit. */
	if (likely(prev->mm_cid.cid == MM_CID_UNSET))
		return;
	__cid_schedout(&prev->mm->mm_cid, prev, &prev->mm_cid);
	conc_debug_cid_kept(&prev->mm->mm_cid, prev);
}

/* The namespace domain, only for tasks whose domain is bounded (ns_dom set). */
static __always_inline void ns_cid_schedin(struct task_struct *next)
{
	struct cid_domain *dom = READ_ONCE(next->ns_dom);

	if (likely(!dom) || !next->ns_cid.active || conc_task_unplaced(next))
		return;
	__cid_schedin(dom, next, &next->ns_cid);
}

static __always_inline void ns_cid_schedout(struct task_struct *prev)
{
	struct cid_domain *dom = READ_ONCE(prev->ns_dom);

	if (likely(!dom) || prev->ns_cid.cid == MM_CID_UNSET)
		return;
	__cid_schedout(dom, prev, &prev->ns_cid);
	conc_debug_cid_kept(dom, prev);
}

static inline void mm_cid_switch_to(struct task_struct *prev, struct task_struct *next)
{
	mm_cid_schedout(prev);
	ns_cid_schedout(prev);
	mm_cid_schedin(next);
	ns_cid_schedin(next);
}

/* Called from outside cid.c. */
void __init cid_init_cpus(void);
void cid_update_cpus_allowed(struct cid_domain *dom, const struct cpumask *affmsk);
void mm_update_cpus_allowed(struct mm_struct *mm, const struct cpumask *affmsk);

void sched_cid_fork(struct task_struct *t);
void cid_apply_bound(struct cid_domain *dom);

#else /* !CONFIG_SCHED_MM_CID */
static inline void cid_init_cpus(void) { }
static inline void mm_cid_switch_to(struct task_struct *prev, struct task_struct *next) { }
static inline void cid_update_cpus_allowed(struct cid_domain *dom, const struct cpumask *affmsk) { }
static inline void mm_update_cpus_allowed(struct mm_struct *mm, const struct cpumask *affmsk) { }

static inline void sched_cid_fork(struct task_struct *t) { }
#endif /* !CONFIG_SCHED_MM_CID */

#endif /* _KERNEL_SCHED_CID_H */
