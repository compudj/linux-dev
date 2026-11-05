// SPDX-License-Identifier: GPL-2.0
/*
 * The concurrency ID engine. The context-switch hooks are inline in cid.h.
 */

#include <linux/sched/isolation.h>
#include <linux/sched/clock.h>
#include <linux/sched/mm.h>
#include <linux/cpuset.h>
#include <linux/stop_machine.h>
#include <linux/cid_namespace.h>
#include <linux/nsproxy.h>

#include "sched.h"
#include "smp.h"

/*
 * Globals and forward declarations
 */

#ifdef CONFIG_CID_NS
static void sched_cid_leave_ns(struct task_struct *t);
#else
static inline void sched_cid_leave_ns(struct task_struct *t) { }
#endif

/*
 * Concurrency IDentifier management
 *
 * Serialization rules:
 *
 * mm::mm_cid::mutex:	Serializes fork() and exit() and therefore
 *			protects mm::mm_cid::users and mode switch
 *			transitions
 *
 * mm::mm_cid::lock:	Serializes mm_update_max_cids() and
 *			mm_update_cpus_allowed(). Nests in mm_cid::mutex
 *			and runqueue lock.
 *
 * The mm_cidmask bitmap is not protected by any of the mm::mm_cid locks
 * and can only be modified with atomic operations.
 *
 * The mm::mm_cid:pcpu per CPU storage is protected by the CPUs runqueue
 * lock.
 *
 * CID ownership:
 *
 * A CID is either owned by a task (stored in task_struct::mm_cid.cid) or
 * by a CPU (stored in mm::mm_cid.pcpu::cid). CIDs owned by CPUs have the
 * MM_CID_ONCPU bit set.
 *
 * During the transition of ownership mode, the MM_CID_TRANSIT bit is set
 * on the CIDs. When this bit is set the tasks drop the CID back into the
 * pool when scheduling out.
 *
 * Both bits (ONCPU and TRANSIT) are filtered out by task_cid() when the
 * CID is actually handed over to user space in the RSEQ memory.
 *
 * Mode switching:
 *
 * The ownership mode is per process and stored in mm:mm_cid::mode with the
 * following possible states:
 *
 *	0:				Per task ownership
 *	0 | MM_CID_TRANSIT:		Transition from per CPU to per task
 *	MM_CID_ONCPU:			Per CPU ownership
 *	MM_CID_ONCPU | MM_CID_TRANSIT:	Transition from per task to per CPU
 *
 * All transitions of ownership mode happen in two phases:
 *
 *  1) mm:mm_cid::mode has the MM_CID_TRANSIT bit set. This is OR'ed on the
 *     CIDs and denotes that the CID is only temporarily owned by a
 *     task. When the task schedules out it drops the CID back into the
 *     pool if this bit is set.
 *
 *  2) The initiating context walks the per CPU space or the tasks to fixup
 *     or drop the CIDs and after completion it clears MM_CID_TRANSIT in
 *     mm:mm_cid::mode. After that point the CIDs are strictly task or CPU
 *     owned again.
 *
 * This two phase transition is required to prevent CID space exhaustion
 * during the transition as a direct transfer of ownership would fail:
 *
 *   - On task to CPU mode switch if a task is scheduled in on one CPU and
 *     then migrated to another CPU before the fixup freed enough per task
 *     CIDs.
 *
 *   - On CPU to task mode switch if two tasks are scheduled in on the same
 *     CPU before the fixup freed per CPU CIDs.
 *
 *   Both scenarios can result in a live lock because sched_in() is invoked
 *   with runqueue lock held and loops in search of a CID and the fixup
 *   thread can't make progress freeing them up because it is stuck on the
 *   same runqueue lock.
 *
 * While MM_CID_TRANSIT is active during the transition phase the MM_CID
 * bitmap can be contended, but that's a temporary contention bound to the
 * transition period. After that everything goes back into steady state and
 * nothing except fork() and exit() will touch the bitmap. This is an
 * acceptable tradeoff as it completely avoids complex serialization,
 * memory barriers and atomic operations for the common case.
 *
 * Aside of that this mechanism also ensures RT compability:
 *
 *   - The task which runs the fixup is fully preemptible except for the
 *     short runqueue lock held sections.
 *
 *   - The transient impact of the bitmap contention is only problematic
 *     when there is a thundering herd scenario of tasks scheduling in and
 *     out concurrently. There is not much which can be done about that
 *     except for avoiding mode switching by a proper overall system
 *     configuration.
 *
 * Switching to per CPU mode happens when the user count becomes greater
 * than the maximum number of CIDs, which is calculated by:
 *
 *	opt_cids = min(mm_cid::nr_cpus_allowed, mm_cid::users);
 *	max_cids = min(1.25 * opt_cids, num_possible_cpus());
 *
 * The +25% allowance is useful for tight CPU masks in scenarios where only
 * a few threads are created and destroyed to avoid frequent mode
 * switches. Though this allowance shrinks, the closer opt_cids becomes to
 * num_possible_cpus(), which is the (unfortunate) hard ABI limit.
 *
 * At the point of switching to per CPU mode the new user is not yet
 * visible in the system, so the task which initiated the fork() runs the
 * fixup function. mm_cid_fixup_tasks_to_cpu() walks the thread list and
 * either marks each task owned CID with MM_CID_TRANSIT if the task is
 * running on a CPU or drops it into the CID pool if a task is not on a
 * CPU. Tasks which schedule in before the task walk reaches them do the
 * handover in mm_cid_schedin(). When mm_cid_fixup_tasks_to_cpus()
 * completes it is guaranteed that no task related to that MM owns a CID
 * anymore.
 *
 * Switching back to task mode happens when the user count goes below the
 * threshold which was recorded on the per CPU mode switch:
 *
 *	pcpu_thrs = min(opt_cids - (opt_cids / 4), num_possible_cpus() / 2);
 *
 * This threshold is updated when a affinity change increases the number of
 * allowed CPUs for the MM, which might cause a switch back to per task
 * mode.
 *
 * If the switch back was initiated by a exiting task, then that task runs
 * the fixup function. If it was initiated by a affinity change, then it's
 * run either in the deferred update function in context of a workqueue or
 * by a task which forks a new one or by a task which exits. Whatever
 * happens first. mm_cid_fixup_cpus_to_task() walks through the possible
 * CPUs and either marks the CPU owned CIDs with MM_CID_TRANSIT if a
 * related task is running on the CPU or drops it into the pool. Tasks
 * which are scheduled in before the fixup covered them do the handover
 * themself. When mm_cid_fixup_cpus_to_tasks() completes it is guaranteed
 * that no CID related to that MM is owned by a CPU anymore.
 */

/*
 * Return the concurrency bound of @dom, or the number of possible CPUs if
 * it has none.
 */
static inline unsigned int cid_bound(struct cid_domain *dom)
{
	return dom->bound ?: num_possible_cpus();
}

