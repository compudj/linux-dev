// SPDX-License-Identifier: GPL-2.0

/*
 * cpu.concurrency.max limits the number of CPUs a cgroup uses at once. See
 * Documentation/scheduler/sched-max-concurrency.rst. The hooks the
 * scheduler calls are inline in concurrency.h.
 */

#include <linux/sched/isolation.h>
#include <linux/sched/clock.h>
#include <linux/sched/mm.h>
#include <linux/cpuset.h>
#include <linux/stop_machine.h>
#include <linux/ipc_namespace.h>
#include <linux/nsproxy.h>
#include <linux/task_work.h>
#include "sched.h"
#include "smp.h"
#include <trace/events/sched.h>
/*
 * Constants
 */

/*
 * Iterate over the active groups, from the group of @p upwards. Stop at
 * the first group which is not active.
 */
#define for_each_conc_level(lvl, p)						\
	for (lvl = task_group(p); conc_active(&lvl->conc); lvl = lvl->parent)
/* The active groups from @tg up. */
#define for_each_conc_level_from(lvl, tg)						\
	for (lvl = (tg); conc_active(&lvl->conc); lvl = lvl->parent)

/* An ancestor of the top group, outside the moved set, that counts it. */
struct conc_sm_ancestor {
	struct task_group	*tg;
	bool			reserved;	/* its dst slot was created by us */
	bool			entity;		/* it counts the top group on dst */
	bool			drained;	/* its src slot emptied by the move */
};

/* A destination slot reserved for one task, with its ancestors' entities. */
struct conc_resv {
	struct task_group	*tg;
	struct conc_sm_ancestor	*anc;
	int			cpu;
	int			nr_anc;
};

/*
 * Globals and forward declarations
 */

/*
 * Concurrency accounting for cpu.concurrency.max, see
 * Documentation/scheduler/sched-max-concurrency.rst
 *
 * Lock order: cpuset_mutex -> conc_mutex -> tg->conc.lock. The locks of two
 * groups never nest (see conc_claim_slow()). tg->conc.lock nests inside
 * p->pi_lock and rq->lock, never the other way.
 */
DEFINE_STATIC_KEY_FALSE(sched_conc_enabled);
DEFINE_PER_CPU(const struct cpumask *, conc_select_cpus);
static DEFINE_PER_CPU(cpumask_var_t, conc_select_mask);
static DEFINE_PER_CPU(cpumask_var_t, conc_entitled_mask);

/* Serializes configuration: limit writes, activation, eviction and retries. */
static DEFINE_MUTEX(conc_mutex);

/*
 * The CPUs that are going offline. No group adds a slot on such a CPU,
 * unless forced. No destination is chosen there. The CPU is still active
 * meanwhile, for the tasks being moved off it and for the tasks that
 * arrive on it late.
 */
static struct cpumask conc_offlining_cpus __read_mostly;

static int conc_subtree_snapshot(struct task_group *top, struct cgroup_subsys_state ***out,
				 int *nr);
static void conc_subtree_release(struct cgroup_subsys_state **arr, int n);

static void conc_irq_work(struct irq_work *work);
static void conc_cleaner_work(struct work_struct *work);
static int conc_resv_cpu(struct conc_resv *resv);

static inline bool conc_task_mask_fixed(struct task_struct *t);

#ifdef CONFIG_SCHED_MM_CID
/*
 * The domains of a cgroup migration in progress, from can_attach() to the
 * end of attach(). The threads leave the css_sets of the source groups
 * before the cpu controller switches their task_group. A slot of a source
 * group which is drained in that window finds no task of theirs. Their IDs
 * on the CPU are dropped all the same, see conc_slot_removed(). Under
 * conc_mutex.
 */
static struct cid_domain **conc_attach_doms;
static int conc_nr_attach_doms;

static void conc_dom_update_bound(struct cid_domain *dom);
#endif /* CONFIG_SCHED_MM_CID */

static struct task_group *conc_pin_task_group(struct task_struct *p);

static void conc_notify_tree(struct task_group *top);

atomic_t conc_nr_held;

static inline bool conc_cpu_usable(int cpu)
{
	return cpu_active(cpu) && !cpumask_test_cpu(cpu, &conc_offlining_cpus);
}

#ifndef CONFIG_SCHED_MM_CID
void sched_cid_resume(void) { }
#endif /* !CONFIG_SCHED_MM_CID */

int alloc_conc_sched_group(struct task_group *tg)
{
	struct tg_concurrency *conc = &tg->conc;

	raw_spin_lock_init(&conc->lock);
	conc->irq_work = IRQ_WORK_INIT_HARD(conc_irq_work);
	INIT_WORK(&conc->work, conc_cleaner_work);
	return 1;
}

/*
 * Also called on the failure path of sched_create_group(). That path may
 * run before alloc_conc_sched_group(). Nothing was set up then.
 */
void free_conc_sched_group(struct task_group *tg)
{
	if (!tg->conc.present)
		return;
	free_percpu(tg->conc.present);
	free_cpumask_var(tg->conc.used);
	free_cpumask_var(tg->conc.unused_cores);
}

void __init conc_init_root(void)
{
	int i;

	/* The root group is never limited and never counts. */
	raw_spin_lock_init(&root_task_group.conc.lock);
	/*
	 * The allocations are not checked. A failure at boot is fatal, as for
	 * the per-CPU masks that sched_init() allocates.
	 */
	for_each_possible_cpu(i) {
		int node = cpu_to_node(i);

		zalloc_cpumask_var_node(&per_cpu(conc_select_mask, i), GFP_KERNEL, node);
		zalloc_cpumask_var_node(&per_cpu(conc_entitled_mask, i), GFP_KERNEL, node);
	}
}

/*
 * Return true when @p's search must skip the unused siblings of the used
 * CPUs of @conc.
 *
 * Slots are sticky, and the siblings of a core share its execution
 * resources. A slot on the sibling of a used CPU is worth less than a slot
 * on a core where no CPU is used. So skip the siblings while such a core
 * exists among the active CPUs of @p's mask.
 *
 * A limit at or above the number of CPUs in @p's mask has room for all of
 * them. Nothing is spread then.
 *
 * The masks are read without the group lock. A stale view only places a
 * slot less well.
 */
static inline bool conc_spread(struct task_struct *p, struct tg_concurrency *conc)
{
	return sched_smt_active() && conc->effective < p->nr_cpus_allowed &&
	       cpumask_first_and_and(p->cpus_ptr, conc->unused_cores,
				     cpu_active_mask) < nr_cpu_ids;
}

/* Is @cpu the unused sibling of a used CPU of @conc? */
static inline bool conc_on_used_core(struct tg_concurrency *conc, int cpu)
{
	return !cpumask_test_cpu(cpu, conc->used) &&
	       !cpumask_test_cpu(cpu, conc->unused_cores);
}

/*
 * Record the CPUs which the limit lets @p use, in conc_select_cpus, before
 * a class selects a CPU for @p.
 *
 * Walk from @p's group up. Skip the groups without a limit. Stop at the
 * first group without headroom and record its used set, within @p's mask.
 * Record nothing if that set is empty. The class then picks a usable CPU,
 * and conc_select_cpu() rarely has to redirect.
 *
 * A group with headroom on the way up spreads its slots over cores, see
 * conc_spread(). The innermost such group narrows the mask first.
 */
void __conc_select_prepare(struct task_struct *p)
{
	struct cpumask *mask = this_cpu_cpumask_var_ptr(conc_select_mask);
	struct task_group *lvl;
	bool spread = false;

	if (!conc_task_active(p))
		return;
	for_each_conc_level(lvl, p) {
		struct tg_concurrency *conc = &lvl->conc;

		if (!conc->effective)
			continue;
		if (READ_ONCE(conc->nr_used) < conc->effective) {
			if (!spread && conc_spread(p, conc)) {
				cpumask_or(mask, conc->used, conc->unused_cores);
				cpumask_and(mask, mask, p->cpus_ptr);
				__this_cpu_write(conc_select_cpus, mask);
				spread = true;
			}
			continue;
		}
		/*
		 * Keep the part of the spread mask which is in the used
		 * set. If there is none, use the used set.
		 */
		if (spread && cpumask_and(mask, mask, conc->used))
			return;
		if (cpumask_and(mask, p->cpus_ptr, conc->used))
			__this_cpu_write(conc_select_cpus, mask);
		else
			__this_cpu_write(conc_select_cpus, NULL);
		return;
	}
}

