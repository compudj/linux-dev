// SPDX-License-Identifier: GPL-2.0
/*
 * The concurrency IDs: the engine that hands the tasks of a domain IDs
 * below its bound, the mm domain (mm_cid) and the domain of a concurrency
 * ID namespace (ns_cid). The context-switch hooks are inline in cid.h.
 */

#include <linux/sched/isolation.h>
#include <linux/sched/clock.h>
#include <linux/sched/mm.h>
#include <linux/cpuset.h>
#include <linux/stop_machine.h>

#include "sched.h"
#include "smp.h"

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

/* The concurrency bound of the cgroup limit, or no bound at all. */
static inline unsigned int cid_bound(struct cid_domain *dom)
{
	return dom->bound ?: num_possible_cpus();
}

/* The maximum CIDs of @dom under the concurrency bound @bound (0: none). */
static unsigned int cid_max_for(struct cid_domain *dom, unsigned int bound)
{
	unsigned int opt_cids;

	bound = bound ?: num_possible_cpus();
	/* Calculate the new optimal constraint */
	opt_cids = min3(dom->nr_cpus_allowed, dom->users, bound);

	/*
	 * Adjust the maximum CIDs to +25% limited by the number of possible
	 * CPUs. The concurrency bound caps the slack too: user space sizes
	 * its data by the bound.
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

/* A user's allowed CPUs changed: the domain's union can only grow. */
void cid_update_cpus_allowed(struct cid_domain *dom, const struct cpumask *affmsk)
{
	unsigned int weight;

	if (!dom || !READ_ONCE(dom->users))
		return;
	guard(raw_spinlock)(&dom->lock);
	/*
	 * The last user left meanwhile and synchronized the work: queue none
	 * after it, the domain of a concurrency ID namespace may be freed next.
	 */
	if (!dom->users)
		return;
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

static inline void cid_transit_to_task(struct cid_user *u, struct cid_pcpu *pcp)
{
	if (cid_on_cpu(u->cid)) {
		unsigned int cid = cpu_cid_to_cid(u->cid);

		u->cid = cid_to_transit_cid(cid);
		cid_pcpu_write(pcp, u->cid);
	}
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
			 * If rq->curr is a user of the domain, transfer it with
			 * the transition bit set. Otherwise drop it.
			 */
			if (cu)
				cid_transit_to_task(cu, pcp);
			else
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
			 * __cid_schedout(), so exclude it. A CPU-owned
			 * (MM_CID_ONCPU) CID is handled by the cid_on_cpu()
			 * branch above and never reaches here.
			 */
			if (cid != MM_CID_UNSET && !cid_in_transit(cid)) {
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

static bool cid_add_user(struct cid_domain *dom, struct cid_user *u)
{
	unsigned int bound;

	lockdep_assert_held(&dom->lock);

	u->active = 1;
	hlist_add_head(&u->node, &dom->user_list);
	dom->users++;
	/*
	 * A bound summed over the topmost limited groups of the users counts
	 * the new user's: compute it with the user linked. The first user
	 * sets it, no ID taken yet; a later one raises it at once, but never
	 * lowers it: a lower one is a decrease on its way, which its cause
	 * applies once its configuration walk is over (conc_bounds_apply(), or
	 * the deferred update of a move or an exit), through cid_apply_bound()
	 * and its order: the mode switched under the old bound, then the IDs
	 * above the new one dropped. Applied here, it would leave those IDs
	 * where they are, and the decrease would find the bound in place and
	 * nothing to do.
	 */
	bound = conc_dom_bound(dom);
	if (dom->users == 1 || (dom->bound && (!bound || bound > dom->bound)))
		dom->bound = bound;
	return cid_update_max(dom);
}

/*
 * A section that may write this CPU's record of @dom (cid_pcpu_write()):
 * under its runqueue lock, which the other writers of the record hold,
 * taken before the domain's lock as sched_move_task() takes them.
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
 * A user waiting for an ID below the domain's bound, on the stack of its
 * sched_cid_resume(): on dom::waitq until it is given one, or kicked to
 * try again, then on dom::handoff until the irq work has woken it; both
 * under dom::lock. @cid is the ID it was given.
 */
struct cid_waiter {
	struct list_head	node;
	struct task_struct	*task;
	unsigned int		cid;
};

/*
 * Wake the waiters given an ID, or kicked, each outside the domain's
 * lock: a wake up takes runqueue locks, under which that lock nests. A
 * waiter leaves its node under the lock, so one found here is still
 * there, and the reference keeps its task across the wake up.
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
 * A freed @cid, under a bound: to the user that has waited for one the
 * longest, since the pool goes to whichever user schedules in first,
 * which can starve the waiters; back to the pool when none waits.
 * Interrupts are off: under a runqueue lock.
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
	dom->waiters--;
	irq_work_queue(&dom->irq_work);
}

/* The ID a CPU owns, at a schedule out there or a reclaim: under its runqueue lock. */
void cid_release_cpu(struct cid_domain *dom, struct cid_pcpu *pcp)
{
	unsigned int cid = cpu_cid_to_cid(pcp->cid);

	if (!cid_on_cpu(pcp->cid))
		return;
	cid_pcpu_write(pcp, cid);
	cid_give(dom, cid);
}

/* An ID was freed some other way, or the bound applied: the waiters try again. */
static void cid_wake_waiters(struct cid_domain *dom)
{
	if (likely(!READ_ONCE(dom->waiters)))
		return;
	scoped_guard(raw_spinlock_irqsave, &dom->lock) {
		list_splice_tail_init(&dom->waitq, &dom->handoff);
		dom->waiters = 0;
	}
	cid_wake_handoffs(dom);
}
#else /* !CONFIG_SCHED_MAX_CONCURRENCY */
static inline void cid_wake_handoffs(struct cid_domain *dom) { }
static inline void cid_wake_waiters(struct cid_domain *dom) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY */

/*
 * @t joins @dom: fork(2) and execve(2) for the mm, setns(2) and
 * unshare(2) for a namespace.
 *
 * In per CPU mode a joining user takes no ID of its own: it borrows the
 * one of the CPU it runs on at its schedule in. A task that joins while
 * it is running reaches user space before any schedule in, so it takes
 * its ID here instead, the way one would. That is setns(2), and the
 * failure path of execve(2) which rejoins an mm whose other threads are
 * users still; a fork(2) child does not run yet, and execve(2) and
 * unshare(2) otherwise join a domain they create, as its first user.
 */
static void cid_user_join(struct cid_domain *dom, struct task_struct *t, struct cid_user *u)
{
	bool percpu = false, flip = false;

	WARN_ON_ONCE(u->cid != MM_CID_UNSET);

	guard(mutex)(&dom->mutex);
	scoped_guard(cid_local, dom) {
		struct cid_pcpu *pcp = this_cpu_ptr(dom->pcpu);

		/* First user ? */
		if (!dom->users) {
			cid_add_user(dom, u);
			u->cid = cid_get(dom);
			/* Required for execve() */
			pcp->cid = u->cid;
			return;
		}

		if (!cid_add_user(dom, u)) {
			if (!cid_on_cpu(dom->mode))
				u->cid = cid_get(dom);
			break;
		}

		/*
		 * Handle the mode change and transfer current's CID. A
		 * running joiner has none to transfer: it takes one below.
		 */
		flip = true;
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
			/*
			 * A running joiner takes its CID below, under the
			 * runqueue lock: a user already, it may have taken
			 * one at a schedule in since.
			 */
			if (t != current)
				u->cid = cid_get(dom);
		}
		/* The fixup put IDs back in the pool. */
		cid_wake_waiters(dom);
	}

	/* The running joiner with no CID, see above. */
	if (t == current) {
		guard(task_rq_lock)(t);
		if (u->cid == MM_CID_UNSET)
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
	 * A failed fork(2) cleanup never gets here, so @current must be a
	 * user of the domain as @t is. That's true for exit() and the
	 * failed pthread_create() cleanup case.
	 */
	if (WARN_ON_ONCE(!cid_task_in_domain(current, dom) && current != t))
		return false;
	return true;
}

/*
 * @t leaves @dom: the CID held by the task is no longer required as the
 * task cannot return to user space in it.
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
			/* The ID the task held is back in the pool. */
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
 * A user worker (io_uring's, vhost's) takes no ID: it never returns to
 * user space and has no rseq area, and an ID of its would be one fewer for
 * the threads that report theirs.
 */
void sched_mm_cid_fork(struct task_struct *t)
{
	if (t->mm && !(t->flags & PF_USER_WORKER))
		cid_user_join(&t->mm->mm_cid, t, &t->mm_cid);
}

void sched_mm_cid_exit(struct task_struct *t)
{
	if (t->mm)
		cid_user_leave(&t->mm->mm_cid, t, &t->mm_cid);
}

/* Deactivate MM CID allocation across execve() */
void sched_mm_cid_before_execve(struct task_struct *t)
{
	sched_mm_cid_exit(t);
}

/* Reactivate MM CID after execve() */
void sched_mm_cid_after_execve(struct task_struct *t)
{
	sched_mm_cid_fork(t);
}

/*
 * Switch @dom to per CPU mode, the users being about to outnumber its
 * CIDs, with current's CID transferred, as cid_user_join() does: the
 * fixup that follows skips current. Under dom->lock, and this CPU's rq
 * lock for its record (cid_local).
 */
static void cid_switch_to_cpus(struct cid_domain *dom)
{
	lockdep_assert_held(&dom->lock);
	dom->pcpu_thrs = cid_calc_pcpu_thrs(dom);
	WRITE_ONCE(dom->mode, dom->mode ^ (MM_CID_TRANSIT | MM_CID_ONCPU));
	/* As in cid_update_max(): the store before the fixups' rq locks. */
	smp_mb();
	if (cid_task_in_domain(current, dom))
		cid_transit_to_cpu(cid_task_user(dom, current), this_cpu_ptr(dom->pcpu));
}

/*
 * Apply the concurrency bound of the cgroup limit: recompute max_cids,
 * switch owner mode when the user count crosses it, then drop CPU-owned
 * CIDs that are above the bound or cached on CPUs the group does not use.
 *
 * A decrease that leaves fewer CIDs than users switches to per CPU mode
 * first, under the bound as it was. Under the new one, the switch would
 * run short: until the fixup reaches a user that is not running, its task
 * owned CID stays taken, and another user scheduling in on the way, its
 * own CID dropped at its schedule out as the transition requires, would
 * find none below the bound. Under the old bound every user had a CID of
 * its own below it. The bound then drops on per CPU mode, where a user
 * scheduling in converges below it (cid_from_cpu()).
 */
void cid_apply_bound(struct cid_domain *dom)
{
	bool percpu, flip;

	lockdep_assert_held(&dom->mutex);
	/*
	 * IDs cached on CPUs the domain's group does not use first, before
	 * the fixups below allocate for the CPUs it does: a cgroup migration
	 * leaves such IDs behind on the old group's CPUs.
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
	/* A raised bound, or IDs put back in the pool: the waiters try again. */
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

	/* Users were given the IDs they waited for, or IDs went back in the pool. */
	cid_wake_handoffs(dom);
	cid_wake_waiters(dom);
	/*
	 * Needs to be unconditional because dom::lock cannot be held when
	 * scheduling work as cid_update_cpus_allowed() nests inside
	 * rq::lock and schedule_work() might end up in wakeup...
	 */
	schedule_work(&dom->work);
}

/*
 * Set up a domain: @pcpu, @cidmask and @cpus_allowed are the caller's
 * storage (the mm's trailing storage, a namespace's allocations); @p, when
 * given, seeds the allowed CPUs and the common ancestor group.
 */
DEFINE_PER_CPU(struct list_head, cid_cpu_doms);

void __init cid_init_cpus(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		INIT_LIST_HEAD(per_cpu_ptr(&cid_cpu_doms, cpu));
}

/* The domain goes: off the lists of the CPUs that still own one of its CIDs. */
void cid_domain_exit(struct cid_domain *dom)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cid_pcpu *pcp = per_cpu_ptr(dom->pcpu, cpu);

		if (list_empty(&pcp->node))
			continue;
		guard(rq_lock_irq)(cpu_rq(cpu));
		list_del_init(&pcp->node);
	}
}

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

#ifdef CONFIG_SCHED_MAX_CONCURRENCY
/*
 * Give the IDs of @dom that CPUs own without running a user of it: cached
 * for the next user there, they are what the waiters need; to them in
 * order, or back to the pool. Under each CPU's runqueue lock, as its
 * record wants.
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
 * Current, a user of @dom that got no ID at its schedule in (cid_wait()),
 * takes the one it was given (@w), or one at its return to user space as
 * the schedule in would, or none again. A held task takes none: it gets
 * one once it is let run.
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
	WRITE_ONCE(t->cid_wait, 0);
	if (!eligible)
		return true;
	/* Hands a given ID to this CPU the way a schedule in would, or takes one. */
	__cid_schedin(dom, t, u);
	return u->cid != MM_CID_UNSET;
}

/*
 * Current found no ID of @dom free below the bound at its schedule in.
 * The users all live under the cgroups that feed the bound, whose
 * placement keeps them at or below the effective limit the bound is, so
 * that is an over-presence of the moment, a transition window of the
 * placement (a task counted before it is held, or moved while it runs):
 * it queues as a waiter, gives the cached IDs no user runs on to the
 * waiters, itself maybe, and sleeps until a user's schedule out hands it
 * one (__cid_schedout()), or an ID put back in the pool, a user's leave
 * or an applied bound kicks it to try again (cid_wake_waiters()),
 * killable as a hold is. Queued before the reclaim, so that a schedule
 * out from then on gives rather than caches, and none is cached unseen
 * between the reclaim and the sleep: the reclaim's runqueue locks order
 * the count before the schedule outs that follow them.
 */
static void cid_resume(struct cid_domain *dom, struct task_struct *t, struct cid_user *u)
{
	struct cid_waiter w = { .task = t, .cid = MM_CID_UNSET };

	INIT_LIST_HEAD(&w.node);
	while (!cid_acquire(dom, t, u, &w)) {
		if (fatal_signal_pending(t))
			break;
		scoped_guard(raw_spinlock_irq, &dom->lock) {
			list_add_tail(&w.node, &dom->waitq);
			dom->waiters++;
		}
		cid_reclaim(dom);
		for (;;) {
			set_current_state(TASK_KILLABLE);
			if (list_empty(&w.node) || fatal_signal_pending(t))
				break;
			schedule();
		}
		__set_current_state(TASK_RUNNING);
		scoped_guard(raw_spinlock_irq, &dom->lock) {
			/* Still queued, uncounted once given an ID (cid_give()). */
			if (!list_empty(&w.node)) {
				list_del_init(&w.node);
				if (w.cid == MM_CID_UNSET)
					dom->waiters--;
			}
		}
	}
	/* The loop ends with @w consumed: a given ID is taken or given on by the acquire. */
}

/*
 * From resume_user_mode_work(): current found no ID free below the bound
 * of its mm's domain at its schedule in. Its ID of that domain reads MM_CID_UNSET until it has one,
 * which a user of the ID in the kernel tests for; user space never sees
 * it, the task returns with its IDs or goes to its exit.
 */
void sched_cid_resume(void)
{
	struct task_struct *t = current;

	if (t->mm_cid.active && t->mm_cid.cid == MM_CID_UNSET)
		cid_resume(&t->mm->mm_cid, t, &t->mm_cid);
	rseq_force_update();
}
#endif /* CONFIG_SCHED_MAX_CONCURRENCY */