/* Return max_cids for @dom under the concurrency bound @bound, 0 for none. */
static unsigned int cid_max_for(struct cid_domain *dom, unsigned int bound)
{
	unsigned int opt_cids;

	bound = bound ?: num_possible_cpus();
	/* Calculate the new optimal constraint */
	opt_cids = min3(dom->nr_cpus_allowed, dom->users, bound);

	/*
	 * Adjust the maximum CIDs to +25% limited by the number of possible
	 * CPUs. The concurrency bound caps the slack too: no ID may reach
	 * the bound.
	 */
	return min3(opt_cids + (opt_cids / 4), num_possible_cpus(), bound);
}

static void __cid_update_max(struct cid_domain *dom)
{
	WRITE_ONCE(dom->max_cids, cid_max_for(dom, dom->bound));
}

static inline unsigned int cid_calc_pcpu_thrs(struct cid_domain *dom)
{
	unsigned int nr_cpus = dom->nr_cpus_allowed;

	/* Has to be at least 1 because 0 indicates PCPU mode off */
	return max(min(nr_cpus - nr_cpus / 4, num_possible_cpus() / 2), 1);
}

static bool cid_update_max(struct cid_domain *dom)
{
	bool percpu = cid_on_cpu(dom->mode);

	lockdep_assert_held(&dom->lock);

	/* Clear deferred mode switch flag. A change is handled by the caller */
	dom->update_deferred = false;
	__cid_update_max(dom);

	/* Check whether owner mode must be changed */
	if (!percpu) {
		/* Enable per CPU mode when the number of users is above max_cids */
		if (dom->users > dom->max_cids)
			dom->pcpu_thrs = cid_calc_pcpu_thrs(dom);
	} else {
		/*
		 * Switch back to per task if user count under threshold. Per
		 * task mode needs an ID per user, which a concurrency bound
		 * below the user count cannot provide.
		 */
		if (dom->users < dom->pcpu_thrs && dom->users <= dom->max_cids)
			dom->pcpu_thrs = 0;
	}

	/* Mode change required? */
	if (percpu == !!dom->pcpu_thrs)
		return false;

	/* Flip the mode and set the transition flag to bridge the transfer */
	WRITE_ONCE(dom->mode, dom->mode ^ (MM_CID_TRANSIT | MM_CID_ONCPU));
	/*
	 * Order the store against the subsequent fixups so that
	 * acquire(rq::lock) cannot be reordered by the CPU before the
	 * store.
	 */
	smp_mb();
	return true;
}

static void __cid_update_cpus_allowed(struct cid_domain *dom, const struct cpumask *affmsk)
{
	unsigned int weight;

	lockdep_assert_held(&dom->lock);
	/*
	 * The domain's mask is the superset of each user's allowed CPUs
	 * mask which means it can only grow.
	 */
	weight = cpumask_weighted_or(dom->cpus_allowed, dom->cpus_allowed, affmsk);
	if (weight == dom->nr_cpus_allowed)
		return;

	WRITE_ONCE(dom->nr_cpus_allowed, weight);
	__cid_update_max(dom);
	if (!cid_on_cpu(dom->mode))
		return;

	/* Adjust the threshold to the wider set */
	dom->pcpu_thrs = cid_calc_pcpu_thrs(dom);
	/* Switch back to per task mode? */
	if (dom->users >= dom->pcpu_thrs)
		return;

	/* Don't queue twice */
	if (dom->update_deferred)
		return;

	/* Queue the irq work, which schedules the real work */
	dom->update_deferred = true;
	irq_work_queue(&dom->irq_work);
}

/* Add the new allowed CPUs of a user to @dom. The union only grows. */
void cid_update_cpus_allowed(struct cid_domain *dom, const struct cpumask *affmsk)
{
	/* The first user takes the allowed CPUs of its task when it joins */
	if (!dom || !READ_ONCE(dom->users))
		return;

	guard(raw_spinlock)(&dom->lock);
	/*
	 * The last user left meanwhile and synchronized the work. Do not
	 * queue the work again: the domain may be freed next.
	 */
	if (!dom->users)
		return;

	__cid_update_cpus_allowed(dom, affmsk);
}

void mm_update_cpus_allowed(struct mm_struct *mm, const struct cpumask *affmsk)
{
	if (mm)
		cid_update_cpus_allowed(&mm->mm_cid, affmsk);
}

static inline void cid_complete_transit(struct cid_domain *dom, unsigned int mode)
{
	/*
	 * Ensure that the store removing the TRANSIT bit cannot be
	 * reordered by the CPU before the fixups have been completed.
	 */
	smp_mb();
	WRITE_ONCE(dom->mode, mode);
}

/*
 * Transfer the ID which the CPU of @pcp owns to @u, the user which runs
 * there. Return false if @u does not use that ID.
 *
 * A user which runs with a hold pending took no ID at its schedule in, see
 * conc_task_unplaced(). Its record can still read a CPU-owned ID from an
 * earlier run. That value is stale: the ID belongs to the CPU where the
 * user ran then, or it was dropped.
 */
static inline bool cid_transit_to_task(struct cid_user *u, struct cid_pcpu *pcp)
{
	unsigned int cid;

	if (!cid_on_cpu(u->cid) || u->cid != pcp->cid)
		return false;
	cid = cpu_cid_to_cid(u->cid);
	u->cid = cid_to_transit_cid(cid);
	cid_pcpu_write(pcp, u->cid);
	return true;
}

static void cid_fixup_cpus_to_tasks(struct cid_domain *dom)
{
	unsigned int cpu;

	/* Walk the CPUs and fixup all stale CIDs */
	for_each_possible_cpu(cpu) {
		struct cid_pcpu *pcp = per_cpu_ptr(dom->pcpu, cpu);
		struct rq *rq = cpu_rq(cpu);
		struct cid_user *cu;

		/* Remote access to the per-CPU storage requires rq_lock */
		guard(rq_lock_irq)(rq);
		cu = cid_task_in_domain(rq->curr, dom) ? cid_task_user(dom, rq->curr) : NULL;
		/* Is the CID still owned by the CPU? */
		if (cid_on_cpu(pcp->cid)) {
			/*
			 * If rq->curr is a user of the domain and uses the
			 * CID, transfer it with the transition bit set.
			 * Otherwise drop it.
			 */
			if (!cu || !cid_transit_to_task(cu, pcp))
				cid_drop_on_cpu(dom, pcp);

		} else if (cu) {
			unsigned int cid = cu->cid;

			/*
			 * Set the transition bit only on a genuine task-owned
			 * CID. A running active task can legitimately have
			 * MM_CID_UNSET here: in per-CPU mode CIDs are assigned
			 * lazily on schedule-in, so the fork()/execve() window
			 * leaves the task active with no owned CID. Setting the
			 * transition bit on MM_CID_UNSET would later feed
			 * clear_bit() an out-of-bounds bit number via
			 * __cid_schedout(), so exclude it. Exclude a CPU-owned
			 * (MM_CID_ONCPU) CID too: the CPU does not own it, so
			 * it is stale, see cid_transit_to_task().
			 */
			if (cid_on_task(cid)) {
				cid = cid_to_transit_cid(cid);
				cu->cid = cid;
				cid_pcpu_write(pcp, cid);
			}
		}
	}
	cid_complete_transit(dom, 0);
}