/*
 * Ask the cleaner to reclaim an idle slot, so that the next arrival finds
 * headroom.
 */
static inline void conc_want_headroom(struct tg_concurrency *conc)
{
	irq_work_queue(&conc->irq_work);
}

/*
 * Add @cpu to the used set. The SMT core of @cpu is no longer unused.
 */
static void conc_used_add(struct tg_concurrency *conc, int cpu)
{
	cpumask_set_cpu(cpu, conc->used);
	cpumask_andnot(conc->unused_cores, conc->unused_cores, cpu_smt_mask(cpu));
	WRITE_ONCE(conc->nr_used, conc->nr_used + 1);
}

/*
 * Return the limit enforced on @conc now, or UINT_MAX when it has none.
 * This is the value of conc_effective_report(), with UINT_MAX in place of
 * 0. Call under conc->lock.
 */
static unsigned int conc_report_ceiling(struct tg_concurrency *conc)
{
	if (!conc->effective)
		return UINT_MAX;
	return conc->shrink_pending ? max(conc->effective, conc->nr_used) : conc->effective;
}

/*
 * Record that the limit enforced on @conc is about to drop from @from. The
 * domains which @conc bounds may have handed out concurrency IDs below
 * @from. Those IDs stay until the bounds are applied again, see
 * conc_bounds_apply(). Until then cpu.concurrency.effective must not show
 * less than @from. Call under conc->lock.
 */
static void conc_report_drop(struct tg_concurrency *conc, unsigned int from)
{
	lockdep_assert_held(&conc->lock);
	conc->shown_floor = max(conc->shown_floor, from);
	conc->bounds_pending = true;
}

/*
 * Remove @cpu from the used set. Its SMT core is unused again unless a
 * sibling is used.
 */
static void conc_used_del(struct tg_concurrency *conc, int cpu)
{
	/*
	 * A group over its limit reports the number of CPUs it uses. That
	 * number drops by one here. Queue the cleaner to apply the new value
	 * to the concurrency bounds, see conc_cleaner_work(). A configuration
	 * change under way may apply it first.
	 */
	if (conc->effective && conc->shrink_pending && conc->nr_used > conc->effective) {
		conc_report_drop(conc, conc->nr_used);
		irq_work_queue(&conc->irq_work);
	}
	cpumask_clear_cpu(cpu, conc->used);
	if (!cpumask_intersects(cpu_smt_mask(cpu), conc->used))
		cpumask_or(conc->unused_cores, conc->unused_cores, cpu_smt_mask(cpu));
	WRITE_ONCE(conc->nr_used, conc->nr_used - 1);
}

/*
 * Return true if @conc can add a slot within its limit. The caller holds
 * conc->lock.
 */
static bool __conc_has_room(struct tg_concurrency *conc)
{
	lockdep_assert_held(&conc->lock);
	return !conc->effective || conc->nr_used < conc->effective;
}

static __maybe_unused struct cpumask *conc_alloc_scratch(void)
{
	return kmalloc(cpumask_size(), GFP_KERNEL);
}

/*
 * Slow path of conc_claim(). The (@tg, @cpu) slot does not exist, or it
 * does not accept entities. Add the slot when the limit allows it. Add
 * one entity to the parent's slot when the parent counts this group.
 *
 * A claim never nests group locks. The parent's entity is claimed with
 * our lock dropped, and the slot state is checked again afterwards. A
 * parent entity that ends up unused is released again. The parent's slot
 * stays, at zero, so this is harmless.
 */
enum conc_claim_result conc_claim_slow(struct task_group *tg, int cpu, bool force,
				   struct task_struct *p)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, cpu);
	bool parent_claimed = false, parent_used = false, parent_inactive = false;
	enum conc_claim_result ret;
	unsigned long flags;
	int v;

again:
	raw_spin_lock_irqsave(&conc->lock, flags);
	if (!conc->active) {
		ret = CONC_INACTIVE;
		goto out;
	}

	v = atomic_read(slot);
	if (v >= 0) {
		/* The slot appeared while we were taking the lock. */
		atomic_inc(slot);
		ret = CONC_CLAIMED;
		goto out;
	}

	if (v != CONC_SLOT_UNUSED) {
		/* A task fills the slot that was reserved for it. */
		if (p && conc_slot_reserved(v) && READ_ONCE(p->conc_resv_cpu) == cpu) {
			atomic_inc(slot);
			ret = CONC_CLAIMED;
			goto out;
		}
		/*
		 * The slot is draining, reserved or evicting. Refuse the claim
		 * unless it is forced.
		 */
		if (!force) {
			ret = CONC_REFUSED;
			goto out;
		}
		if (v == CONC_SLOT_DRAINING)
			atomic_set(slot, 1);	/* the cleaner notices and keeps the slot */
		else
			atomic_inc(slot);	/* reserved or evicting: count one more */
		ret = CONC_CLAIMED;
		goto out;
	}

	/* There is no slot. Add none on a CPU going offline, unless forced. */
	if (!force && cpumask_test_cpu(cpu, &conc_offlining_cpus)) {
		ret = CONC_REFUSED;
		goto out;
	}
	/*
	 * Add a slot when the limit allows it. An evicting slot still counts:
	 * its tasks run there until they are moved.
	 */
	if (!__conc_has_room(conc)) {
		if (!force) {
			conc_want_headroom(conc);
			ret = CONC_REFUSED;
			goto out;
		}
		conc->shrink_pending = true;
	}

	if (conc->propagate && !parent_claimed && !WARN_ON_ONCE(parent_inactive)) {
		raw_spin_unlock_irqrestore(&conc->lock, flags);
		ret = conc_claim(tg->parent, cpu, force, p);
		if (ret == CONC_REFUSED)
			return ret;
		/*
		 * CONC_CLAIMED: the parent counts us. Otherwise the parent
		 * stopped counting and claimed nothing. It cleared our
		 * @propagate before that. The next pass sees it.
		 */
		parent_claimed = ret == CONC_CLAIMED;
		parent_inactive = !parent_claimed;
		goto again;
	}

	atomic_set(slot, 1);
	conc_used_add(conc, cpu);
	parent_used = conc->propagate;
	ret = CONC_CLAIMED;
out:
	raw_spin_unlock_irqrestore(&conc->lock, flags);
	if (parent_claimed && !parent_used)
		conc_release(tg->parent, cpu);
	return ret;
}

/*
 * Bring @p's mask up to date with its cpuset. Return true if the mask
 * changed.
 *
 * A cpuset change updates the masks of the tasks one by one. The slots of
 * the group move earlier, with the cpuset as a whole, see
 * conc_task_entitled(). A task can thus wake up with a mask which is behind
 * its cpuset. Apply here the mask which the cpuset's pass would apply.
 *
 * The caller holds p->pi_lock. @p is not queued.
 */
static bool conc_task_mask_behind(struct task_struct *p)
{
	struct cpumask *ent = this_cpu_cpumask_var_ptr(conc_entitled_mask);
	struct affinity_context ac = { .new_mask = ent, .flags = 0 };

	lockdep_assert_held(&p->pi_lock);
	if (p->on_rq || conc_task_mask_fixed(p) || is_migration_disabled(p) ||
	    p->migration_pending)
		return false;
	scoped_guard(rcu)
		cpuset_task_cpus_rcu(p, ent);
	if (p->user_cpus_ptr && cpumask_intersects(ent, p->user_cpus_ptr))
		cpumask_and(ent, ent, p->user_cpus_ptr);
	if (cpumask_equal(ent, p->cpus_ptr))
		return false;
	/*
	 * The cpuset was read without its lock, in the middle of a change.
	 * Do not apply a mask without an active CPU. The pass of the cpuset
	 * brings the final mask.
	 */
	if (!cpumask_intersects(ent, cpu_active_mask))
		return false;
	scoped_guard(__task_rq_lock, p)
		do_set_cpus_allowed(p, &ac);
	return true;
}

/*
 * Mark @p as the holder of the slot claimed for it on @cpu.
 *
 * A task being woken up or forked is not queued. p->pi_lock keeps every
 * other path off its accounting.
 *
 * A held task (@held) is queued but out of its class. The selection can end
 * its hold. A fallback widening (select_fallback_rq(), or the widening of a
 * pinned task in conc_select_cpu()) changes the mask of a queued task. That
 * dequeues the task, and the dequeue ends the hold. The task is then back
 * in its class without a slot, and a new hold is requested. It can run or
 * move under its runqueue lock alone.
 *
 * So take that lock. Give the slot only if the task is still held.
 * Otherwise release the slot.
 */
