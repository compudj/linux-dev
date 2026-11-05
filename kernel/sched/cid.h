/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Inline helpers of the concurrency ID code for the context switch, and
 * the prototypes of cid.c. See cid.c for the design.
 */
#ifndef _KERNEL_SCHED_CID_H
#define _KERNEL_SCHED_CID_H

/* Included by sched.h. */

#ifdef CONFIG_SCHED_MM_CID
/*
 * The concurrency ID engine works on a cid domain. A domain is the mm of a
 * process (mm_cid) or the concurrency ID namespace of a task (ns_cid), see
 * Documentation/scheduler/sched-max-concurrency.rst. The users of a domain are
 * tasks. A domain hands out IDs below its bound to its users that run at the
 * same time. In per-task mode, with few users, each user owns its ID for its
 * lifetime. In per-CPU mode the CPU owns the ID while a user runs on it.
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

#ifdef CONFIG_CID_NS
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
#else /* !CONFIG_CID_NS */
static __always_inline struct cid_user *cid_task_user(struct cid_domain *dom, struct task_struct *t)
{
	return &t->mm_cid;
}

static inline struct task_struct *cid_user_task(struct cid_domain *dom, struct cid_user *u)
{
	return container_of(u, struct task_struct, mm_cid);
}

static __always_inline bool cid_task_in_domain(struct task_struct *t, struct cid_domain *dom)
{
	return t->mm == cid_domain_mm(dom) && t->mm_cid.active;
}
#endif /* !CONFIG_CID_NS */

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
 * The domains whose CID each CPU owns. They let the limit drop these CIDs
 * when a CPU leaves the used set of a group, see conc_slot_removed(). No
 * walk of the tasks of the group is needed. A record is on the list of its
 * CPU while its CID has the ONCPU bit.
 *
 * Every write of the CID of a record goes through cid_pcpu_write(), under
 * the runqueue lock of pcp->cpu. The walkers of the list hold that lock
 * too. Some paths read the CID without the lock, to skip a CPU which owns
 * nothing.
 *
 * The walk of conc_slot_removed() holds no reference on the domains it
 * finds. The lock keeps a domain alive while its record is on the list.
 * cid_domain_exit() skips the lock for a record whose CID has no ONCPU
 * bit. So the store of the CID is the last access of a walker to the
 * domain and to the record. It is a release, and cid_domain_exit() reads
 * the CID with an acquire. The domain can then be freed at once.
 */
DECLARE_PER_CPU(struct list_head, cid_cpu_doms);
static __always_inline void cid_pcpu_write(struct cid_pcpu *pcp, unsigned int cid)
{
	bool had = cid_on_cpu(pcp->cid), has = cid_on_cpu(cid);

	if (has && !had)
		list_add(&pcp->node, per_cpu_ptr(&cid_cpu_doms, pcp->cpu));
	else if (had && !has)
		list_del_init(&pcp->node);
	/* The last access to the record, see above. */
	smp_store_release(&pcp->cid, cid);
}

#ifdef CONFIG_SCHED_MAX_CONCURRENCY
/* Return true if users wait for an ID below the bound. */
static __always_inline bool cid_waiters(struct cid_domain *dom)
{
	return READ_ONCE(dom->waiters);
}

/*
 * An ID went back to the pool by another way than a schedule out, which
 * gives its ID to a waiter. Make the waiters try again. Do it from the irq
 * work, which can be queued under any lock the callers hold.
 */
static __always_inline void cid_kick_waiters(struct cid_domain *dom)
{
	if (unlikely(cid_waiters(dom))) {
		/* Set before the queueing: the irq work then sees it. */
		WRITE_ONCE(dom->kick, 1);
		irq_work_queue(&dom->irq_work);
	}
}
#else /* !CONFIG_SCHED_MAX_CONCURRENCY */
static inline bool cid_waiters(struct cid_domain *dom) { return false; }
static inline void cid_kick_waiters(struct cid_domain *dom) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY */

static __always_inline void cid_drop_on_cpu(struct cid_domain *dom, struct cid_pcpu *pcp)
{
	/* Clear the ONCPU bit, but do not set UNSET in the per CPU storage */
	if (cid_on_cpu(pcp->cid)) {
		unsigned int cid = cpu_cid_to_cid(pcp->cid);

		cid_drop(dom, cid);
		cid_kick_waiters(dom);
		/* Last: @dom can be freed after this, see cid_pcpu_write(). */
		cid_pcpu_write(pcp, cid);
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

/*
 * Allocate an ID, below max_cids if possible. Under a concurrency bound,
 * return no ID at or above the bound.
 *
 * With CONFIG_SCHED_MAX_CONCURRENCY, return MM_CID_UNSET when no ID is
 * free. The caller then waits for one at its return to user space, see
 * cid_wait(). Do not retry here, with or without a bound. A freed ID can be
 * on its way to a waiter, see cid_give(). Its bit stays set until that
 * waiter runs, and the caller can hold the runqueue lock which the waiter
 * needs.
 *
 * Without the option there are no waiters and at most one ID per possible
 * CPU is in use. Retry until one is free.
 */
static inline unsigned int cid_get(struct cid_domain *dom)
{
	unsigned int bound = READ_ONCE(dom->bound);
	unsigned int cid = __cid_get(dom, READ_ONCE(dom->max_cids));

	if (cid != MM_CID_UNSET)
		return cid;
	if (bound)
		return __cid_get(dom, bound);
	if (IS_ENABLED(CONFIG_SCHED_MAX_CONCURRENCY))
		return __cid_get(dom, num_possible_cpus());
	while (cid == MM_CID_UNSET) {
		cpu_relax();
		cid = __cid_get(dom, num_possible_cpus());
	}
	return cid;
}

/* Return true if @cid is at or above the bound of the domain. */
static __always_inline bool cid_over(struct cid_domain *dom, unsigned int cid)
{
	unsigned int bound = READ_ONCE(dom->bound);

	return bound && cpu_cid_to_cid(cid) >= bound;
}

#ifdef CONFIG_SCHED_MAX_CONCURRENCY
/*
 * @t schedules in on this CPU and got no ID of the domain. Make it take
 * one at its return to user space, or wait there for one, see
 * sched_cid_resume(). Nothing of the domain is written here: @t owns no ID
 * and this CPU caches none.
 */
static __always_inline void cid_wait(struct task_struct *t)
{
	WRITE_ONCE(t->cid_wait, 1);
	set_tsk_thread_flag(t, TIF_NOTIFY_RESUME);
}
extern void cid_release_cpu(struct cid_domain *dom, struct cid_pcpu *pcp);
#else /* !CONFIG_SCHED_MAX_CONCURRENCY */
static inline void cid_wait(struct task_struct *t) { }
static inline void cid_release_cpu(struct cid_domain *dom, struct cid_pcpu *pcp) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY */

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
 * Under a concurrency bound, make @orig_cid converge below @max_cids now.
 * Use it for a CID handed over between a task and a CPU. Use it also for
 * the CID of a CPU when a user which has none finds it. The fast paths do
 * the same for the CIDs they keep. A bound is a hard limit: it must hold
 * now, not at a later schedule in. Without a bound, return @orig_cid: the
 * next schedule in makes it converge.
 */
static __always_inline unsigned int cid_converge_bounded(struct cid_domain *dom,
							 unsigned int orig_cid,
							 unsigned int max_cids)
{
	if (likely(!READ_ONCE(dom->bound)) || cpu_cid_to_cid(orig_cid) < max_cids)
		return orig_cid;
	return cid_converge(dom, orig_cid, max_cids);
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

/*
 * Drop @cid, an ID in hand which is at or above the bound. This CPU's
 * record can still claim an ID: @cid itself, or one which cid_converge()
 * replaced by @cid and dropped. Clear that claim without a second drop.
 */
static __always_inline void cid_drop_over(struct cid_domain *dom, unsigned int cid)
{
	struct cid_pcpu *pcp = this_cpu_ptr(dom->pcpu);

	if (cid_on_cpu(pcp->cid))
		cid_pcpu_write(pcp, cpu_cid_to_cid(pcp->cid));
	cid_drop(dom, cpu_cid_to_cid(cid_from_transit_cid(cid)));
	cid_kick_waiters(dom);
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
		if (unlikely(cid_over(dom, cpu_cid)))
			goto over;
	} else {
		/* Hand over or drop the task owned CID */
		if (cid_on_task(tcid)) {
			if (cid_on_cpu(cpu_cid))
				cid_unset_on_task(dom, u);
			else
				cpu_cid = cid_to_cpu_cid(tcid);
		}
		/* Still nothing, allocate a new one */
		if (!cid_on_cpu(cpu_cid)) {
			cpu_cid = cid_get(dom);
			if (unlikely(cpu_cid == MM_CID_UNSET))
				goto none;
			cpu_cid = cid_to_cpu_cid(cpu_cid);
		} else {
			cpu_cid = cid_converge_bounded(dom, cpu_cid, max_cids);
			if (unlikely(cid_over(dom, cpu_cid)))
				goto over;
		}

		/* Handle the transition mode flag if required */
		if (mode & MM_CID_TRANSIT)
			cpu_cid = cpu_cid_to_cid(cpu_cid) | MM_CID_TRANSIT;
	}
	cid_update_pcpu(dom, cpu_cid);
	cid_update_task(t, u, cpu_cid);
	return;
over:
	/*
	 * The ID is at or above the bound. It is the one this CPU cached, or
	 * one the task handed over, which the record does not hold yet.
	 */
	cid_drop_over(dom, cpu_cid);
none:
	cid_update_task(t, u, MM_CID_UNSET);
	cid_wait(t);
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
		if (unlikely(cid_over(dom, tcid)))
			goto over;
	} else {
		/* Hand over or drop the CPU owned CID */
		if (cid_on_cpu(cpu_cid)) {
			if (cid_on_task(tcid))
				cid_drop_on_cpu(dom, this_cpu_ptr(dom->pcpu));
			else
				tcid = cpu_cid_to_cid(cpu_cid);
		}
		/* Still nothing, allocate a new one */
		if (!cid_on_task(tcid)) {
			tcid = cid_get(dom);
			if (unlikely(tcid == MM_CID_UNSET))
				goto none;
		} else {
			tcid = cid_converge_bounded(dom, tcid, max_cids);
			if (unlikely(cid_over(dom, tcid)))
				goto over;
		}
		/* Set the transition mode flag if required */
		tcid |= mode & MM_CID_TRANSIT;
	}
	cid_update_pcpu(dom, tcid);
	cid_update_task(t, u, tcid);
	return;
over:
	/*
	 * The ID is at or above the bound. It is the one of the task, or one
	 * this CPU handed over, which its record still claims.
	 */
	cid_drop_over(dom, tcid);
none:
	cid_update_task(t, u, MM_CID_UNSET);
	cid_wait(t);
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
		/*
		 * A user waits for an ID: the one this CPU owns goes to it
		 * rather than staying cached here.
		 */
		if (unlikely(cid_waiters(dom)) && cid_on_cpu(u->cid)) {
			cid_release_cpu(dom, this_cpu_ptr(dom->pcpu));
			return;
		}
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
		/* The ID is CPU owned now: same check as above. */
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
	/* The task holds no ID. This covers a task without an mm. */
	if (likely(prev->mm_cid.cid == MM_CID_UNSET))
		return;
	__cid_schedout(&prev->mm->mm_cid, prev, &prev->mm_cid);
	conc_debug_cid_kept(&prev->mm->mm_cid, prev);
}

#ifdef CONFIG_CID_NS
/*
 * Enabled when the first concurrency ID namespace is created. Until then
 * the hooks below return at once.
 */
DECLARE_STATIC_KEY_FALSE(cid_ns_used);

/* Update the ID of @next in its namespace at its schedule in. */
static __always_inline void ns_cid_schedin(struct task_struct *next)
{
	struct cid_domain *dom;

	if (!static_branch_unlikely(&cid_ns_used))
		return;
	dom = READ_ONCE(next->ns_dom);
	if (likely(!dom) || !next->ns_cid.active || conc_task_unplaced(next))
		return;
	__cid_schedin(dom, next, &next->ns_cid);
}

static __always_inline void ns_cid_schedout(struct task_struct *prev)
{
	struct cid_domain *dom;

	if (!static_branch_unlikely(&cid_ns_used))
		return;
	dom = READ_ONCE(prev->ns_dom);
	if (likely(!dom) || prev->ns_cid.cid == MM_CID_UNSET)
		return;
	__cid_schedout(dom, prev, &prev->ns_cid);
	conc_debug_cid_kept(dom, prev);
}
#else /* !CONFIG_CID_NS */
static inline void ns_cid_schedin(struct task_struct *next) { }
static inline void ns_cid_schedout(struct task_struct *prev) { }
#endif /* !CONFIG_CID_NS */

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