static inline void cid_transit_to_cpu(struct cid_user *u, struct cid_pcpu *pcp)
{
	if (cid_on_task(u->cid)) {
		u->cid = cid_to_transit_cid(u->cid);
		cid_pcpu_write(pcp, u->cid);
	}
}

static void cid_fixup_task_to_cpu(struct cid_domain *dom, struct task_struct *t,
				  struct cid_user *u)
{
	/* Remote access to the per-CPU storage requires rq_lock */
	guard(task_rq_lock)(t);
	if (cid_on_task(u->cid)) {
		/* If running on the CPU, put the CID in transit mode, otherwise drop it */
		if (task_rq(t)->curr == t)
			cid_transit_to_cpu(u, per_cpu_ptr(dom->pcpu, task_cpu(t)));
		else
			cid_unset_on_task(dom, u);
	}
}

static void cid_fixup_tasks_to_cpus(struct cid_domain *dom)
{
	struct cid_user *u;

	lockdep_assert_held(&dom->mutex);

	hlist_for_each_entry(u, &dom->user_list, node) {
		struct task_struct *t = cid_user_task(dom, u);

		/* Current has already transferred before invoking the fixup. */
		if (t != current)
			cid_fixup_task_to_cpu(dom, t, u);
	}

	cid_complete_transit(dom, MM_CID_ONCPU);
}

/*
 * Add the allowed CPUs of @t to those of @dom. The members of a namespace
 * can have any affinity, so each join can widen the domain. For an mm
 * this changes nothing: a new thread has the mask of its creator.
 */
static void cid_join_cpus_allowed(struct cid_domain *dom, struct task_struct *t)
{
	unsigned int weight;

	lockdep_assert_held(&dom->lock);
	weight = cpumask_weighted_or(dom->cpus_allowed, dom->cpus_allowed, &t->cpus_mask);
	if (weight == dom->nr_cpus_allowed)
		return;
	WRITE_ONCE(dom->nr_cpus_allowed, weight);
	if (cid_on_cpu(dom->mode))
		dom->pcpu_thrs = cid_calc_pcpu_thrs(dom);
}
static bool cid_add_user(struct cid_domain *dom, struct cid_user *u)
{
	unsigned int bound;

	lockdep_assert_held(&dom->lock);

	u->active = 1;
	hlist_add_head(&u->node, &dom->user_list);
	dom->users++;
	/*
	 * Compute the bound with the new user linked. A bound can be a sum
	 * over the topmost limited groups of the users. It must count the
	 * group of the new user.
	 *
	 * The first user sets the bound. No ID is taken yet. A later user
	 * can raise the bound or remove it, never lower it. A lower value
	 * means that a decrease is in progress. Its path applies it after
	 * its configuration walk, through cid_apply_bound(). The callers
	 * are conc_bounds_apply() and the deferred update of a move or an
	 * exit. cid_apply_bound() switches the mode under the old bound,
	 * then drops the IDs above the new one. If the lower bound were set
	 * here, those IDs would stay, and cid_apply_bound() would find the
	 * bound unchanged and do nothing.
	 */
	bound = conc_dom_bound(dom);
	if (dom->users == 1 || (dom->bound && (!bound || bound > dom->bound)))
		dom->bound = bound;
	return cid_update_max(dom);
}

/*
 * Lock a section which may write this CPU's record of @dom, see
 * cid_pcpu_write(). Disable interrupts, take the runqueue lock of this
 * CPU, then dom::lock. Every writer of a record holds the runqueue lock of
 * its CPU. dom::lock nests inside the runqueue lock.
 */
DEFINE_LOCK_GUARD_1(cid_local, struct cid_domain,
		    local_irq_disable();
		    _T->rq = this_rq();
		    raw_spin_rq_lock(_T->rq);
		    raw_spin_lock(&_T->lock->lock),
		    raw_spin_unlock(&_T->lock->lock);
		    raw_spin_rq_unlock(_T->rq);
		    local_irq_enable(),
		    struct rq *rq)

#ifdef CONFIG_SCHED_MAX_CONCURRENCY
/*
 * A user which waits for an ID below the bound of the domain. It lives on
 * the stack of cid_resume(). It is on dom::waitq until it is given an ID
 * or told to try again. Then it is on dom::handoff until
 * cid_wake_handoffs() takes it off to wake it. Both lists are under
 * dom::lock. @cid is the ID it was given.
 */
struct cid_waiter {
	struct list_head	node;
	struct task_struct	*task;
	unsigned int		cid;
	bool			queued;
};

/*
 * Wake the waiters on dom::handoff: those which were given an ID, and
 * those told to try again. Wake each one outside dom::lock: a wakeup takes
 * runqueue locks, and dom::lock nests inside them. A waiter leaves its
 * node under dom::lock, so a waiter found on the list is still waiting.
 * The task reference keeps the task across the wakeup.
 */
static void cid_wake_handoffs(struct cid_domain *dom)
{
	for (;;) {
		struct task_struct *t;

		scoped_guard(raw_spinlock_irqsave, &dom->lock) {
			struct cid_waiter *w;

			w = list_first_entry_or_null(&dom->handoff, struct cid_waiter, node);
			if (!w)
				return;
			list_del_init(&w->node);
			t = w->task;
			get_task_struct(t);
		}
		wake_up_process(t);
		put_task_struct(t);
	}
}

/*
 * Give the freed @cid of a bounded domain to the user which has waited the
 * longest. Put it back in the pool when no user waits. An ID in the pool
 * goes to the first user which schedules in, and the waiters could starve.
 * Call under a runqueue lock, with interrupts off.
 */
static void cid_give(struct cid_domain *dom, unsigned int cid)
{
	struct cid_waiter *w;

	guard(raw_spinlock)(&dom->lock);
	w = list_first_entry_or_null(&dom->waitq, struct cid_waiter, node);
	if (!w) {
		cid_drop(dom, cid);
		return;
	}
	list_move_tail(&w->node, &dom->handoff);
	w->cid = cid;
	w->queued = false;
	dom->waiters--;
	irq_work_queue(&dom->irq_work);
}

/*
 * Release the ID which the CPU of @pcp owns: give it to a waiter, or to
 * the pool. Called at a schedule out on that CPU and by cid_reclaim().
 * Call under the runqueue lock of that CPU.
 */
void cid_release_cpu(struct cid_domain *dom, struct cid_pcpu *pcp)
{
	unsigned int cid = cpu_cid_to_cid(pcp->cid);

	if (!cid_on_cpu(pcp->cid))
		return;
	cid_pcpu_write(pcp, cid);
	cid_give(dom, cid);
}

/*
 * Make every waiter try again. Call when an ID went back to the pool, a
 * user left or a bound was applied.
 */