static void conc_select_account(struct task_struct *p, struct task_group *tg, int cpu,
			      bool held)
{
	if (held) {
		scoped_guard(__task_rq_lock, p) {
			if (p->conc_held) {
				p->conc_accounted = 1;
				return;
			}
		}
		conc_release(tg, cpu);
		return;
	}
	p->conc_accounted = 1;
}

/*
 * Claim a slot for @p after its class chose @cpu, see select_task_rq().
 * Used for wakeups, forks and the placement of a held task.
 *
 * Return the CPU to use. @p holds a slot on it when its group counts,
 * except when no CPU within the limit was found. Then @p runs without a
 * slot and a hold is requested, see conc_hold_request(). Return -1 for a
 * held task which stays held.
 *
 * A CPU is usable when @p's group can claim it. It is a CPU of its own used
 * set, or a CPU of an active ancestor's used set when the group has
 * headroom.
 *
 * The classes already search within the used set. This catches what they
 * miss:
 *  - the used set changed during the search;
 *  - a wakeup took the previous CPU without a search;
 *  - the rt or deadline search found no used CPU, because its mask is
 *    narrowed after cpupri_find() or cpudl_find() chose a priority level.
 */
int conc_select_cpu(struct task_struct *p, int cpu)
{
	struct task_group *tg = task_group(p), *lvl;
	int prev = task_cpu(p), c, attempt;
	enum conc_claim_result ret;
	bool updated = false, widened = false, held = p->conc_held;

	lockdep_assert_held(&p->pi_lock);
	WARN_ON_ONCE(p->conc_accounted);

again:
	for (attempt = 0; attempt < 3; attempt++) {
		ret = conc_claim(tg, cpu, false, p);
		if (ret != CONC_REFUSED)
			goto done;

		for_each_conc_level_from(lvl, tg) {
			/*
			 * Try the previous CPU first. It is the most likely to
			 * be cache hot.
			 */
			if (prev != cpu && cpumask_test_cpu(prev, lvl->conc.used) &&
			    cpumask_test_cpu(prev, p->cpus_ptr) && is_cpu_allowed(p, prev)) {
				ret = conc_claim(tg, prev, false, p);
				if (ret != CONC_REFUSED) {
					cpu = prev;
					goto done;
				}
			}

			/* Then an idle CPU of the used set, then any. */
			for_each_cpu_and(c, lvl->conc.used, p->cpus_ptr) {
				if (c == cpu || c == prev || !available_idle_cpu(c) ||
				    !is_cpu_allowed(p, c))
					continue;
				ret = conc_claim(tg, c, false, p);
				if (ret != CONC_REFUSED) {
					cpu = c;
					goto done;
				}
			}
			for_each_cpu_wrap(c, lvl->conc.used, prev + 1) {
				if (c == cpu || c == prev || !cpumask_test_cpu(c, p->cpus_ptr) ||
				    !is_cpu_allowed(p, c))
					continue;
				ret = conc_claim(tg, c, false, p);
				if (ret != CONC_REFUSED) {
					cpu = c;
					goto done;
				}
			}
		}

		/*
		 * No usable CPU. Try a slot of @p's own group which is
		 * being evicted or drained. The group's concurrency IDs are
		 * still on that CPU. @p is then a late arrival there, and
		 * the eviction deals with it. Otherwise @p would run
		 * without a slot on a new CPU.
		 *
		 * The claim fails when the slot is gone. The used set
		 * changed then, so select again.
		 */
		for_each_cpu_and(c, tg->conc.used, p->cpus_ptr) {
			if (!is_cpu_allowed(p, c))
				continue;
			if (conc_claim_late(tg, c)) {
				ret = CONC_CLAIMED;
				cpu = c;
				goto done;
			}
		}
	}

	/*
	 * @p's mask may be behind its cpuset, see conc_task_mask_behind().
	 * Update it and select once more.
	 */
	if (!updated && conc_task_mask_behind(p)) {
		updated = true;
		if (!cpumask_test_cpu(cpu, p->cpus_ptr) || !is_cpu_allowed(p, cpu))
			cpu = select_fallback_rq(prev, p);
		goto again;
	}

	/*
	 * Every active CPU of @p's mask is going offline, and @p wakes up
	 * before the unplug widened its mask. Give it now the mask which
	 * select_fallback_rq() would give it once those CPUs are gone. It
	 * can then join a used CPU. Otherwise it would add a slot over the
	 * limit on a CPU going offline, and take a concurrency ID above the
	 * bound there.
	 */
	if (!widened && conc_task_pinned_offlining(p)) {
		widened = true;
		if (!cpuset_cpus_allowed_fallback(p) || conc_task_pinned_offlining(p))
			set_cpus_allowed_force(p, task_cpu_fallback_mask(p));
		if (!cpumask_test_cpu(cpu, p->cpus_ptr) || !is_cpu_allowed(p, cpu) ||
		    cpumask_test_cpu(cpu, &conc_offlining_cpus))
			cpu = select_fallback_rq(prev, p);
		goto again;
	}

	/*
	 * No CPU within the limit was found. @p's mask misses the used sets
	 * and the group may not add a slot.
	 *
	 * A held task stays held.
	 *
	 * Any other task runs without a slot and takes no concurrency ID.
	 * It runs on the class's choice, or on another allowed CPU if that
	 * one is going offline. It finishes its kernel-mode work and is
	 * held at its return to user space, see conc_hold_request().
	 */
	if (p->conc_held)
		return -1;
	if (cpumask_test_cpu(cpu, &conc_offlining_cpus)) {
		for_each_cpu_and(c, p->cpus_ptr, cpu_active_mask) {
			if (c != cpu && conc_cpu_usable(c) && is_cpu_allowed(p, c)) {
				cpu = c;
				break;
			}
		}
	}
	conc_hold_request(p, CONC_HOLD_UNPLACED);
	return cpu;
done:
	if (ret == CONC_CLAIMED)
		conc_select_account(p, tg, cpu, held);
	return cpu;
}

/*
 * Move @p's slot from @old_tg to its new group. Called from
 * sched_move_task() after task_group(p) changed. The task is dequeued. The
 * caller holds p->pi_lock and the runqueue lock.
 */
void conc_task_change_group(struct task_struct *p, struct task_group *old_tg)
{
	int cpu = task_cpu(p);

	if (p->conc_accounted) {
		p->conc_accounted = 0;
		conc_release(old_tg, cpu);
	}
	/*
	 * A held task claims nothing. The new group may have room for it,
	 * so kick the placement work.
	 */
	if (p->conc_held) {
		conc_hold_kick();
		return;
	}
	/*
	 * Only a present task claims a slot. It is queued, or it is between
	 * its selection and its enqueue.
	 */
	if (!conc_task_active(p) ||
	    (!task_on_rq_queued(p) && READ_ONCE(p->__state) != TASK_WAKING))
		return;
	/*
	 * If the new group has no room, hold @p at its next return to user
	 * space.
	 */
	switch (conc_claim(task_group(p), cpu, false, p)) {
	case CONC_CLAIMED:
		p->conc_accounted = 1;
		break;
	case CONC_REFUSED:
		conc_hold_request(p, CONC_HOLD_UNPLACED);
		break;
	default:
		break;
	}
}

static inline bool conc_tg_has_headroom(struct task_group *tg)
{
	return !tg->conc.effective || READ_ONCE(tg->conc.nr_used) < tg->conc.effective;
}

/*
 * Return false if @p is held, or if the limit would refuse a move of @p to
 * @cpu. This is a lockless check for the load balancer. conc_task_move()
 * validates the move itself.
 */
bool conc_can_move(struct task_struct *p, int cpu)
{
	struct task_group *tg;

	if (conc_task_held(p))
		return false;
	if (!conc_task_active(p))
		return true;
	tg = task_group(p);
	if (atomic_read(per_cpu_ptr(tg->conc.present, cpu)) >= 0)
		return true;
	if (!conc_tg_has_headroom(tg))
		return false;
	/* A new slot: one per core first, see conc_spread(). */
	return !conc_on_used_core(&tg->conc, cpu) || !conc_spread(p, &tg->conc);
}

