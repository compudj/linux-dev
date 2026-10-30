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
static __maybe_unused DEFINE_PER_CPU(cpumask_var_t, conc_select_mask);

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

static void conc_notify_tree(struct task_group *top);

atomic_t conc_nr_held;

static inline bool conc_cpu_usable(int cpu)
{
	return cpu_active(cpu) && !cpumask_test_cpu(cpu, &conc_offlining_cpus);
}

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

static inline bool conc_tg_has_headroom(struct task_group *tg)
{
	return !tg->conc.effective || READ_ONCE(tg->conc.nr_used) < tg->conc.effective;
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

static __maybe_unused int conc_resv_cpu(struct conc_resv *resv)
{
	return resv->cpu;
}

#ifndef CONFIG_SCHED_MAX_CONCURRENCY_DEBUG
static inline void conc_debug_hold_start(struct task_struct *p) { }

static inline void conc_debug_hold_stop(struct task_struct *p) { }

static inline void conc_debug_hold_end(struct task_struct *p, const char *how) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY_DEBUG */