static void cid_wake_waiters(struct cid_domain *dom)
{
	if (likely(!READ_ONCE(dom->waiters)))
		return;
	scoped_guard(raw_spinlock_irqsave, &dom->lock) {
		struct cid_waiter *w;

		list_for_each_entry(w, &dom->waitq, node)
			w->queued = false;
		list_splice_tail_init(&dom->waitq, &dom->handoff);
		dom->waiters = 0;
	}
	cid_wake_handoffs(dom);
}

/* Return true once for each cid_kick_waiters() which found waiters. */
static bool cid_kick_pending(struct cid_domain *dom)
{
	return READ_ONCE(dom->kick) && xchg(&dom->kick, 0);
}
#else /* !CONFIG_SCHED_MAX_CONCURRENCY */
static inline void cid_wake_handoffs(struct cid_domain *dom) { }
static inline void cid_wake_waiters(struct cid_domain *dom) { }
static inline bool cid_kick_pending(struct cid_domain *dom) { return false; }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY */

/*
 * The last user to leave does not handle the mode switch. For exit() that
 * would be wasted work. If the process that called execve() was
 * multithreaded, the last thread to leave may have switched back from per
 * CPU mode. The domain then keeps stale state, and cid_domain::mode has
 * MM_CID_TRANSIT set. Reset such a domain before its first user joins.
 *
 * The caller holds dom::mutex and the domain has no user. A stale per CPU
 * record can still be on the list of its CPU. Clear each record under the
 * runqueue lock of its CPU, see cid_pcpu_write(). Return true if the
 * domain had stale state: the caller then resets its allowed CPUs.
 *
 * dom::lock is not taken:
 *  - dom->mode. Every writer holds dom::mutex. A context switch reads it
 *    only for an active user or for a task which holds an ID. The domain
 *    has none. An affinity change returns before it reads it when the
 *    domain has no user, see cid_update_cpus_allowed().
 *  - The bitmap. dom::lock does not protect it. An ID is allocated only
 *    for a user. A CPU owned ID is dropped under the runqueue lock of its
 *    CPU. The loop clears every record under that lock before the bitmap
 *    is zeroed.
 */
static bool cid_cleanup_execve(struct cid_domain *dom)
{
	unsigned int cpu;

	lockdep_assert_held(&dom->mutex);
	if (likely(!dom->mode))
		return false;

	WARN_ON_ONCE(dom->mode & MM_CID_ONCPU);
	dom->mode = 0;

	for_each_possible_cpu(cpu) {
		guard(rq_lock_irq)(cpu_rq(cpu));
		cid_pcpu_write(per_cpu_ptr(dom->pcpu, cpu), 0);
	}

	bitmap_zero(dom->cidmask, num_possible_cpus());
	return true;
}

/*
 * Make @t the first user of @dom. The caller holds dom::mutex.
 *
 * Clean up stale state first, see cid_cleanup_execve(). It runs under
 * dom::mutex only. It takes the runqueue lock of each CPU in turn and
 * enables interrupts in between, so it must run before t->pi_lock is
 * taken.
 *
 * cid_domain_init() copied the allowed CPUs of a task before the domain
 * had a user. An affinity change since then did not reach the domain. Add
 * the allowed CPUs of @t again. Hold t->pi_lock, which every affinity
 * change takes. A concurrent change is in t->cpus_mask already, or it
 * runs later. It then sees dom->users set and updates the domain itself.
 *
 * Take the runqueue lock of this CPU inside t->pi_lock, and dom::lock
 * inside the runqueue lock. The runqueue lock covers the write of this
 * CPU's record, see cid_local.
 */
static void cid_add_first_user(struct cid_domain *dom, struct task_struct *t, struct cid_user *u)
{
	bool stale = cid_cleanup_execve(dom);
	struct rq *rq;

	guard(raw_spinlock_irq)(&t->pi_lock);
	rq = this_rq();
	raw_spin_rq_lock(rq);
	scoped_guard(raw_spinlock, &dom->lock) {
		if (stale) {
			dom->nr_cpus_allowed = t->nr_cpus_allowed;
			cpumask_copy(dom->cpus_allowed, &t->cpus_mask);
		}
		/* The first user sets the ancestor, see conc_dom_fork(). */
		conc_dom_fork(dom, t);
		cid_add_user(dom, u);
		__cid_update_cpus_allowed(dom, &t->cpus_mask);
		u->cid = cid_get(dom);
		/* Required for execve(). Without an ID, the schedule in takes one. */
		if (u->cid != MM_CID_UNSET)
			cid_pcpu_write(this_cpu_ptr(dom->pcpu), u->cid);
	}
	raw_spin_rq_unlock(rq);
}

/*
 * Make @t a user of @dom. The first user is handled by
 * cid_add_first_user().
 *
 * For an mm, the callers are fork(2) and execve(2). For a namespace, they
 * are fork(2) and the commit of a namespace switch. That commit is
 * setns(2), or execve(2) after unshare(2). unshare(2) itself joins
 * nothing: it only sets the namespace of the children.
 *
 * In per-task mode a joining user takes its ID here. In per CPU mode it
 * takes no ID of its own: it borrows the ID of its CPU when it is
 * scheduled in. A task which joins while it runs returns to user space
 * before any schedule in. So it takes its ID here, as a schedule in would.
 *
 * These callers join while they run:
 *  - setns(2);
 *  - execve(2) into a namespace which has members already;
 *  - the failure path of execve(2), which rejoins an mm that other
 *    threads still use.
 * A fork(2) child does not run yet.
 */
static void cid_user_join(struct cid_domain *dom, struct task_struct *t, struct cid_user *u)
{
	bool percpu = false, flip = false;

	WARN_ON_ONCE(u->cid != MM_CID_UNSET);

	guard(mutex)(&dom->mutex);
	/* The mutex keeps other users from joining or leaving */
	if (!dom->users) {
		cid_add_first_user(dom, t, u);
		return;
	}

	scoped_guard(cid_local, dom) {
		struct cid_pcpu *pcp = this_cpu_ptr(dom->pcpu);

		/* The new user's mask and group may widen the CPUs and the bound. */
		cid_join_cpus_allowed(dom, t);
		conc_dom_fork(dom, t);

		flip = cid_add_user(dom, u);
		/*
		 * A running joiner takes no CID at a schedule in before the
		 * fixups are done. The users it joins can hold every CID
		 * until then, and the fixups are its own work.
		 */
		if (t == current)
			u->active = 0;

		if (!flip) {
			if (!cid_on_cpu(dom->mode))
				u->cid = cid_get(dom);
			break;
		}

		/*
		 * Handle the mode change and transfer current's CID. A
		 * running joiner has no CID yet. It takes one below.
		 */
		percpu = cid_on_cpu(dom->mode);
		if (!percpu)
			cid_transit_to_task(cid_task_user(dom, current), pcp);
		else
			cid_transit_to_cpu(cid_task_user(dom, current), pcp);
	}

	if (flip) {
		if (percpu) {
			cid_fixup_tasks_to_cpus(dom);
		} else {
			cid_fixup_cpus_to_tasks(dom);
			/* A running joiner takes its CID below. */
			if (t != current)
				u->cid = cid_get(dom);
		}
		/* The fixup put IDs back in the pool. */
		cid_wake_waiters(dom);
	}

	/*
	 * The running joiner is active from here on, see above. It takes its
	 * CID now. A task with a pending hold takes none, as at a schedule
	 * in. It gets its CID once it is placed.
	 */
	if (t == current) {
		guard(task_rq_lock)(t);
		u->active = 1;
		if (u->cid == MM_CID_UNSET && !conc_task_unplaced(t))
			__cid_schedin(dom, t, u);
	}
}