/*
 * Restrict the CPU search mask @mask of @p by the limit of its group.
 * Return false if no CPU is left.
 *
 * Without headroom, keep the used set. With headroom, drop the unused
 * siblings of the used CPUs, see conc_spread().
 */
bool conc_constrain_mask(struct task_struct *p, struct cpumask *mask)
{
	struct tg_concurrency *conc;
	int cpu;

	if (!conc_task_active(p))
		return true;
	conc = &task_group(p)->conc;
	if (!conc_tg_has_headroom(task_group(p)))
		return cpumask_and(mask, mask, conc->used);
	/*
	 * Spread over cores, see conc_spread(). Do it only if a CPU is left
	 * afterwards. An rt or deadline task takes the sibling of a used
	 * CPU rather than wait.
	 */
	if (conc_spread(p, conc) && (cpumask_intersects(mask, conc->used) ||
				 cpumask_intersects(mask, conc->unused_cores))) {
		for_each_cpu(cpu, mask) {
			if (conc_on_used_core(conc, cpu))
				__cpumask_clear_cpu(cpu, mask);
		}
	}
	return true;
}

/*
 * Return the destination CPU for an affinity change of @p. It is in
 * @allowed and in @valid. Prefer the CPU reserved for the change, then a
 * used CPU of @p's group, then a CPU which is not going offline.
 */
int conc_pick_dest(struct task_struct *p, const struct cpumask *valid,
		 const struct cpumask *allowed, struct affinity_context *ctx)
{
	int cpu;

	if (conc_task_active(p)) {
		struct task_group *tg = task_group(p);

		for_each_cpu_and(cpu, allowed, tg->conc.used) {
			int v = atomic_read(per_cpu_ptr(tg->conc.present, cpu));

			/* A reserved slot must not run anything yet. */
			if (cpumask_test_cpu(cpu, valid) && !conc_slot_reserved(v))
				return cpu;
		}
	}
	/* Not a CPU going offline, when there is any other. */
	if (cpumask_intersects(valid, &conc_offlining_cpus)) {
		for_each_cpu_and(cpu, valid, allowed) {
			if (!cpumask_test_cpu(cpu, &conc_offlining_cpus))
				return cpu;
		}
	}
	return cpumask_any_and_distribute(valid, allowed);
}

static void conc_irq_work(struct irq_work *work)
{
	struct tg_concurrency *conc = container_of(work, struct tg_concurrency, irq_work);

	/* conc->lock cannot be held while scheduling work, see cid_irq_work(). */
	schedule_work(&conc->work);
}

/* A slot was removed. Recompute whether the group is over its limit. */
static inline void conc_slot_count_dropped(struct tg_concurrency *conc)
{
	lockdep_assert_held(&conc->lock);
	conc->shrink_pending = conc->effective && conc->nr_used > conc->effective;
}

/*
 * Remove the (@tg, @cpu) slot. Called with conc->lock held, after the slot's
 * value was set negative by the caller so that no entity can be added.
 */
static void __conc_remove_slot(struct task_group *tg, int cpu)
{
	struct tg_concurrency *conc = &tg->conc;

	lockdep_assert_held(&conc->lock);
	atomic_set(per_cpu_ptr(conc->present, cpu), CONC_SLOT_UNUSED);
	conc_used_del(conc, cpu);
	conc_slot_count_dropped(conc);
	if (conc->propagate)
		conc_release(tg->parent, cpu);
	/* A held task may find room now. */
	conc_hold_kick();
}

#ifdef CONFIG_SCHED_MM_CID
/* Drop a reference on the object which @dom is embedded in. */
static void cid_domain_put(struct cid_domain *dom)
{
	mmdrop(cid_domain_mm(dom));
}

/*
 * The domains of @t, each with a reference held: its mm's. Returns their
 * number.
 */
static int conc_task_domains(struct task_struct *t, struct cid_domain **doms)
{
	int n = 0;

	task_lock(t);
	if (t->mm) {
		mmgrab(t->mm);
		doms[n++] = &t->mm->mm_cid;
	}
	task_unlock(t);
	return n;
}

static void conc_drop_cid_on_cpu(struct cid_domain *dom, int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	/* Nothing cached: the lock would find nothing to drop. */
	if (!cid_on_cpu(READ_ONCE(per_cpu_ptr(dom->pcpu, cpu)->cid)))
		return;
	/* Remote access to the per-CPU storage requires rq_lock */
	guard(rq_lock_irq)(rq);
	/*
	 * The task which runs here owns the cached ID while it is present.
	 * A task which has blocked can still be current while the runqueue
	 * picks its successor with the lock dropped, see
	 * sched_balance_newidle(). Such a task gave its slot back. The slot
	 * may be going away, and the ID must go with it. The switch out of
	 * the task would drop the ID too late for a CPU which allocates in
	 * between.
	 */
	if (!cid_task_in_domain(rq->curr, dom) || !task_on_rq_queued(rq->curr))
		cid_drop_on_cpu(dom, per_cpu_ptr(dom->pcpu, cpu));
}

/*
 * A slot is going away. Drop the CPU owned CIDs cached on @cpu, see below
 * for which domains. A CPU outside the used set then never holds an ID,
 * and stale caches cannot exhaust the IDs below the bound. Child groups
 * are drained before their parent, so their tasks were handled already.
 */
static void conc_slot_removed(struct task_group *tg, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct cid_pcpu *pcp, *tmp;
	int i;

	lockdep_assert_held(&conc_mutex);
	for (i = 0; i < conc_nr_attach_doms; i++)
		conc_drop_cid_on_cpu(conc_attach_doms[i], cpu);

	/*
	 * Walk the domains whose CID the CPU owns, not the tasks of the
	 * group. This also drops the CIDs of domains of other groups, which
	 * costs them an allocation at their next schedule in on this CPU. A
	 * walk of the tasks of the group would cost O(tasks) at every slot
	 * drain.
	 */
	guard(rq_lock_irq)(rq);
	list_for_each_entry_safe(pcp, tmp, per_cpu_ptr(&cid_cpu_doms, cpu), node) {
		struct cid_domain *dom = pcp->dom;

		if (!cid_task_in_domain(rq->curr, dom) || !task_on_rq_queued(rq->curr))
			cid_drop_on_cpu(dom, pcp);
	}
}

/*
 * Refresh the CID bounds of the domains of @t. If @walk is given, skip the
 * domains which the bounds walk @walk already refreshed (under conc_mutex).
 */
static void conc_task_update_bounds(struct task_struct *t, u64 walk)
{
	struct cid_domain *doms[2];
	int i, n = conc_task_domains(t, doms);

	for (i = 0; i < n; i++) {
		if (!walk || doms[i]->bounds_walk != walk) {
			if (walk)
				doms[i]->bounds_walk = walk;
			conc_dom_update_bound(doms[i]);
		}
		cid_domain_put(doms[i]);
	}
}

/* Refresh the CID bounds of the tasks being attached. */
static __maybe_unused void conc_update_tset_mm_bounds(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct task_struct *task;

	cgroup_taskset_for_each(task, css, tset)
		conc_task_update_bounds(task, 0);
}

/*
 * Refresh the CID bounds of every domain with a task in the subtree of
 * @top, each domain once. A refresh depends on the domain alone and looks
 * at every possible CPU, see conc_dom_drop_stale_cids(). The threads of a
 * process share their domain. Call under conc_mutex.
 */
static void conc_update_mm_bounds(struct task_group *top)
{
	static u64 walk;
	struct cgroup_subsys_state *css;

	lockdep_assert_held(&conc_mutex);
	walk++;
	rcu_read_lock();
	css_for_each_descendant_pre(css, &top->css) {
		struct css_task_iter it;
		struct task_struct *t;

		/* The reference pins the position across the sleep. */
		if (!css_tryget_online(css))
			continue;
		rcu_read_unlock();
		css_task_iter_start(css, 0, &it);
		while ((t = css_task_iter_next(&it)))
			conc_task_update_bounds(t, walk);
		css_task_iter_end(&it);
		rcu_read_lock();
		css_put(css);
	}
	rcu_read_unlock();
}