static bool cid_remove_user(struct cid_domain *dom, struct cid_user *u)
{
	lockdep_assert_held(&dom->lock);

	u->active = 0;
	/* Clear the transition bit */
	u->cid = cid_from_transit_cid(u->cid);
	cid_unset_on_task(dom, u);
	hlist_del_init(&u->node);
	dom->users--;
	return cid_update_max(dom);
}

static bool __cid_user_leave(struct cid_domain *dom, struct task_struct *t, struct cid_user *u)
{
	if (!cid_remove_user(dom, u))
		return false;
	/*
	 * Contrary to a join this only deals with a switch back to per
	 * task mode either because the above decreased users or an
	 * affinity change increased the number of allowed CPUs and the
	 * deferred fixup did not run yet.
	 */
	if (WARN_ON_ONCE(cid_on_cpu(dom->mode)))
		return false;
	/*
	 * A failed fork(2) cleanup never gets here. So @current is @t, or
	 * another active user of the domain. cid_remove_user() cleared the
	 * active flag of @t, hence the separate test for @t.
	 */
	if (WARN_ON_ONCE(!cid_task_in_domain(current, dom) && current != t))
		return false;
	return true;
}

/*
 * Remove @t from the users of @dom. @t does not return to user space as
 * a user of @dom, so it needs no CID from it.
 */
static void cid_user_leave(struct cid_domain *dom, struct task_struct *t, struct cid_user *u)
{
	if (!u->active)
		return;
	/*
	 * Ensure that only one instance is doing CID operations within a
	 * domain. The common case is uncontended. The rare fixup case adds
	 * some overhead.
	 */
	scoped_guard(mutex, &dom->mutex) {
		/* The mutex is sufficient to protect dom::users */
		if (likely(dom->users > 1)) {
			bool flip;

			scoped_guard(cid_local, dom) {
				flip = __cid_user_leave(dom, t, u);
				conc_dom_user_exited(dom, t);
				/*
				 * Mode change. The task has the CID unset
				 * already and dealt with an eventually set
				 * TRANSIT bit. If the CID is owned by the CPU
				 * then drop it.
				 */
				if (flip)
					cid_drop_on_cpu(dom, this_cpu_ptr(dom->pcpu));
			}
			if (flip)
				cid_fixup_cpus_to_tasks(dom);
			cid_wake_waiters(dom);
			return;
		}
		/* Last user */
		scoped_guard(cid_local, dom) {
			/* Required across execve() */
			if (t == current)
				cid_transit_to_task(u, this_cpu_ptr(dom->pcpu));
			/* Ignore mode change. There is nothing to do. */
			cid_remove_user(dom, u);
		}
	}

	/*
	 * As this is the last user (execve(), process exit or failed
	 * fork(2)) there is no concurrency anymore.
	 *
	 * Synchronize eventually pending work to ensure that there are no
	 * dangling references left. dom::users is zero so nothing can queue
	 * this work anymore.
	 */
	irq_work_sync(&dom->irq_work);
	cancel_work_sync(&dom->work);
}

/*
 * A user worker (io_uring, vhost) never returns to user space and has no
 * rseq area. It needs no CID.
 */
static void sched_mm_cid_fork(struct task_struct *t)
{
	if (t->mm && !(t->flags & PF_USER_WORKER))
		cid_user_join(&t->mm->mm_cid, t, &t->mm_cid);
}

static void sched_mm_cid_leave(struct task_struct *t)
{
	if (t->mm)
		cid_user_leave(&t->mm->mm_cid, t, &t->mm_cid);
}

/*
 * Leave the domain of the mm and the domain of the namespace at exit. A
 * failed fork(2) never joined them, see sched_post_fork().
 */
void sched_mm_cid_exit(struct task_struct *t)
{
	sched_mm_cid_leave(t);
	sched_cid_leave_ns(t);
}

/*
 * Leave the domain of the mm before execve(2). exec_task_namespaces()
 * switches the namespace.
 */
void sched_mm_cid_before_execve(struct task_struct *t)
{
	sched_mm_cid_leave(t);
}

/* Reactivate MM CID after execve() */
void sched_mm_cid_after_execve(struct task_struct *t)
{
	sched_mm_cid_fork(t);
}

/*
 * Pair with set_cpus_allowed_common()'s p->pi_lock critical section. Call
 * after publishing p->mm and before dropping the old MM reference.
 */
void sched_mm_cid_mm_barrier(struct task_struct *t)
{
	guard(raw_spinlock_irq)(&t->pi_lock);
}

/*
 * Switch @dom to per CPU mode: its users are about to outnumber its CIDs.
 * Transfer the CID of current, as cid_user_join() does. The fixup which
 * follows skips current. Call under dom::lock and under the runqueue lock
 * of this CPU. The runqueue lock covers the write of the record of this
 * CPU, see cid_local.
 */
static void cid_switch_to_cpus(struct cid_domain *dom)
{
	lockdep_assert_held(&dom->lock);
	dom->pcpu_thrs = cid_calc_pcpu_thrs(dom);
	WRITE_ONCE(dom->mode, dom->mode ^ (MM_CID_TRANSIT | MM_CID_ONCPU));
	/*
	 * Order the store before the runqueue locks of the fixups, as in
	 * cid_update_max().
	 */
	smp_mb();
	if (cid_task_in_domain(current, dom))
		cid_transit_to_cpu(cid_task_user(dom, current), this_cpu_ptr(dom->pcpu));
}

/*
 * Return true if the task which runs on @cpu is a user of @dom with an ID
 * at or above the bound. Flag it then: it takes a new ID at its return to
 * user space, see sched_cid_resume(). Do not change its ID from here. The
 * task can be reading it, on its way to user space.
 */
static bool cid_flag_over(struct cid_domain *dom, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct task_struct *t;
	unsigned int cid;

	guard(rq_lock_irq)(rq);
	t = rq->curr;
	if (!cid_task_in_domain(t, dom))
		return false;
	cid = cid_task_user(dom, t)->cid;
	if (cid == MM_CID_UNSET || !cid_over(dom, cid_from_transit_cid(cid)))
		return false;
	cid_wait(t);
	return true;
}

static void cid_kick_nop(void *unused)
{
}

/*
 * The bound of @dom was lowered. A user which schedules in takes an ID
 * below it. A user which keeps running does not schedule in: it would
 * keep its ID, and user space would keep reading it. Flag each of them
 * and interrupt its CPU. The interrupt returns to user space through
 * sched_cid_resume(), which takes a new ID. A task in the kernel gets
 * there at its own return.
 *
 * Wait for each interrupt. When this returns, no user runs user space
 * code with an ID at or above the bound.
 */
static void cid_kick_over(struct cid_domain *dom)
{
	int cpu;

	lockdep_assert_held(&dom->mutex);
	if (!READ_ONCE(dom->bound))
		return;
	for_each_online_cpu(cpu) {
		if (cid_flag_over(dom, cpu))
			smp_call_function_single(cpu, cid_kick_nop, NULL, 1);
	}
}

/*
 * Apply the concurrency bound of the cgroup limit. Recompute max_cids and
 * switch the owner mode if the user count crosses it. Then drop the CPU
 * owned CIDs which are above the bound, or cached on CPUs which the group
 * does not use.
 *
 * A decrease can leave fewer CIDs than users. Then switch to per CPU mode
 * first, under the old bound, where every user has a CID of its own. Under
 * the new bound the switch could run short. The fixup visits the users one
 * at a time. A user which is not running keeps its task owned CID until
 * the fixup reaches it. Meanwhile a running user drops its CID at schedule
 * out, as the transition requires. It could then find none free below the
 * new bound at its next schedule in.
 *
 * Then lower the bound. In per CPU mode a user which schedules in
 * converges below it, see cid_from_cpu(). A user which keeps running is
 * made to take a new ID, see cid_kick_over().
 */
void cid_apply_bound(struct cid_domain *dom)
{
	bool percpu, flip;

	lockdep_assert_held(&dom->mutex);
	/*
	 * First drop the IDs cached on CPUs which the group of the domain
	 * does not use. A cgroup migration leaves such IDs behind on the CPUs
	 * of the old group. Do it before the fixups below allocate IDs for
	 * the CPUs which the group uses.
	 */
	conc_dom_drop_stale_cids(dom);
	conc_dom_grow_tops(dom);
	scoped_guard(cid_local, dom) {
		unsigned int bound = conc_dom_bound(dom);

		flip = !cid_on_cpu(dom->mode) && dom->users > cid_max_for(dom, bound);
		if (flip)
			cid_switch_to_cpus(dom);
	}
	if (flip)
		cid_fixup_tasks_to_cpus(dom);
	scoped_guard(cid_local, dom) {
		dom->bound_update = 0;
		dom->bound = conc_dom_bound(dom);
		percpu = cid_on_cpu(dom->mode);
		flip = cid_update_max(dom);
		/* The bound moved meanwhile: current transfers here too. */
		if (flip && !percpu && cid_task_in_domain(current, dom))
			cid_transit_to_cpu(cid_task_user(dom, current), this_cpu_ptr(dom->pcpu));
	}
	if (flip) {
		if (percpu)
			cid_fixup_cpus_to_tasks(dom);
		else
			cid_fixup_tasks_to_cpus(dom);
	}
	conc_dom_drop_stale_cids(dom);
	cid_kick_over(dom);
	/*
	 * The bound was raised, or IDs went back to the pool. Make the
	 * waiters try again.
	 */
	cid_wake_waiters(dom);
}

static void __cid_work_fn(struct cid_domain *dom, bool bounds)
{
	guard(mutex)(&dom->mutex);
	/* Did the last user leave already? */
	if (!dom->users)
		return;

	/* A cgroup change or a limit write deferred a bound update. */
	if (bounds && READ_ONCE(dom->bound_update))
		cid_apply_bound(dom);

	scoped_guard(raw_spinlock_irq, &dom->lock) {
		/* Have a join or a leave handled it already? */
		if (!dom->update_deferred)
			return;
		/* This clears dom::update_deferred */
		if (!cid_update_max(dom))
			return;
		/* Affinity changes can only switch back to task mode */
		if (WARN_ON_ONCE(cid_on_cpu(dom->mode)))
			return;
	}
	cid_fixup_cpus_to_tasks(dom);
	cid_wake_waiters(dom);
}

static void cid_work_fn(struct work_struct *work)
{
	struct cid_domain *dom = container_of(work, struct cid_domain, work);
	bool bounds = READ_ONCE(dom->bound_update);

	/*
	 * A deferred bound update waits for the configuration walks, see
	 * conc_bounds_lock(). One set meanwhile requeues this work.
	 */
	if (bounds)
		conc_bounds_lock();
	__cid_work_fn(dom, bounds);
	if (bounds)
		conc_bounds_unlock();
}
static void cid_irq_work(struct irq_work *work)
{
	struct cid_domain *dom = container_of(work, struct cid_domain, irq_work);

	/*
	 * Wake the waiters which were given an ID. Make the others try again
	 * only if an ID went back to the pool, see cid_kick_waiters(). The
	 * work wakes them itself when it applies a bound or switches the
	 * mode.
	 */
	cid_wake_handoffs(dom);
	if (cid_kick_pending(dom))
		cid_wake_waiters(dom);
	/*
	 * Needs to be unconditional because dom::lock cannot be held when
	 * scheduling work as cid_update_cpus_allowed() nests inside
	 * rq::lock and schedule_work() might end up in wakeup...
	 */
	schedule_work(&dom->work);
}

DEFINE_PER_CPU(struct list_head, cid_cpu_doms);

void __init cid_init_cpus(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		INIT_LIST_HEAD(per_cpu_ptr(&cid_cpu_doms, cpu));
}

/*
 * The domain goes away. Take its records off the lists of the CPUs which
 * still own one of its CIDs.
 */
void cid_domain_exit(struct cid_domain *dom)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cid_pcpu *pcp = per_cpu_ptr(dom->pcpu, cpu);

		/*
		 * A record without the ONCPU bit is off the list, and no
		 * walker of the list uses it any more, see cid_pcpu_write().
		 */
		if (!cid_on_cpu(smp_load_acquire(&pcp->cid)))
			continue;
		guard(rq_lock_irq)(cpu_rq(cpu));
		list_del_init(&pcp->node);
	}
}

/*
 * Set up a domain. @pcpu, @cidmask and @cpus_allowed are storage which the
 * caller provides. For an mm it is the trailing storage of the mm. For a
 * namespace the caller allocates it. @p, when given, seeds the allowed
 * CPUs and the common ancestor group.
 */
void cid_domain_init(struct cid_domain *dom, unsigned int kind, struct cid_pcpu __percpu *pcpu,
		     unsigned long *cidmask, struct cpumask *cpus_allowed, struct task_struct *p)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cid_pcpu *pcp = per_cpu_ptr(pcpu, cpu);

		pcp->cpu = cpu;
		pcp->dom = dom;
		INIT_LIST_HEAD(&pcp->node);
	}
	dom->kind = kind;
	dom->pcpu = pcpu;
	dom->cidmask = cidmask;
	dom->cpus_allowed = cpus_allowed;
	dom->max_cids = 0;
	dom->mode = 0;
	dom->users = 0;
	dom->pcpu_thrs = 0;
	dom->update_deferred = 0;
	dom->bound = 0;
	dom->bound_update = 0;