/*
 * Call before the threads of a cgroup migration leave their groups. Drop
 * the IDs which their domains cache on the CPUs where the source group has
 * an idle slot, or no slot. The group drains such a slot later and does
 * not find the threads there. A thread which runs on such a CPU again
 * allocates a new ID. A slot with tasks, or in a transient state, keeps
 * its ID.
 *
 * Remember the domains in conc_attach_doms until conc_attach_forget_mms().
 */
static __maybe_unused void conc_attach_drop_idle_cids(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct task_struct *task;
	struct cid_domain **doms;
	int n = 0, cap = 0;

	cgroup_taskset_for_each(task, css, tset)
		cap += 2;
	doms = kvcalloc(cap, sizeof(*doms), GFP_KERNEL);

	cgroup_taskset_for_each(task, css, tset) {
		struct task_group *tg = conc_pin_task_group(task);
		struct cid_domain *tdoms[2];
		int cpu, i, k, nt;

		if (!tg)
			continue;
		nt = conc_task_domains(task, tdoms);
		for (k = 0; k < nt; k++) {
			struct cid_domain *dom = tdoms[k];

			for_each_possible_cpu(cpu) {
				int v = atomic_read(per_cpu_ptr(tg->conc.present, cpu));

				if (v != 0 && v != CONC_SLOT_UNUSED)
					continue;
				conc_drop_cid_on_cpu(dom, cpu);
			}
			/*
			 * Remember the domain until the migration is
			 * over, see conc_slot_removed().
			 */
			for (i = 0; doms && i < n; i++) {
				if (doms[i] == dom)
					break;
			}
			if (doms && i == n && n < cap)
				doms[n++] = dom;
			else
				cid_domain_put(dom);
		}
		css_put(&tg->css);
	}
	mutex_lock(&conc_mutex);
	conc_attach_doms = doms;
	conc_nr_attach_doms = doms ? n : 0;
	mutex_unlock(&conc_mutex);
}

static __maybe_unused void conc_attach_forget_mms(void)
{
	struct cid_domain **doms;
	int i, n;

	/*
	 * Only the migrations set and clear it, and cgroup_mutex serializes
	 * them.
	 */
	if (!READ_ONCE(conc_attach_doms))
		return;
	mutex_lock(&conc_mutex);
	doms = conc_attach_doms;
	n = conc_nr_attach_doms;
	conc_attach_doms = NULL;
	conc_nr_attach_doms = 0;
	mutex_unlock(&conc_mutex);
	for (i = 0; i < n; i++)
		cid_domain_put(doms[i]);
	kvfree(doms);
}
#else /* !CONFIG_SCHED_MM_CID */
static void conc_slot_removed(struct task_group *tg, int cpu) { }

static __maybe_unused void conc_update_tset_mm_bounds(struct cgroup_taskset *tset) { }
static __maybe_unused void conc_attach_drop_idle_cids(struct cgroup_taskset *tset) { }
static __maybe_unused void conc_attach_forget_mms(void) { }
static void conc_update_mm_bounds(struct task_group *top) { }
#endif /* !CONFIG_SCHED_MM_CID */

/*
 * Apply the concurrency bounds of every domain with a task in the subtree
 * of @top. Then let cpu.concurrency.effective of each group in the subtree
 * show the value which the bounds follow now. A group whose value dropped
 * again meanwhile, see conc_report_drop(), keeps its floor until the
 * bounds are applied again. Call under conc_mutex.
 */
static void conc_bounds_apply(struct task_group *top)
{
	struct cgroup_subsys_state *css;

	lockdep_assert_held(&conc_mutex);

	rcu_read_lock();
	css_for_each_descendant_pre(css, &top->css) {
		struct tg_concurrency *conc = &css_tg(css)->conc;

		guard(raw_spinlock_irqsave)(&conc->lock);
		conc->bounds_pending = false;
	}
	rcu_read_unlock();

	conc_update_mm_bounds(top);

	rcu_read_lock();
	css_for_each_descendant_pre(css, &top->css) {
		struct tg_concurrency *conc = &css_tg(css)->conc;

		guard(raw_spinlock_irqsave)(&conc->lock);
		if (!conc->bounds_pending)
			conc->shown_floor = 0;
	}
	rcu_read_unlock();
}

/*
 * Remove the (@tg, @cpu) slot if it is idle through the whole hierarchy.
 * Drain the child groups that use the CPU first: their slots are entities
 * of ours. Return -EBUSY when a task is present.
 *
 * Take the locks one group at a time, never a child's under a parent's.
 */
static int __conc_drain_cpu(struct task_group *tg, int cpu, bool up)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, cpu);
	struct task_group *child;
	unsigned long flags;
	int ret = 0, v;

	lockdep_assert_held(&conc_mutex);

	rcu_read_lock();
	list_for_each_entry_rcu(child, &tg->children, siblings) {
		if (!cpumask_test_cpu(cpu, child->conc.used))
			continue;
		/*
		 * A dying child is drained as any other: its slot is an entity
		 * of ours. What remains is reset when the child goes offline.
		 */
		if (!css_tryget(&child->css))
			continue;
		rcu_read_unlock();
		ret = __conc_drain_cpu(child, cpu, false);
		rcu_read_lock();
		/* The reference pins the list entry, see sched_release_group(). */
		css_put(&child->css);
		if (ret)
			break;
	}
	rcu_read_unlock();
	if (ret)
		return ret;

	raw_spin_lock_irqsave(&conc->lock, flags);
	v = atomic_read(slot);
	if (v == CONC_SLOT_UNUSED) {
		raw_spin_unlock_irqrestore(&conc->lock, flags);
		return 0;
	}
	/* Only an idle slot can be drained. A lockless claim may come first. */
	if (v != 0 || atomic_cmpxchg(slot, 0, CONC_SLOT_DRAINING) != 0) {
		raw_spin_unlock_irqrestore(&conc->lock, flags);
		return -EBUSY;
	}
	raw_spin_unlock_irqrestore(&conc->lock, flags);

	conc_slot_removed(tg, cpu);

	raw_spin_lock_irqsave(&conc->lock, flags);
	if (atomic_read(slot) == CONC_SLOT_DRAINING)
		__conc_remove_slot(tg, cpu);
	else
		ret = -EBUSY;	/* a forced claim revived it */
	raw_spin_unlock_irqrestore(&conc->lock, flags);
	/*
	 * This group was the last entity of the parent's slot. That slot is
	 * idle now. Drain it too, up the chain.
	 */
	if (!ret && up && conc->propagate && tg->parent &&
	    atomic_read(per_cpu_ptr(tg->parent->conc.present, cpu)) == 0)
		__conc_drain_cpu(tg->parent, cpu, true);
	return ret;
}

/*
 * Remove the idle (@tg, @cpu) slot, with the idle child slots it counts
 * and the ancestor slots it was the last entity of.
 */
static int conc_drain_cpu(struct task_group *tg, int cpu)
{
	return __conc_drain_cpu(tg, cpu, true);
}

/*
 * Remove idle slots of a group at its limit until a new slot fits. The
 * next task that needs a new CPU then finds room. Also apply the
 * concurrency bounds after the reported value dropped, see
 * conc_used_del().
 */
static void conc_cleaner_work(struct work_struct *work)
{
	struct task_group *tg = container_of(work, struct task_group, conc.work);
	struct tg_concurrency *conc = &tg->conc;
	cpumask_var_t cpus;
	bool done = false;
	int cpu, pass;

	if (!alloc_cpumask_var(&cpus, GFP_KERNEL))
		return;

	mutex_lock(&conc_mutex);
	if (!conc->active || !conc->effective || READ_ONCE(conc->nr_used) < conc->effective)
		goto out;
	cpumask_copy(cpus, conc->used);

	/*
	 * Three passes, until a new slot fits:
	 *  0. idle slots that share an SMT core with another used CPU;
	 *  1. the other idle slots;
	 *  2. slots whose only entities are idle child groups.
	 */
	for (pass = 0; pass < 3 && !done; pass++) {
		for_each_cpu(cpu, cpus) {
			int v = atomic_read(per_cpu_ptr(conc->present, cpu));

			if (pass == 2 ? v <= 0 : v != 0)
				continue;
			if (!pass && cpumask_weight_and(cpu_smt_mask(cpu), conc->used) < 2)
				continue;
			if (!conc_drain_cpu(tg, cpu) &&
			    READ_ONCE(conc->nr_used) < conc->effective) {
				done = true;
				break;
			}
		}
	}
out:
	/*
	 * The reported value dropped, see conc_used_del(). Apply the bounds of
	 * the concurrency IDs first. Then notify the readers of the effective
	 * file.
	 */
	if (READ_ONCE(conc->bounds_pending)) {
		conc_bounds_apply(tg);
		conc_notify_tree(tg);
	}
	mutex_unlock(&conc_mutex);
	free_cpumask_var(cpus);
}