#ifdef CONFIG_SCHED_MAX_CONCURRENCY
	dom->lca = NULL;
	dom->tops = NULL;
	dom->nr_tops = 0;
	dom->tops_wanted = 0;
	dom->bounds_walk = 0;
	INIT_LIST_HEAD(&dom->waitq);
	INIT_LIST_HEAD(&dom->handoff);
	dom->waiters = 0;
#endif
	raw_spin_lock_init(&dom->lock);
	mutex_init(&dom->mutex);
	dom->irq_work = IRQ_WORK_INIT_HARD(cid_irq_work);
	INIT_WORK(&dom->work, cid_work_fn);
	INIT_HLIST_HEAD(&dom->user_list);
	if (p) {
		cpumask_copy(cpus_allowed, &p->cpus_mask);
		dom->nr_cpus_allowed = p->nr_cpus_allowed;
	} else {
		cpumask_clear(cpus_allowed);
		dom->nr_cpus_allowed = 0;
	}
	bitmap_zero(cidmask, num_possible_cpus());
	conc_dom_init(dom, p);
}

void mm_init_cid(struct mm_struct *mm, struct task_struct *p)
{
	cid_domain_init(&mm->mm_cid, CID_DOMAIN_MM, mm->mm_cid.pcpu, mm_cidmask(mm),
			mm_cpus_allowed(mm), p);
}

#ifdef CONFIG_CID_NS
/*
 * The domain of a concurrency ID namespace, see
 * Documentation/scheduler/sched-max-concurrency.rst.
 *
 * The members are the tasks with an mm whose nsproxy names the namespace.
 * User workers are not members. A member holds a reference on the
 * namespace. It caches the domain in task_struct::ns_dom for the context
 * switch.
 *
 * The membership changes at fork(2), at the commit of a namespace switch
 * and at exit. A cgroup migration does not change it. The members stay in
 * the subtree of the home of the namespace, see conc_ns_home().
 *
 * The bound of the domain is the soft limit of the namespace, see
 * conc_ns_home() and conc_ns_feed(). No ID is handed out at or above it. A
 * member which finds none free waits for one at its return to user space,
 * see sched_cid_resume().
 */
DEFINE_STATIC_KEY_FALSE(cid_ns_used);

int cid_ns_domain_init(struct cid_namespace *ns)
{
	struct cid_pcpu __percpu *pcpu;
	struct cpumask *cpus;
	unsigned long *cidmask;

	/*
	 * A process in a user namespace can create these. Charge the memory
	 * to its cgroup.
	 */
	pcpu = alloc_percpu_gfp(struct cid_pcpu, GFP_KERNEL_ACCOUNT);
	cidmask = bitmap_zalloc(num_possible_cpus(), GFP_KERNEL_ACCOUNT);
	cpus = kzalloc(cpumask_size(), GFP_KERNEL_ACCOUNT);
	if (!pcpu || !cidmask || !cpus) {
		kfree(cpus);
		bitmap_free(cidmask);
		free_percpu(pcpu);
		return -ENOMEM;
	}
	cid_domain_init(&ns->dom, CID_DOMAIN_NS, pcpu, cidmask, cpus, NULL);
	ns->dom.bound = conc_dom_bound(&ns->dom);
	static_branch_enable(&cid_ns_used);
	return 0;
}

void cid_ns_domain_exit(struct cid_namespace *ns)
{
	struct cid_domain *dom = &ns->dom;

	WARN_ON_ONCE(dom->users);
	irq_work_sync(&dom->irq_work);
	cancel_work_sync(&dom->work);
	conc_dom_destroy(dom);
	cid_domain_exit(dom);
	free_percpu(dom->pcpu);
	bitmap_free(dom->cidmask);
	kfree(dom->cpus_allowed);
}

static inline struct cid_namespace *cid_domain_ns(struct cid_domain *dom)
{
	return container_of(dom, struct cid_namespace, dom);
}

/*
 * Make @t a member of @ns. Set t->ns_dom before the join. The leave clears
 * it after the user left. In between, the context switch sees an inactive
 * user and does nothing with it.
 */
static void sched_cid_join_ns(struct task_struct *t, struct cid_namespace *ns)
{
	get_cid_ns(ns);
	/*
	 * A namespace made by clone3(2) gets its home, its maximum and its
	 * soft limit here, from the cgroup of the child, before any ID is
	 * taken. One made by unshare(2) has them already.
	 */
	conc_ns_home(ns, t);
	task_lock(t);
	t->ns_dom = &ns->dom;
	task_unlock(t);
	cid_user_join(&ns->dom, t, &t->ns_cid);
}

static void sched_cid_leave_ns(struct task_struct *t)
{
	struct cid_domain *dom = t->ns_dom;

	if (!dom)
		return;
	cid_user_leave(dom, t, &t->ns_cid);
	task_lock(t);
	t->ns_dom = NULL;
	task_unlock(t);
	put_cid_ns(cid_domain_ns(dom));
}

/*
 * Return the namespace which @t is a member of under @nsproxy. Return NULL
 * for the initial namespace, for a task without an mm and for a user
 * worker.
 */
static struct cid_namespace *sched_cid_task_ns(struct task_struct *t, struct nsproxy *nsproxy)
{
	struct cid_namespace *ns;

	if (!nsproxy || !t->mm || (t->flags & PF_USER_WORKER))
		return NULL;
	ns = nsproxy->cid_ns;
	return ns == &init_cid_ns ? NULL : ns;
}

/*
 * Make the fork(2) child @t a user of the domain of its mm. Make it a
 * member of its namespace too, if it is in one.
 */
void sched_cid_fork(struct task_struct *t)
{
	struct cid_namespace *ns;

	sched_mm_cid_fork(t);
	ns = sched_cid_task_ns(t, t->nsproxy);
	if (ns && !t->ns_dom)
		sched_cid_join_ns(t, ns);
}

/*
 * Follow a switch of the namespaces of @t to @new. @new is NULL at exit.
 * Leave the domain of the old namespace and join the domain of the new
 * one. The callers which change the namespace are setns(2) and execve(2),
 * where @t is current, and exit. unshare(2) comes here too, but it does
 * not change the namespace of @t.
 */
void sched_cid_switch_ns(struct task_struct *t, struct nsproxy *new)
{
	struct cid_namespace *ns = sched_cid_task_ns(t, new);

	if ((ns ? &ns->dom : NULL) == t->ns_dom)
		return;
	sched_cid_leave_ns(t);
	if (ns)
		sched_cid_join_ns(t, ns);
	/*
	 * The task now reports an ID of another namespace. Update its rseq
	 * area before it returns to user space.
	 */
	if (t == current)
		rseq_force_update();
}

#else /* !CONFIG_CID_NS */
void sched_cid_fork(struct task_struct *t)
{
	sched_mm_cid_fork(t);
}
#endif /* !CONFIG_CID_NS */

#ifdef CONFIG_SCHED_MAX_CONCURRENCY
/*
 * Release the IDs of @dom which CPUs own while they run no user of it.
 * Such an ID is cached for the next user on that CPU. Give each one to the
 * first waiter, or put it back in the pool. Take the runqueue lock of each
 * CPU: its record is written under it.
 */