/*
 * Configuration: cpu.concurrency.max and cpu.concurrency.effective
 */

/*
 * @tg stops counting. Release the slot of each of its tasks and clear its
 * accounted flag. A task is accounted only while its group counts.
 */
static void conc_deactivate_tasks(struct task_group *tg)
{
	struct css_task_iter it;
	struct task_struct *t;

	css_task_iter_start(&tg->css, 0, &it);
	while ((t = css_task_iter_next(&it))) {
		guard(task_rq_lock)(t);

		if (!t->conc_accounted || task_group(t) != tg)
			continue;
		t->conc_accounted = 0;
		conc_release(tg, task_cpu(t));
	}
	css_task_iter_end(&it);
}

/*
 * @tg starts counting. Claim a slot for each of its tasks which is present
 * and not accounted. Skip a task which a concurrent claim already
 * accounted. Ask for a hold of a task which the limit refuses: it is held
 * at its next return to user space.
 */
static void conc_activate_tasks(struct task_group *tg)
{
	struct css_task_iter it;
	struct task_struct *t;

	css_task_iter_start(&tg->css, 0, &it);
	while ((t = css_task_iter_next(&it))) {
		guard(task_rq_lock)(t);

		if (t->conc_accounted || t->conc_held || task_group(t) != tg)
			continue;
		/* Present: queued, or in flight between selection and enqueue. */
		if (!task_on_rq_queued(t) && READ_ONCE(t->__state) != TASK_WAKING)
			continue;
		switch (conc_claim(tg, task_cpu(t), false, t)) {
		case CONC_CLAIMED:
			t->conc_accounted = 1;
			break;
		case CONC_REFUSED:
			conc_hold_request(t, CONC_HOLD_UNPLACED);
			break;
		default:
			break;
		}
	}
	css_task_iter_end(&it);
}

/*
 * Return the limit enforced on @tg now, 0 for none. While a shrink is
 * pending, return the number of CPUs still used if it is larger.
 */
static __maybe_unused unsigned int conc_effective_report(struct task_group *tg)
{
	struct tg_concurrency *conc = &tg->conc;
	unsigned int eff = READ_ONCE(conc->effective);

	if (eff && READ_ONCE(conc->shrink_pending))
		eff = max(eff, READ_ONCE(conc->nr_used));
	return eff;
}

/*
 * Return the value which cpu.concurrency.effective shows for @tg, 0 for
 * "max". This is the limit enforced now, or the floor recorded by
 * conc_report_drop() if that is larger.
 */
static unsigned int conc_effective_shown(struct task_group *tg)
{
	struct tg_concurrency *conc = &tg->conc;
	unsigned int val;

	guard(raw_spinlock_irqsave)(&conc->lock);
	val = max(conc_report_ceiling(conc), conc->shown_floor);
	return val == UINT_MAX ? 0 : val;
}

/*
 * Store in @out the groups of the subtree of @top in pre-order, each with
 * a reference, and their number in @nr. Return 0, or -ENOMEM.
 *
 * This is for the walks which allocate, or which take the locks of every
 * task. css_for_each_descendant_pre() needs RCU. Without the snapshot, the
 * configuration paths would hold RCU across every task of the subtree and
 * allocate with GFP_ATOMIC.
 *
 * A css which css_tryget() refuses is being destroyed and has no task
 * left: its entry is NULL. Dying groups are listed, as the RCU walk lists
 * them. The callers skip them.
 */
static int conc_subtree_snapshot(struct task_group *top, struct cgroup_subsys_state ***out,
				 int *nr)
{
	struct cgroup_subsys_state **arr = NULL, *css;
	int n, i, cap = 0;

again:
	n = 0;
	rcu_read_lock();
	css_for_each_descendant_pre(css, &top->css) {
		if (n < cap && css_tryget(css))
			arr[n] = css;
		else if (n < cap)
			arr[n] = NULL;
		n++;
	}
	rcu_read_unlock();
	if (n > cap) {
		/* The first pass, or the subtree grew meanwhile: once more. */
		for (i = 0; i < cap; i++) {
			if (arr[i])
				css_put(arr[i]);
		}
		kvfree(arr);
		cap = n + 8;
		arr = kvcalloc(cap, sizeof(*arr), GFP_KERNEL);
		if (!arr)
			return -ENOMEM;
		goto again;
	}
	*out = arr;
	*nr = n;
	return 0;
}

static void conc_subtree_release(struct cgroup_subsys_state **arr, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (arr[i])
			css_put(arr[i]);
	}
	kvfree(arr);
}

/*
 * @tg is about to stop counting. First stop each child from propagating
 * its slots into @tg, and release the entities the child holds there. A
 * claim by a child which finds @tg inactive then also finds its own
 * propagation stopped, see conc_claim_slow(). A child which counts on its
 * own keeps counting.
 */
static void conc_detach_children(struct task_group *tg, struct cpumask *scratch)
{
	struct task_group *child;
	unsigned long flags;
	bool was;
	int cpu;

	/* A child removed meanwhile stays readable until the grace period. */
	rcu_read_lock();
	list_for_each_entry_rcu(child, &tg->children, siblings) {
		struct tg_concurrency *conc = &child->conc;

		raw_spin_lock_irqsave(&conc->lock, flags);
		was = conc->propagate;
		if (was) {
			cpumask_copy(scratch, conc->used);
			conc->propagate = false;
		}
		raw_spin_unlock_irqrestore(&conc->lock, flags);
		if (!was)
			continue;
		for_each_cpu(cpu, scratch)
			conc_release(tg, cpu);
	}
	rcu_read_unlock();
}

/*
 * Recompute the effective limit and the effective ceiling of every group
 * in the subtree of @top. A value of 0 means none:
 *
 *   limit   = min(request, limit(parent))
 *   ceiling = min(ceiling, ceiling(parent))
 *   limit   = min(limit, ceiling)
 *
 * A ceiling without a limit is enforced as a limit. Processes size per-ID
 * data by the ceiling, so the IDs must stay below it.
 *
 * A group counts when it has a limit or a ceiling, or when its parent
 * counts. Switch counting on or off to match. Walk in pre-order, so that a
 * parent's state is final when its children are visited. Shrinking is
 * left to the caller.
 */
static __maybe_unused void conc_update_tree(struct task_group *top, struct cpumask *scratch)
{
	struct cgroup_subsys_state **csses, *css;
	int nr_css, k;

	lockdep_assert_held(&conc_mutex);

	/*
	 * This walk visits every task and may sleep. So it runs on a
	 * referenced snapshot of the subtree, not under RCU. A group created
	 * meanwhile is walked once it is linked in.
	 */
	if (conc_subtree_snapshot(top, &csses, &nr_css)) {
		pr_warn_once("conc: no memory to update the limits of css %d\n", top->css.id);
		return;
	}
	for (k = 0; k < nr_css; k++) {
		bool should_active, new_prop, walk = false, teardown = false;
		struct task_group *tg, *parent;
		unsigned int eff, ceiling, was;
		struct tg_concurrency *conc;
		unsigned long flags;
		int cpu, attach = 0;

		css = csses[k];
		if (!css)
			continue;
		tg = css_tg(css);
		parent = READ_ONCE(tg->parent);
		conc = &tg->conc;
		eff = conc->request;
		ceiling = conc->ceiling;

		/*
		 * Skip a group which is not linked in yet, see
		 * sched_online_group(). It is walked once it is linked in, and
		 * no task can join it before. Skip a dying group too. No task
		 * can join it either, and it is reset when it goes offline.
		 */
		if (!parent || css_is_dying(css))
			continue;

		if (parent->conc.effective && (!eff || eff > parent->conc.effective))
			eff = parent->conc.effective;
		if (parent->conc.ceiling_effective &&
		    (!ceiling || ceiling > parent->conc.ceiling_effective))
			ceiling = parent->conc.ceiling_effective;
		/* A limit inherited from the parent can be above the ceiling. */
		if (ceiling && (!eff || eff > ceiling))
			eff = ceiling;
		should_active = eff || parent->conc.active;
		new_prop = should_active && parent->conc.active;
		if (!should_active && conc->active)
			conc_detach_children(tg, scratch);

		raw_spin_lock_irqsave(&conc->lock, flags);
		was = conc_report_ceiling(conc);
		conc->effective = eff;
		conc->ceiling_effective = ceiling;
		if (should_active && !conc->active) {
			/* Its state is in place, see conc_active(). */
			smp_store_release(&conc->active, true);
			walk = true;
		} else if (!should_active && conc->active) {
			WRITE_ONCE(conc->active, false);
			teardown = true;
		}
		if (conc->propagate != new_prop) {
			/*
			 * Attach to the parent, or detach from it. Copy the
			 * used CPUs in the same critical section as the flag
			 * change. A slot added concurrently then either
			 * propagates itself or is in the copy. A slot cannot
			 * be removed: that needs conc_mutex. Claim or release
			 * in the parent below, with this lock dropped.
			 */
			cpumask_copy(scratch, conc->used);
			conc->propagate = new_prop;
			attach = new_prop ? 1 : -1;
		}
		conc->shrink_pending = eff && conc->nr_used > eff;
		/* The caller applies the bounds, see conc_bounds_apply(). */
		if (conc_report_ceiling(conc) < was)
			conc_report_drop(conc, was);
		raw_spin_unlock_irqrestore(&conc->lock, flags);

		if (attach) {
			for_each_cpu(cpu, scratch) {
				if (attach > 0)
					conc_claim(parent, cpu, true, NULL);
				else
					conc_release(parent, cpu);
			}
		}

		if (walk)
			conc_activate_tasks(tg);
		if (teardown)
			conc_deactivate_tasks(tg);
	}
	conc_subtree_release(csses, nr_css);
}