static void cid_reclaim(struct cid_domain *dom)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cid_pcpu *pcp = per_cpu_ptr(dom->pcpu, cpu);
		struct rq *rq = cpu_rq(cpu);

		/* Nothing cached: the lock would find nothing to give. */
		if (!cid_on_cpu(READ_ONCE(pcp->cid)))
			continue;
		guard(rq_lock_irq)(rq);
		if (!cid_on_cpu(pcp->cid))
			continue;
		if (cid_task_in_domain(rq->curr, dom) && task_on_rq_queued(rq->curr))
			continue;
		cid_release_cpu(dom, pcp);
	}
}

/*
 * Current is a user of @dom which got no ID at its schedule in, see
 * cid_wait(). Take the ID which @w was given, if any. Otherwise try to
 * take one as a schedule in would. Return true if current has an ID, or
 * needs none. A task with a pending hold takes none and gets its ID once
 * it is placed. An ID given to a task which cannot take it goes to the
 * next waiter, or to the pool.
 */
static bool cid_acquire(struct cid_domain *dom, struct task_struct *t, struct cid_user *u,
			struct cid_waiter *w)
{
	bool eligible;

	guard(task_rq_lock)(t);
	eligible = u->active && !conc_task_unplaced(t);
	if (w->cid != MM_CID_UNSET) {
		if (eligible && u->cid == MM_CID_UNSET)
			u->cid = w->cid;
		else
			cid_give(dom, w->cid);
		w->cid = MM_CID_UNSET;
	}
	if (!eligible)
		return true;
	/*
	 * A schedule in since the caller looked gave current an ID. It is
	 * owned by this CPU or is a transit ID. A schedule in never finds such
	 * an ID on the task: the schedule out before it converts it. So do
	 * not run one over it, it would allocate a second ID.
	 */
	if (u->cid != MM_CID_UNSET && !cid_on_task(u->cid))
		return true;
	/* Hand the given ID to this CPU as a schedule in would, or take one. */
	__cid_schedin(dom, t, u);
	return u->cid != MM_CID_UNSET;
}

/*
 * Current found no ID of @dom free below the bound at its schedule in.
 * Take one now, or wait for one.
 *
 * All users of the domain are in the cgroups which make up the bound, and
 * the placement keeps those cgroups within their effective limit. So a
 * shortage only lasts for a transition of the placement: a task counted
 * before it is held, or moved while it runs.
 *
 * Until cid_acquire() succeeds or a fatal signal is pending:
 *  1. Queue on dom::waitq.
 *  2. Give the IDs cached on CPUs which run no user to the waiters, in
 *     order, see cid_reclaim(). Current may get one.
 *  3. Try cid_acquire() again. An ID can have gone back to the pool
 *     between the first try and the queueing, when nobody waited.
 *  4. Sleep, killable, until cid_wake_handoffs() wakes current. That
 *     follows a schedule out which hands over its ID, see
 *     __cid_schedout(). It also follows cid_wake_waiters(), after an ID
 *     went back to the pool, a user left or a bound was applied.
 *
 * Queue before the reclaim. A schedule out which sees the waiter gives its
 * ID instead of caching it.
 */
static void cid_resume(struct cid_domain *dom, struct task_struct *t, struct cid_user *u)
{
	struct cid_waiter w = { .task = t, .cid = MM_CID_UNSET };
	bool got;

	INIT_LIST_HEAD(&w.node);
	for (;;) {
		got = cid_acquire(dom, t, u, &w);
		if (got || fatal_signal_pending(t))
			break;
		scoped_guard(raw_spinlock_irq, &dom->lock) {
			list_add_tail(&w.node, &dom->waitq);
			w.queued = true;
			dom->waiters++;
		}
		cid_reclaim(dom);
		/*
		 * Try again now that the waiter is queued. An ID which went
		 * back to the pool before that woke nobody.
		 */
		got = cid_acquire(dom, t, u, &w);
		while (!got) {
			set_current_state(TASK_KILLABLE);
			if (list_empty(&w.node) || fatal_signal_pending(t))
				break;
			schedule();
		}
		__set_current_state(TASK_RUNNING);
		scoped_guard(raw_spinlock_irq, &dom->lock) {
			/*
			 * Still on a list. Count the waiter down only if it
			 * is still on dom::waitq: cid_give() and
			 * cid_wake_waiters() did it for a waiter which they
			 * moved to dom::handoff.
			 */
			if (!list_empty(&w.node)) {
				list_del_init(&w.node);
				if (w.queued) {
					w.queued = false;
					dom->waiters--;
				}
			}
		}
		if (got)
			break;
	}
	/*
	 * An ID given to the waiter after its last cid_acquire() has no
	 * owner. Pass it on.
	 */
	if (w.cid != MM_CID_UNSET) {
		guard(irqsave)();
		cid_give(dom, w.cid);
	}
}

/*
 * Drop the ID of current in @dom if it is at or above the bound. The
 * bound was lowered while current ran, see cid_kick_over(). Do as a
 * schedule in which finds such an ID, see cid_from_cpu().
 */
static void cid_drop_lowered(struct cid_domain *dom, struct task_struct *t, struct cid_user *u)
{
	guard(task_rq_lock)(t);
	if (u->cid == MM_CID_UNSET || !cid_over(dom, cid_from_transit_cid(u->cid)))
		return;
	cid_drop_over(dom, u->cid);
	u->cid = MM_CID_UNSET;
}

/*
 * Give current the IDs it lacks. Called from resume_user_mode_work(),
 * after a pending hold. There are two cases. Current found no ID free
 * below the bound of a domain at its schedule in. Or the bound was
 * lowered below the ID of current while it ran: drop that ID first. The
 * domain is the one of its mm or the one of its namespace. Its ID of
 * that domain reads MM_CID_UNSET until it has one. A user of the ID in
 * the kernel tests for that value. User space never sees it: the task
 * returns with its IDs or goes to its exit.
 */
void sched_cid_resume(void)
{
	struct task_struct *t = current;

	/*
	 * Clear the flag once, before the domains are looked at. A schedule
	 * in which finds no ID from here on sets it again, with
	 * TIF_NOTIFY_RESUME, and this function runs once more. Clearing it
	 * after the wait for one domain would lose that for the other.
	 */
	WRITE_ONCE(t->cid_wait, 0);
	if (t->mm_cid.active) {
		cid_drop_lowered(&t->mm->mm_cid, t, &t->mm_cid);
		if (t->mm_cid.cid == MM_CID_UNSET)
			cid_resume(&t->mm->mm_cid, t, &t->mm_cid);
	}
#ifdef CONFIG_CID_NS
	if (t->ns_dom) {
		cid_drop_lowered(t->ns_dom, t, &t->ns_cid);
		if (t->ns_cid.cid == MM_CID_UNSET)
			cid_resume(t->ns_dom, t, &t->ns_cid);
	}
#endif
	rseq_force_update();
}
#endif /* CONFIG_SCHED_MAX_CONCURRENCY */