/*
 * Notify cpu.concurrency.effective of each group whose shown value or
 * effective ceiling changed.
 */
static void conc_notify_tree(struct task_group *top)
{
	struct cgroup_subsys_state *css;

	rcu_read_lock();
	css_for_each_descendant_pre(css, &top->css) {
		struct task_group *tg = css_tg(css);
		unsigned int val = conc_effective_shown(tg), ceiling = tg->conc.ceiling_effective;

		if (val != tg->conc.reported || ceiling != tg->conc.reported_ceiling) {
			tg->conc.reported = val;
			tg->conc.reported_ceiling = ceiling;
			cgroup_file_notify(&tg->conc.effective_file);
		}
	}
	rcu_read_unlock();
}

/*
 * Return true if the limit never changes the mask of @t on its own. This
 * holds for kernel threads, for tasks whose affinity cannot be set, and
 * for deadline tasks. The bandwidth of a deadline task is admitted per
 * root domain. It would have to be checked again for a mask which reaches
 * another root domain, see dl_task_check_affinity().
 */
static inline bool conc_task_mask_fixed(struct task_struct *t)
{
	return t->flags & (PF_KTHREAD | PF_NO_SETAFFINITY) || dl_task(t);
}

static __maybe_unused int conc_resv_cpu(struct conc_resv *resv)
{
	return resv->cpu;
}

#ifndef CONFIG_SCHED_MAX_CONCURRENCY_DEBUG
static inline void conc_debug_hold_start(struct task_struct *p) { }

static inline void conc_debug_hold_stop(struct task_struct *p) { }

static inline void conc_debug_hold_end(struct task_struct *p, const char *how) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY_DEBUG */

/*
 * @p is current. Take the IDs which it did not take while a hold was
 * pending. These IDs are given outside a context switch. The exit to user
 * space writes changed IDs only after a context switch, and clears the
 * change otherwise. So force the rseq update here.
 */
static __maybe_unused void conc_cid_refresh(struct task_struct *p)
{
#ifdef CONFIG_SCHED_MM_CID
	mm_cid_schedin(p);
	rseq_force_update();
#endif
}

/* Return the group of @p with a reference, or NULL if it does not count. */
static __maybe_unused struct task_group *conc_pin_task_group(struct task_struct *p)
{
	struct task_group *tg;

	if (!static_branch_unlikely(&sched_conc_enabled))
		return NULL;
	guard(rcu)();
	tg = READ_ONCE(p->sched_task_group);
	if (!conc_active(&tg->conc) || !css_tryget_online(&tg->css))
		return NULL;
	return tg;
}

#ifdef CONFIG_SCHED_MM_CID
/*
 * Glue between the cgroup concurrency limit and the cid domains.
 *
 * The bound of the domain of an mm comes from the lowest common ancestor
 * of the groups of its users. If a counting group is at or above it, the
 * bound is the effective limit of the innermost such group. This is the
 * usual case for the threads of a process. Otherwise the bound is the sum
 * over the topmost counting groups which cover the users, see
 * __conc_dom_bound().
 */
static unsigned int conc_tg_depth(struct task_group *tg)
{
	unsigned int d = 0;

	for (; tg->parent; tg = tg->parent)
		d++;
	return d;
}

static struct task_group *conc_tg_lca(struct task_group *a, struct task_group *b)
{
	unsigned int da = conc_tg_depth(a), db = conc_tg_depth(b);

	for (; da > db; da--)
		a = a->parent;
	for (; db > da; db--)
		b = b->parent;
	while (a != b) {
		a = a->parent;
		b = b->parent;
	}
	return a;
}

static bool conc_tg_is_ancestor(struct task_group *anc, struct task_group *tg)
{
	for (; tg; tg = tg->parent) {
		if (tg == anc)
			return true;
	}
	return false;
}

/*
 * Return the innermost counting group at or above the common ancestor of
 * @dom, or NULL.
 */
static struct task_group *conc_dom_group(struct cid_domain *dom)
{
	struct task_group *tg;

	for (tg = dom->lca; tg; tg = tg->parent) {
		if (conc_active(&tg->conc))
			return tg;
	}
	return NULL;
}

/*
 * Return the bound of @dom, 0 for none.
 *
 * Take the lowest common ancestor of the groups of the users. If a
 * counting group is at or above it, the bound is the effective limit of
 * the innermost such group.
 *
 * Otherwise the users span several counting subtrees, or some are in none.
 * The bound is then the sum of the effective limits of the topmost
 * counting groups which cover the users. Each one bounds the presence of
 * its own subtree. There is no bound if a user is covered by no counting
 * group, or by one with no limit.
 *
 * Call with dom::lock held when the domain has users.
 */
static unsigned int __conc_dom_bound(struct cid_domain *dom)
{
	struct task_group *tg = conc_dom_group(dom), *stack_tops[8], **tops = stack_tops;
	struct cid_user *u;
	unsigned int sum = 0, eff, cap = ARRAY_SIZE(stack_tops);
	int n = 0, i;

	/*
	 * Use the limit enforced so far, see conc_effective_report(). A
	 * decrease reaches the IDs once the group is within the new limit,
	 * not while its excess is still being evicted.
	 */
	if (tg)
		return conc_effective_report(tg);
	if (!dom->users)
		return 0;
	lockdep_assert_held(&dom->lock);
	if (dom->nr_tops > cap) {
		tops = dom->tops;
		cap = dom->nr_tops;
	}
	hlist_for_each_entry(u, &dom->user_list, node) {
		struct task_struct *t = cid_user_task(dom, u);
		struct task_group *g = READ_ONCE(t->sched_task_group), *top = NULL;

		for (; g && conc_active(&g->conc); g = g->parent)
			top = g;
		if (!top)
			return 0;
		for (i = 0; i < n; i++) {
			if (tops[i] == top)
				break;
		}
		if (i < n)
			continue;
		if (n == cap) {
			/*
			 * There are more topmost groups than the buffer
			 * holds, and it cannot grow under this raw lock.
			 * Keep the bound as it is. cid_apply_bound() grows
			 * the buffer and computes the bound again.
			 */
			/*
			 * If that size was asked for already, its
			 * allocation failed, see conc_dom_grow_tops(). Do
			 * not ask again from here: the update would queue
			 * itself for as long as memory is short. The next
			 * update of the bound tries again.
			 */
			if (dom->tops_wanted == cap * 2)
				return dom->bound;
			dom->tops_wanted = cap * 2;
			if (!dom->bound_update) {
				dom->bound_update = 1;
				irq_work_queue(&dom->irq_work);
			}
			return dom->bound;
		}
		tops[n++] = top;
		eff = conc_effective_report(top);
		/*
		 * A topmost group with no limit counts under a ceiling. Its
		 * users can run anywhere, so there is no bound, as for a user
		 * in no group. Adding 0 for it would keep the sum of the other
		 * groups in force over users which the sum does not cover.
		 */
		if (!eff)
			return 0;
		sum += eff;
	}
	return sum;
}

unsigned int conc_dom_bound(struct cid_domain *dom)
{
	return __conc_dom_bound(dom);
}

/*
 * Grow the buffer to the size which the scan in __conc_dom_bound() asked
 * for. Call in process context, with dom::mutex held.
 */
void conc_dom_grow_tops(struct cid_domain *dom)
{
	struct task_group **tops, **old;
	unsigned int want = READ_ONCE(dom->tops_wanted);

	lockdep_assert_held(&dom->mutex);
	if (want <= dom->nr_tops)
		return;
	tops = kvmalloc_array(want, sizeof(*tops), GFP_KERNEL);
	if (!tops)
		return;
	scoped_guard(raw_spinlock_irq, &dom->lock) {
		old = dom->tops;
		dom->tops = tops;
		dom->nr_tops = want;
	}
	kvfree(old);
}

/* The domain goes away. */
void conc_dom_destroy(struct cid_domain *dom)
{
	kvfree(dom->tops);
	dom->tops = NULL;
	dom->nr_tops = 0;
}

void conc_dom_init(struct cid_domain *dom, struct task_struct *p)
{
	dom->lca = p ? task_group(p) : NULL;
	dom->bound = conc_dom_bound(dom);
}

/*
 * A user is about to be added. The first user of an mm, at fork(2) or
 * execve(2), sets the common ancestor to its own group. The task may have
 * been placed in another cgroup than its parent. A later user can only
 * widen the ancestor, and the bound can only widen with it, so no fixup is
 * needed. The bound is computed once the user is linked, see
 * cid_add_user().
 */
void conc_dom_fork(struct cid_domain *dom, struct task_struct *t)
{
	struct task_group *tg = task_group(t);

	lockdep_assert_held(&dom->lock);
	if (!dom->users)
		dom->lca = tg;
	else if (!dom->lca || !conc_tg_is_ancestor(dom->lca, tg))
		dom->lca = dom->lca ? conc_tg_lca(dom->lca, tg) : tg;
}

/*
 * Recompute the common ancestor of all users. The task groups are read
 * without a lock.
 */
static void conc_dom_recompute_lca(struct cid_domain *dom)
{
	struct task_group *lca = NULL;
	struct cid_user *u;

	lockdep_assert_held(&dom->lock);
	hlist_for_each_entry(u, &dom->user_list, node) {
		struct task_group *tg = READ_ONCE(cid_user_task(dom, u)->sched_task_group);

		lca = lca ? conc_tg_lca(lca, tg) : tg;
	}
	if (lca)
		dom->lca = lca;
}

/*
 * Called at the schedule out of @prev, when this CPU owns the ID of @dom.
 * Drop the ID if the slot of the group of @prev on this CPU counts no
 * task. Otherwise the ID would stay on a CPU which the group may give up.
 * No user of the domain would be left in the group to find it there. The
 * next user on this CPU allocates again. Call under the runqueue lock.
 */
void conc_cid_left_cpu(struct cid_domain *dom, struct task_struct *prev)
{
	struct task_group *tg;

	if (!conc_task_active(prev))
		return;
	tg = task_group(prev);
	if (atomic_read(per_cpu_ptr(tg->conc.present, smp_processor_id())) <= 0)
		cid_drop_on_cpu(dom, this_cpu_ptr(dom->pcpu));
}

void conc_dom_user_exited(struct cid_domain *dom, struct task_struct *t)
{
	struct task_group *old = dom->lca;

	lockdep_assert_held(&dom->lock);
	/*
	 * This CPU owns the ID. The exiting task cannot drop it at its last
	 * schedule out: its mm is gone then, see conc_cid_left_cpu(). Drop
	 * it now if the domain is bounded. A user queued on this CPU
	 * allocates again.
	 */
	if (t == current && cid_on_cpu(dom->mode) && conc_dom_group(dom))
		cid_drop_on_cpu(dom, this_cpu_ptr(dom->pcpu));
	if (!dom->users || task_group(t) == old)
		return;
	conc_dom_recompute_lca(dom);
	if (dom->lca == old || dom->bound_update)
		return;
	dom->bound_update = 1;
	irq_work_queue(&dom->irq_work);
}

/*
 * A task of @dom changed group. Recompute the common ancestor and defer
 * the bound update to the work of the domain: the caller may hold a
 * runqueue lock.
 */
static void conc_dom_task_moved(struct cid_domain *dom)
{
	guard(raw_spinlock)(&dom->lock);
	if (!dom->users)
		return;
	conc_dom_recompute_lca(dom);
	if (dom->bound_update)
		return;
	dom->bound_update = 1;
	irq_work_queue(&dom->irq_work);
}

void conc_mm_task_moved(struct task_struct *p)
{
	/*
	 * Read p->mm once: @p can exit meanwhile. The caller holds the
	 * runqueue lock of @p. So @p has not done its last schedule, and it
	 * still pins the mm it read here as its active_mm.
	 */
	struct mm_struct *mm = READ_ONCE(p->mm);

	if (mm)
		conc_dom_task_moved(&mm->mm_cid);
}

/*
 * Drop the CPU owned CIDs which are at or above max_cids. Drop also those
 * cached on a CPU outside the used set of the group of the domain. Call
 * with dom::mutex held and no transition in progress.
 */
void conc_dom_drop_stale_cids(struct cid_domain *dom)
{
	unsigned int max_cids = READ_ONCE(dom->max_cids);
	struct task_group *tg = conc_dom_group(dom);
	int cpu;

	lockdep_assert_held(&dom->mutex);

	for_each_possible_cpu(cpu) {
		struct cid_pcpu *pcp = per_cpu_ptr(dom->pcpu, cpu);
		struct rq *rq = cpu_rq(cpu);

		/* Nothing cached: the lock would find nothing to drop. */
		if (!cid_on_cpu(READ_ONCE(pcp->cid)))
			continue;
		/* Remote access to the per-CPU storage requires rq_lock */
		guard(rq_lock_irq)(rq);
		if (!cid_on_cpu(pcp->cid))
			continue;
		if (cid_task_in_domain(rq->curr, dom) && task_on_rq_queued(rq->curr))
			continue;
		if (cpu_cid_to_cid(pcp->cid) >= max_cids ||
		    (tg && !cpumask_test_cpu(cpu, tg->conc.used)))
			cid_drop_on_cpu(dom, pcp);
	}
}

/*
 * Refresh the bound of @dom. Call after a limit write, a retry or a cgroup
 * migration changed effective values or the group of the domain. Even if
 * the bound did not change, a migration can leave IDs cached on CPUs which
 * the new group does not use. The old group drains those slots later and
 * does not find the domain. So drop the stale IDs in that case too.
 */
static void conc_dom_update_bound(struct cid_domain *dom)
{
	bool changed;

	guard(mutex)(&dom->mutex);
	if (!dom->users)
		return;
	scoped_guard(raw_spinlock_irq, &dom->lock) {
		/*
		 * The scan may ask for a deferred update itself. Read
		 * the flag after the scan.
		 */
		unsigned int bound = conc_dom_bound(dom);

		changed = dom->bound_update || dom->bound != bound;
	}
	if (changed)
		cid_apply_bound(dom);
	else
		conc_dom_drop_stale_cids(dom);
}

/*
 * Take conc_mutex: a bound is applied between configuration walks only.
 *
 * In the middle of a walk, a group counts before its present tasks have a
 * slot or a hold, see conc_activate_tasks(). conc_dom_drop_stale_cids()
 * would take such a task for the owner of the ID of its CPU, and keep an
 * ID which the bound requires to drop.
 *
 * The walks hold conc_mutex and apply the bounds themselves when they are
 * over, see conc_bounds_apply(). A deferred update and the refresh of a
 * migration take conc_mutex too.
 */
void conc_bounds_lock(void)
{
	mutex_lock(&conc_mutex);
}

void conc_bounds_unlock(void)
{
	mutex_unlock(&conc_mutex);
}
#endif /* CONFIG_SCHED_MM_CID */
