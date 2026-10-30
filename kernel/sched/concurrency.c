// SPDX-License-Identifier: GPL-2.0

/*
 * cpu.concurrency.max: a limit on the CPUs a cgroup uses at once, see
 * Documentation/scheduler/sched-max-concurrency.rst. The hooks the
 * scheduler calls are inline in concurrency.h; the bounds the limit puts
 * on the concurrency IDs are at the end, with the IPC-namespace admission.
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

/* The active groups from @p's up: the most levels whose lists cover @p. */
#define for_each_conc_level(lvl, p)						\
	for (lvl = task_group(p); READ_ONCE(lvl->conc.active); lvl = lvl->parent)
/* The active groups from @tg up. */
#define for_each_conc_level_from(lvl, tg)						\
	for (lvl = (tg); READ_ONCE(lvl->conc.active); lvl = lvl->parent)

#define CONC_EVICT_ROUNDS	10
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
	int			cpu;
	struct conc_sm_ancestor	*anc;
	int			nr_anc;
};

/*
 * Globals and forward declarations
 */

/*
 * Concurrency accounting for cpu.concurrency.max, see
 * Documentation/scheduler/sched-max-concurrency.rst
 *
 * Lock order: cpuset_mutex -> conc_mutex -> tg->conc.lock. Group locks nest
 * only from a child to its
 * parent, when the parent merges the child's classes (conc_classes_merge());
 * a claim never nests them (see conc_claim_slow()). tg->conc.lock nests inside
 * p->pi_lock and rq->lock, never the other way.
 */
DEFINE_STATIC_KEY_FALSE(sched_conc_enabled);
static DECLARE_SWAIT_QUEUE_HEAD(conc_held_wq);	/* woken under rq locks */
DEFINE_PER_CPU(const struct cpumask *, conc_select_cpus);
static DEFINE_PER_CPU(cpumask_var_t, conc_select_mask);
static DEFINE_PER_CPU(cpumask_var_t, conc_entitled_mask);
static DEFINE_PER_CPU(cpumask_var_t, conc_unhit_mask);
static DEFINE_PER_CPU(cpumask_var_t, conc_class_scratch);

/* Serializes configuration: limit writes, activation, eviction and retries. */
static DEFINE_MUTEX(conc_mutex);

/*
 * CPUs going offline, from conc_cpu_deactivate() to conc_cpu_offline(): no
 * group adds a slot there (unless forced), and no destination is chosen
 * there, while the CPU is still active for the presences being moved off
 * it and the late arrivals that follow them.
 */
static struct cpumask conc_offlining_cpus __read_mostly;

/*
 * Held by a set-move stopper from its count check to its relabel, and by
 * a late arrival claiming an evicting slot: the claim either lands before
 * the check and is moved, or finds the slot gone and selects again.
 * Nests outside tg->conc.lock.
 */
static DEFINE_RAW_SPINLOCK(conc_sm_lock);
static int conc_subtree_snapshot(struct task_group *top, struct cgroup_subsys_state ***out, int *nr);
static void conc_subtree_release(struct cgroup_subsys_state **arr, int n);

static void conc_irq_work(struct irq_work *work);
static void conc_cleaner_work(struct work_struct *work);
static int conc_resv_cpu(struct conc_resv *resv);

static void conc_classes_drop(struct tg_concurrency *conc);

static inline bool conc_task_mask_fixed(struct task_struct *t);
static void conc_task_entitled(struct task_struct *t, struct cpumask *mask);

#ifdef CONFIG_SCHED_MM_CID
/*
 * The domains of a cgroup migration in flight, from can_attach() to the
 * end of attach(). Their threads leave the source groups' css_sets before
 * the cpu controller switches their task_group, so a slot of a source
 * group drained in that window finds no task of theirs; their IDs on the
 * CPU are dropped all the same. Under conc_mutex.
 */
static struct cid_domain **conc_attach_doms;
static int conc_nr_attach_doms;

static void conc_dom_update_bound(struct cid_domain *dom);

/*
 * Before the threads of a migration leave their groups: the IDs their
 * domains cached on CPUs where the source group has an idle slot, or
 * none, would be orphaned, since the group drains those slots later
 * without finding the threads there. Drop them now; a thread back on such
 * a CPU allocates again. A slot with tasks, or in a transient state,
 * keeps its ID.
 */
static struct task_group *conc_pin_task_group(struct task_struct *p);
#endif /* CONFIG_SCHED_MM_CID */

static void conc_notify_tree(struct task_group *top);

static unsigned int conc_class_cap(struct tg_concurrency *conc);

static int conc_evict_cpu(struct task_group *tg, int cpu, bool set_move, bool hold,
			struct task_struct *mask_task, const struct cpumask *mask,
			const struct cpumask *within);
static int conc_hold_cpu(struct task_group *tg, int cpu);
static struct task_group *conc_pin_task_group(struct task_struct *p);
static bool conc_make_headroom(struct task_group *tg, int cpu);
static bool conc_needs_retry(struct task_group *tg);

/*
 * Holding: a task that finds no CPU within its group's bound never runs
 * in excess of it. For the kernel-mode part of its wakeup, migration or
 * group change, where it may hold locks the repair itself needs
 * (conc_mutex among them), it goes without a slot, takes no concurrency ID
 * meanwhile (see conc_task_unplaced()), and is held off the CPUs at its
 * next return to user space, the way the per-task CFS bandwidth throttle
 * parks tasks: dequeued from its class in TASK_RUNNING, still queued as
 * far as the core is concerned (p->on_rq, PSI, uclamp, the load it
 * migrates with), listed on conc_held_list, until a slot of its mask
 * frees. Its class saw it go to sleep, and a sleep followed by a wakeup
 * is all the class ever sees of a hold: a held task that blocks becomes
 * a plain sleeper (conc_held_blocks()), and anything else the core does
 * with it (a migration, a change of class, priority or group) starts by
 * returning it to its class where it is (conc_unhold()) and asks for the
 * hold again. Kernel threads never return to user space and are never
 * held; they run on without a slot. The same work holds the tasks of a slot being emptied
 * by conc_hold_cpu() (CONC_HOLD_EVICT). SIGKILL ends a hold, or keeps one
 * from starting: the task goes to do_exit() with what it has, a slot or
 * none, its hold left pending so that it takes no ID on the way (see
 * conc_fatal_signal()).
 *
 * Placement is a selection as for a wakeup, from conc_hold_work_fn(), run
 * after every event that can make room: a slot that empties or goes, a
 * reservation settled, a set-move, a retry, a change of the held task's
 * mask or group, hotplug. The selection of a held task refuses to go
 * without a slot and leaves it held when nothing within the bound fits.
 */
static LIST_HEAD(conc_held_list);
static DEFINE_RAW_SPINLOCK(conc_hold_lock);
atomic_t conc_nr_held;
static void conc_hold_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(conc_hold_work, conc_hold_work_fn);

static void conc_hold_irq_work_fn(struct irq_work *work);
static struct irq_work conc_hold_irq_work = IRQ_WORK_INIT_HARD(conc_hold_irq_work_fn);

static inline bool conc_cpu_usable(int cpu)
{
	return cpu_active(cpu) && !cpumask_test_cpu(cpu, &conc_offlining_cpus);
}

#ifndef CONFIG_SCHED_MM_CID
void conc_mm_task_moved(struct task_struct *p)
{
	if (p->mm)
		conc_dom_task_moved(&p->mm->mm_cid);
}
#endif /* !CONFIG_SCHED_MM_CID */

int alloc_conc_sched_group(struct task_group *tg)
{
	struct tg_concurrency *conc = &tg->conc;

	raw_spin_lock_init(&conc->lock);
	INIT_LIST_HEAD(&conc->masks);
	INIT_LIST_HEAD(&conc->onlining);
	conc->irq_work = IRQ_WORK_INIT_HARD(conc_irq_work);
	INIT_WORK(&conc->work, conc_cleaner_work);
	return 1;
}

/*
 * Also on the failure path of sched_create_group(), which may have failed
 * before alloc_conc_sched_group(): nothing was set up then.
 */
void free_conc_sched_group(struct task_group *tg)
{
	if (!tg->conc.present)
		return;
	conc_classes_drop(&tg->conc);
	free_percpu(tg->conc.present);
	free_cpumask_var(tg->conc.used);
	free_cpumask_var(tg->conc.unused_cores);
}

void __init conc_init_root(void)
{
	int __maybe_unused i;

	/* The root group is never limited and never counts. */
	raw_spin_lock_init(&root_task_group.conc.lock);
	INIT_LIST_HEAD(&root_task_group.conc.masks);
	/* Unchecked, as sched_init() allocates its per-CPU masks: a failure at boot is fatal. */
#ifdef CONFIG_CPUMASK_OFFSTACK
	for_each_possible_cpu(i) {
		per_cpu(conc_select_mask, i) = (cpumask_var_t)kzalloc_node(
			cpumask_size(), GFP_KERNEL, cpu_to_node(i));
		per_cpu(conc_entitled_mask, i) = (cpumask_var_t)kzalloc_node(
			cpumask_size(), GFP_KERNEL, cpu_to_node(i));
		per_cpu(conc_unhit_mask, i) = (cpumask_var_t)kzalloc_node(
			cpumask_size(), GFP_KERNEL, cpu_to_node(i));
		per_cpu(conc_class_scratch, i) = (cpumask_var_t)kzalloc_node(
			cpumask_size(), GFP_KERNEL, cpu_to_node(i));
	}
#endif
}

/*
 * Slots go one per SMT core first. The siblings of a core share its
 * execution resources: a slot added on the unused sibling of a used CPU,
 * while a core of which no CPU is used was there to take, leaves the group
 * running on less than it is allowed, and slots stay. So while a group
 * with headroom has such a core among the active CPUs @p may run on, the
 * unused siblings of its used CPUs are left out of @p's search. A limit at
 * or above the task's CPU count has room for all of them: nothing to
 * spread. The masks are read without the group lock, like the used set in
 * conc_select_prepare(): a stale view only places a slot less well.
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
 * Before a class selects a CPU for @p: restrict its view of the allowed
 * CPUs to the used set of the first level, from @p's group up, that has
 * no headroom, so that the class picks a usable CPU in the first place
 * and conc_select_cpu() rarely has to redirect. A level below it that has
 * headroom spreads its slots over cores, see conc_spread(): the innermost
 * one that does narrows the search first.
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
		/* The used set, within the spread when they meet. */
		if (spread && cpumask_and(mask, mask, conc->used))
			return;
		if (cpumask_and(mask, p->cpus_ptr, conc->used))
			__this_cpu_write(conc_select_cpus, mask);
		else
			__this_cpu_write(conc_select_cpus, NULL);
		return;
	}
}

/* Ask the cleaner to reclaim an idle slot so that the next arrival finds headroom. */
static inline void conc_want_headroom(struct tg_concurrency *conc)
{
	irq_work_queue(&conc->irq_work);
}

/*
 * Affinity masks: the masks the tasks of a subtree are configured to run
 * on (see __conc_task_class_mask()) are kept per group with a task count,
 * under the group's lock. Changes are admitted by the width of the masks
 * (see conc_masks_width()); tasks are placed by the classes, the minimal
 * live parts (see conc_refresh_live()). A mask that restricts nothing is
 * not kept. Entries are allocated outside the lock, by the process-context
 * caller of the change, and freed through RCU. A task points at the entry
 * it is counted under at its own level; the ancestors hold the same mask.
 * The mask is computed afresh at every registration, so that a task
 * registered while a cpuset change is reaching the tasks one at a time
 * gets the new mask, as the ones registered ahead by conc_cpuset_update()
 * did.
 */
static struct conc_class *conc_class_alloc(gfp_t gfp)
{
	return kzalloc(sizeof(struct conc_class) + 2 * cpumask_size(), gfp);
}

/*
 * Dormant: no CPU of @c may be used now, or none of its tasks can run on
 * one, every applied mask lying outside its mask (see
 * conc_task_set_outside()). Either way no task of it takes a slot inside
 * it, and headroom kept for it would be headroom no task can take.
 */
static bool conc_class_is_dormant(struct conc_class *c)
{
	return cpumask_empty(conc_class_live(c)) || c->outside == c->tasks;
}

/* The live part of @c: the CPUs of its mask that may be used now. */
static void conc_class_set_live(struct conc_class *c)
{
	cpumask_and(conc_class_live(c), conc_class_mask(c), cpu_active_mask);
	cpumask_andnot(conc_class_live(c), conc_class_live(c), &conc_offlining_cpus);
	c->dormant = conc_class_is_dormant(c);
}

/*
 * Does @d keep @o from being a class? A live part strictly inside @o's,
 * or an equal one already counted as the class: equal live parts make
 * one class.
 */
static bool conc_class_dominates(struct conc_class *d, struct conc_class *o)
{
	if (d == o || d->dormant || !cpumask_subset(conc_class_live(d), conc_class_live(o)))
		return false;
	return d->minimal || !cpumask_equal(conc_class_live(d), conc_class_live(o));
}

static bool conc_mask_counts(struct task_struct *p, const struct cpumask *mask)
{
	return mask && !cpumask_subset(task_cpu_possible_mask(p), mask);
}

/* Does @applied, the mask a task runs on, miss @mask, the one it is registered by? */
static inline bool conc_mask_outside(const struct cpumask *mask, const struct cpumask *applied)
{
	return !cpumask_intersects(mask, applied);
}

/*
 * Count the classes of @conc and the unhit ones again, from the live parts
 * and the dormant entries as they are; ask for the headroom they lack.
 */
static void conc_classes_recount(struct tg_concurrency *conc)
{
	struct conc_class *c, *d;

	lockdep_assert_held(&conc->lock);
	list_for_each_entry(c, &conc->masks, node)
		c->minimal = false;
	conc->unhit = 0;
	/* In list order: of equal live parts, the first is the class. */
	list_for_each_entry(c, &conc->masks, node) {
		bool minimal = !c->dormant;

		list_for_each_entry(d, &conc->masks, node) {
			if (!minimal)
				break;
			if (conc_class_dominates(d, c))
				minimal = false;
		}
		if (!minimal)
			continue;
		c->minimal = true;
		if (!cpumask_intersects(conc_class_live(c), conc->used))
			conc->unhit++;
	}
	if (conc->effective && conc->nr_used + conc->unhit > conc->effective)
		conc_want_headroom(conc);
}

/*
 * The tasks of @c changed. When that puts it to sleep or wakes it up, the
 * classes are counted again; one going dormant may hand headroom back to
 * held tasks.
 */
static void conc_class_tasks_changed(struct tg_concurrency *conc, struct conc_class *c)
{
	bool dormant = conc_class_is_dormant(c);

	lockdep_assert_held(&conc->lock);
	if (dormant == c->dormant)
		return;
	c->dormant = dormant;
	conc_classes_recount(conc);
	if (dormant)
		conc_hold_kick();
}

/*
 * Register one task with @mask; @entry is consumed when a new entry is
 * needed. Returns the entry the task is counted under, NULL when none
 * could be made.
 */
static struct conc_class *__conc_class_add(struct tg_concurrency *conc, const struct cpumask *mask,
				       struct conc_class *entry, bool outside)
{
	struct conc_class *c;
	bool minimal = true;

	lockdep_assert_held(&conc->lock);

	list_for_each_entry(c, &conc->masks, node) {
		if (cpumask_equal(conc_class_mask(c), mask)) {
			c->tasks++;
			c->outside += outside;
			conc_class_tasks_changed(conc, c);
			return c;
		}
	}
	if (!entry)
		return NULL;

	cpumask_copy(conc_class_mask(entry), mask);
	entry->tasks = 1;
	entry->outside = outside;
	conc_class_set_live(entry);
	minimal = !entry->dormant;
	/* A class unless an existing live part is within its own ... */
	list_for_each_entry(c, &conc->masks, node) {
		if (minimal && !c->dormant && cpumask_subset(conc_class_live(c), conc_class_live(entry))) {
			minimal = false;
			break;
		}
	}
	/* ... in which case the classes its live part is within are no longer. */
	if (minimal) {
		list_for_each_entry(c, &conc->masks, node) {
			if (!c->minimal || !cpumask_subset(conc_class_live(entry), conc_class_live(c)))
				continue;
			c->minimal = false;
			if (!cpumask_intersects(conc_class_live(c), conc->used))
				conc->unhit--;
		}
		if (!cpumask_intersects(conc_class_live(entry), conc->used)) {
			conc->unhit++;
			/* Idle slots may have to go for its headroom. */
			if (conc->effective && conc->nr_used + conc->unhit > conc->effective)
				conc_want_headroom(conc);
		}
	}
	entry->minimal = minimal;
	list_add(&entry->node, &conc->masks);
	return entry;
}

/*
 * Unregister one task with @mask, @outside when its applied mask missed
 * it; returns the entry to free when it was the last.
 */
static struct conc_class *__conc_class_del(struct tg_concurrency *conc, const struct cpumask *mask,
				       bool outside)
{
	struct conc_class *c, *o, *d;

	lockdep_assert_held(&conc->lock);

	list_for_each_entry(c, &conc->masks, node) {
		if (cpumask_equal(conc_class_mask(c), mask))
			goto found;
	}
	return NULL;
found:
	if (outside && c->outside)
		c->outside--;
	if (--c->tasks) {
		c->outside = min(c->outside, c->tasks);
		conc_class_tasks_changed(conc, c);
		return NULL;
	}
	list_del(&c->node);
	if (!c->minimal)
		return c;
	if (!cpumask_intersects(conc_class_live(c), conc->used)) {
		conc->unhit--;
		/* The headroom kept for it is free: a held task may fit. */
		conc_hold_kick();
	}
	/* The live parts it was within may be classes now. */
	list_for_each_entry(o, &conc->masks, node) {
		bool dominated = false;

		if (o->minimal || o->dormant ||
		    !cpumask_subset(conc_class_live(c), conc_class_live(o)))
			continue;
		list_for_each_entry(d, &conc->masks, node) {
			if (conc_class_dominates(d, o)) {
				dominated = true;
				break;
			}
		}
		if (dominated)
			continue;
		o->minimal = true;
		if (!cpumask_intersects(conc_class_live(o), conc->used))
			conc->unhit++;
	}
	return c;
}

/* The used set gains @cpu: classes it hits are no longer unhit. */
static void conc_unhit_slot_added(struct tg_concurrency *conc, int cpu)
{
	struct conc_class *c;

	lockdep_assert_held(&conc->lock);
	list_for_each_entry(c, &conc->masks, node) {
		if (c->minimal && cpumask_test_cpu(cpu, conc_class_live(c)) &&
		    !cpumask_intersects(conc_class_live(c), conc->used))
			conc->unhit--;
	}
}

/* The used set loses @cpu: classes only it hit become unhit. */
static void conc_unhit_slot_removed(struct tg_concurrency *conc, int cpu)
{
	struct conc_class *c;

	lockdep_assert_held(&conc->lock);
	list_for_each_entry(c, &conc->masks, node) {
		if (c->minimal && cpumask_test_cpu(cpu, conc_class_live(c)) &&
		    cpumask_weight_and(conc_class_live(c), conc->used) == 1)
			conc->unhit++;
	}
}

/*
 * The used set gains @cpu: the classes it hits are no longer unhit and its
 * SMT core is no longer unused, see conc_spread().
 */
static void conc_used_add(struct tg_concurrency *conc, int cpu)
{
	conc_unhit_slot_added(conc, cpu);
	cpumask_set_cpu(cpu, conc->used);
	cpumask_andnot(conc->unused_cores, conc->unused_cores, cpu_smt_mask(cpu));
	WRITE_ONCE(conc->nr_used, conc->nr_used + 1);
}

/*
 * The value cpu.concurrency.effective reports for @conc, see
 * conc_effective_report(), as a ceiling: UINT_MAX for none. Under conc->lock.
 */
static unsigned int conc_report_ceiling(struct tg_concurrency *conc)
{
	if (!conc->effective)
		return UINT_MAX;
	return conc->shrink_pending ? max(conc->effective, conc->nr_used) : conc->effective;
}

/*
 * The value reported for @conc is about to drop from @from. The concurrency
 * IDs of the domains it bounds may have been given under @from (see
 * __conc_dom_bound()) and keep it until their bounds are applied again: the
 * file shows no less until then, see conc_bounds_apply(). Under conc->lock.
 */
static void conc_report_drop(struct tg_concurrency *conc, unsigned int from)
{
	lockdep_assert_held(&conc->lock);
	conc->shown_floor = max(conc->shown_floor, from);
	conc->bounds_pending = true;
}

/*
 * The used set loses @cpu: the classes only it hit become unhit, and its
 * SMT core is unused again unless a sibling is used.
 */
static void conc_used_del(struct tg_concurrency *conc, int cpu)
{
	/*
	 * Over its limit, the group reports its CPUs in use: one fewer now.
	 * The cleaner applies the bounds (see conc_cleaner_work()), unless the
	 * configuration change under way does it first.
	 */
	if (conc->effective && conc->shrink_pending && conc->nr_used > conc->effective) {
		conc_report_drop(conc, conc->nr_used);
		irq_work_queue(&conc->irq_work);
	}
	conc_unhit_slot_removed(conc, cpu);
	cpumask_clear_cpu(cpu, conc->used);
	if (!cpumask_intersects(cpu_smt_mask(cpu), conc->used))
		cpumask_or(conc->unused_cores, conc->unused_cores, cpu_smt_mask(cpu));
	WRITE_ONCE(conc->nr_used, conc->nr_used - 1);
}

/* How many of the classes no used CPU hits would @cpu hit? */
static unsigned int conc_unhit_hits(struct tg_concurrency *conc, int cpu)
{
	struct conc_class *c;
	unsigned int n = 0;

	lockdep_assert_held(&conc->lock);
	if (!conc->unhit)
		return 0;
	list_for_each_entry(c, &conc->masks, node) {
		if (c->minimal && cpumask_test_cpu(cpu, conc_class_live(c)) &&
		    !cpumask_intersects(conc_class_live(c), conc->used))
			n++;
	}
	return n;
}

/*
 * Can @conc add a slot on @cpu within its limit? An unhit class keeps a
 * slot of headroom for its sleeping members, which a slot inside the
 * class provides itself. Under conc->lock.
 */
static bool __conc_room_for(struct tg_concurrency *conc, int cpu)
{
	lockdep_assert_held(&conc->lock);
	return !conc->effective ||
	       conc->nr_used + conc->unhit - conc_unhit_hits(conc, cpu) < conc->effective;
}

static bool conc_room_for(struct tg_concurrency *conc, int cpu)
{
	guard(raw_spinlock_irqsave)(&conc->lock);
	return __conc_room_for(conc, cpu);
}

/*
 * The CPUs of @p's mask inside the classes that no used CPU of its chain
 * hits, into the per-CPU scratch mask. A slot added on one of them
 * serves the sleeping members of that class too. Under p->pi_lock.
 */
static struct cpumask *conc_unhit_cpus(struct task_struct *p)
{
	struct cpumask *mask = this_cpu_cpumask_var_ptr(conc_unhit_mask);
	struct task_group *lvl;
	struct conc_class *c;

	cpumask_clear(mask);
	for_each_conc_level(lvl, p) {
		guard(raw_spinlock)(&lvl->conc.lock);
		if (!lvl->conc.unhit)
			continue;
		list_for_each_entry(c, &lvl->conc.masks, node) {
			if (c->minimal && !cpumask_intersects(conc_class_live(c), lvl->conc.used))
				cpumask_or(mask, mask, conc_class_live(c));
		}
	}
	if (cpumask_empty(mask))
		return NULL;
	cpumask_and(mask, mask, p->cpus_ptr);
	cpumask_and(mask, mask, cpu_active_mask);
	cpumask_andnot(mask, mask, &conc_offlining_cpus);
	return cpumask_empty(mask) ? NULL : mask;
}

/* Is @cpu the only used CPU hitting some class? */
static bool conc_slot_sole_hitter(struct tg_concurrency *conc, int cpu)
{
	struct conc_class *c;

	guard(raw_spinlock_irqsave)(&conc->lock);
	list_for_each_entry(c, &conc->masks, node) {
		if (c->minimal && cpumask_test_cpu(cpu, conc_class_live(c)) &&
		    cpumask_weight_and(conc_class_live(c), conc->used) == 1)
			return true;
	}
	return false;
}

/* Drop every entry: the group stops counting, or goes away. */
static void conc_classes_drop(struct tg_concurrency *conc)
{
	struct conc_class *c, *n;
	LIST_HEAD(gone);
	unsigned long flags;

	raw_spin_lock_irqsave(&conc->lock, flags);
	list_splice_init(&conc->masks, &gone);
	conc->unhit = 0;
	raw_spin_unlock_irqrestore(&conc->lock, flags);
	list_for_each_entry_safe(c, n, &gone, node)
		kfree_rcu(c, rcu);
}

static int conc_chain_depth(struct task_struct *p)
{
	struct task_group *lvl;
	int n = 0;

	for_each_conc_level(lvl, p)
		n++;
	return n;
}

/*
 * The levels of a task's chain, one at a time: lock @lvl and return it
 * when it counts, else NULL. See conc_chain_unlock() for the next level.
 */
static struct task_group *conc_chain_lock(struct task_group *lvl, unsigned long *flags)
{
	if (!lvl)
		return NULL;
	raw_spin_lock_irqsave(&lvl->conc.lock, *flags);
	if (lvl->conc.active)
		return lvl;
	raw_spin_unlock_irqrestore(&lvl->conc.lock, *flags);
	return NULL;
}

/*
 * Unlock @lvl and return the next level of the chain: its parent when
 * @lvl's list is merged into it. That is read under the lock a merge holds
 * while it copies the list up (conc_classes_merge()), so that a task's
 * entry at @lvl is either copied by the merge or added above by its own
 * walk, never both, and is removed from above when it was copied there.
 */
static struct task_group *conc_chain_unlock(struct task_group *lvl, unsigned long flags)
{
	struct task_group *up = lvl->conc.merged ? lvl->parent : NULL;

	raw_spin_unlock_irqrestore(&lvl->conc.lock, flags);
	return up;
}

static struct conc_class **conc_class_prealloc(int n, gfp_t gfp)
{
	struct conc_class **pre;
	int i;

	if (n <= 0)
		return NULL;
	pre = kcalloc(n, sizeof(*pre), gfp);
	if (!pre)
		return NULL;
	for (i = 0; i < n; i++) {
		pre[i] = conc_class_alloc(gfp);
		if (!pre[i])
			break;
	}
	return pre;
}

static void conc_class_prealloc_free(struct conc_class **pre, int n)
{
	int i;

	if (!pre)
		return;
	for (i = 0; i < n; i++)
		kfree(pre[i]);
	kfree(pre);
}

/*
 * Register @mask, the mask @p carries or is about to, in its chain,
 * consuming entries of @pre where a level needs one; @applied is the mask
 * @p runs on, or is about to. Under p->pi_lock, or the lock of the
 * runqueue @p is queued on. Without an entry a level misses the class
 * until the next activation of the group. A task that is exiting is not
 * registered: its release, which unregisters, may have run already (a
 * cgroup iterator can hand out a task on its way out).
 */
static void conc_task_class_add(struct task_struct *p, const struct cpumask *mask,
			      const struct cpumask *applied, struct conc_class **pre, int npre)
{
	bool outside = conc_mask_outside(mask, applied);
	struct task_group *lvl = task_group(p);
	unsigned long flags;
	int i = 0;

	if (p->conc_class || (p->flags & PF_EXITING) || !conc_mask_counts(p, mask))
		return;
	while ((lvl = conc_chain_lock(lvl, &flags))) {
		struct conc_class *e = pre && i < npre ? pre[i] : NULL, *got;

		got = __conc_class_add(&lvl->conc, mask, e, outside);
		lvl = conc_chain_unlock(lvl, flags);
		if (got && got == e)
			pre[i] = NULL;
		if (!i) {
			/* Not even at its own level: not registered at all. */
			if (!got)
				return;
			p->conc_class = got;
			p->conc_outside = outside;
		}
		i++;
	}
}

/*
 * Unregister the mask @p is registered with from the chain starting at
 * @tg, its group or the one it just left. Same locking. The entry's mask
 * outlives its removal by a grace period.
 */
static void conc_task_class_del_from(struct task_struct *p, struct task_group *tg)
{
	struct conc_class *own = p->conc_class;
	struct task_group *lvl = tg;
	const struct cpumask *mask;
	unsigned long flags;
	bool outside;

	if (!own)
		return;
	outside = p->conc_outside;
	p->conc_class = NULL;
	p->conc_outside = 0;
	/*
	 * A group that stopped counting dropped its list and unmerged it
	 * from its ancestors; the entry of a task it could not reach then
	 * (one on its way out) is gone with it.
	 */
	if (!READ_ONCE(tg->conc.active))
		return;
	mask = conc_class_mask(own);
	guard(rcu)();
	while ((lvl = conc_chain_lock(lvl, &flags))) {
		struct conc_class *e = __conc_class_del(&lvl->conc, mask, outside);

		lvl = conc_chain_unlock(lvl, flags);
		if (e)
			kfree_rcu(e, rcu);
	}
}

static void conc_task_class_del(struct task_struct *p)
{
	conc_task_class_del_from(p, task_group(p));
}

/* The mask @p is registered with, or NULL. */
static inline const struct cpumask *conc_task_class_mask(struct task_struct *p)
{
	return p->conc_class ? conc_class_mask(p->conc_class) : NULL;
}

/*
 * @p stays registered with its mask, and @applied, the mask it runs on or
 * is about to, misses it or meets it again. A cpuset applies its own CPUs
 * and keeps the request aside when they miss it, and gives its tasks its
 * parent's CPUs when all of its own are offline: a hotplug event never
 * changes the registered mask (see __conc_task_class_mask()), so the
 * applied one can lie outside it. A class all of whose tasks are outside
 * is dormant, see conc_class_is_dormant(): until cpuset gives the CPUs
 * back, which reaches the cpusets one at a time after they come online,
 * headroom kept for it could serve none of its tasks, and would hold them
 * off the other CPUs. Counted in place at every level of the chain,
 * without entries to allocate. Same locking as conc_task_class_add().
 */
static void conc_task_set_outside(struct task_struct *p, const struct cpumask *applied)
{
	const struct cpumask *mask = conc_task_class_mask(p);
	struct task_group *lvl = task_group(p);
	unsigned long flags;
	bool outside;

	if (!mask)
		return;
	outside = conc_mask_outside(mask, applied);
	if (outside == p->conc_outside)
		return;
	p->conc_outside = outside;
	while ((lvl = conc_chain_lock(lvl, &flags))) {
		struct tg_concurrency *conc = &lvl->conc;
		struct conc_class *c;

		list_for_each_entry(c, &conc->masks, node) {
			if (!cpumask_equal(conc_class_mask(c), mask))
				continue;
			if (outside)
				c->outside = min(c->outside + 1, c->tasks);
			else if (c->outside)
				c->outside--;
			conc_class_tasks_changed(conc, c);
			break;
		}
		lvl = conc_chain_unlock(lvl, flags);
	}
}

/*
 * Is @p registered with @mask? Its outside state then follows @applied,
 * the mask it runs on or is about to.
 */
static bool conc_task_registered_with(struct task_struct *p, const struct cpumask *mask,
				    const struct cpumask *applied)
{
	if (!p->conc_class || !cpumask_equal(conc_task_class_mask(p), mask))
		return false;
	conc_task_set_outside(p, applied);
	return true;
}

/*
 * The mask @p is registered with is about to become @new, the mask it
 * runs on @applied, its current one still in p->cpus_mask. Entries come
 * from @pre when the change was prepared in process context, see below.
 */
static void __conc_task_mask_changed(struct task_struct *p, const struct cpumask *new,
				   const struct cpumask *applied, struct conc_class **pre, int npre)
{
	/*
	 * Registered ahead by conc_prepare_affinity() or conc_cpuset_update(),
	 * or unchanged.
	 */
	if (conc_task_registered_with(p, new, applied))
		return;
	conc_task_class_del(p);
	if (!conc_mask_counts(p, new) || !READ_ONCE(task_group(p)->conc.active))
		return;
	/*
	 * Without entries (a forced change under the runqueue lock, which
	 * cannot allocate: a fallback widening at a wakeup) existing entries
	 * are joined and a level that would need a new one misses the class
	 * until the next registration; a task that wakes up to no CPU for
	 * that is held, not placed in excess, see conc_hold_request().
	 */
	conc_task_class_add(p, new, applied, pre, npre);
}

/*
 * The mask @t is configured to run on, by which it is registered: its
 * cpuset's CPUs with every CPU online (see
 * cpuset_task_cpus_configured_rcu()), masked by @user, its requested mask,
 * when they intersect, as a cpuset change masks it. A hotplug event never
 * changes it, so that the width of the registered masks, by which changes
 * are admitted, never grows behind the admission's back; the classes by
 * which tasks are placed follow the live parts (see conc_refresh_live()).
 * A fallback widening drops the requested mask (set_cpus_allowed_force()),
 * and the class follows: the task's mask restricts nothing from then on.
 */
static void __conc_task_class_mask(struct task_struct *t, const struct cpumask *user,
				 struct cpumask *mask)
{
	scoped_guard(rcu)
		cpuset_task_cpus_configured_rcu(t, mask);
	if (user && cpumask_intersects(mask, user))
		cpumask_and(mask, mask, user);
}

static void conc_task_class_mask_now(struct task_struct *t, struct cpumask *mask)
{
	guard(raw_spinlock_irq)(&t->pi_lock);
	__conc_task_class_mask(t, t->user_cpus_ptr, mask);
}

/*
 * set_cpus_allowed_common(): under p->pi_lock and the rq lock, the task is
 * registered by the mask it is configured to run on with the change.
 */
void conc_task_mask_changed(struct task_struct *p, struct affinity_context *ctx)
{
	struct cpumask *ent;

	if (!static_branch_unlikely(&sched_conc_enabled))
		return;
	if (!p->conc_class && !READ_ONCE(task_group(p)->conc.active))
		return;
	lockdep_assert_irqs_disabled();
	ent = this_cpu_cpumask_var_ptr(conc_class_scratch);
	/* The requested mask that will be in place, see the swap in the caller. */
	__conc_task_class_mask(p, ctx->flags & SCA_USER ? ctx->user_mask : p->user_cpus_ptr, ent);
	__conc_task_mask_changed(p, ent, ctx->new_mask, ctx->conc.prealloc, ctx->conc.nr_prealloc);
}

/*
 * Register @p with the mask it is configured to run on, when not
 * registered yet. Under p->pi_lock.
 */
static void conc_task_register(struct task_struct *p, struct conc_class **pre, int npre)
{
	struct cpumask *ent = this_cpu_cpumask_var_ptr(conc_class_scratch);

	lockdep_assert_irqs_disabled();
	if (p->conc_class)
		return;
	__conc_task_class_mask(p, p->user_cpus_ptr, ent);
	conc_task_class_add(p, ent, &p->cpus_mask, pre, npre);
}

/* A new task: register it, after cpuset_fork() placed it. */
void conc_task_fork(struct task_struct *p)
{
	struct conc_class **pre;
	int n;

	if (!static_branch_unlikely(&sched_conc_enabled))
		return;
	n = conc_chain_depth(p);
	if (!n)
		return;
	pre = conc_class_prealloc(n, GFP_KERNEL);
	scoped_guard(raw_spinlock_irq, &p->pi_lock)
		conc_task_register(p, pre, n);
	conc_class_prealloc_free(pre, n);
}

/* The levels a merge of @child copies its list into, see conc_classes_merge(). */
static int conc_merge_depth(struct task_group *child)
{
	struct task_group *lvl;
	int n = 0;

	for (lvl = child->parent; lvl; lvl = READ_ONCE(lvl->conc.merged) ? lvl->parent : NULL)
		n++;
	return n;
}

/*
 * Merge (@sign 1) or unmerge (@sign -1) the masks of @child, when its
 * parent starts or stops counting it. A merge copies them into the parent
 * and every level above that the parent's list is merged into, then marks
 * the child's list merged; an unmerge removes them from the parent, the
 * one level they were in, and clears the mark. @pre supplies up to @npre
 * entries for a merge. The child's lock is held across, so that a task of
 * the subtree finds its entry copied up and the list merged, or neither,
 * see conc_chain_unlock(); each level's lock nests inside in turn, the one
 * place where two group locks nest.
 */
static void conc_classes_merge(struct task_group *child, int sign, struct conc_class **pre, int npre)
{
	struct tg_concurrency *conc = &child->conc;
	struct task_group *lvl = child->parent;
	struct conc_class *c, *e;
	unsigned long flags;
	unsigned int i;
	LIST_HEAD(gone);
	int used = 0;

	raw_spin_lock_irqsave(&conc->lock, flags);
	while (lvl) {
		struct tg_concurrency *pc = &lvl->conc;

		raw_spin_lock_nested(&pc->lock, SINGLE_DEPTH_NESTING);
		list_for_each_entry(c, &conc->masks, node) {
			for (i = 0; i < c->tasks; i++) {
				if (sign < 0) {
					e = __conc_class_del(pc, conc_class_mask(c), i < c->outside);
					if (e)
						list_add(&e->node, &gone);
					continue;
				}
				/* Without an entry left, an equal one still counts it. */
				e = pre && used < npre ? pre[used] : NULL;
				if (__conc_class_add(pc, conc_class_mask(c), e, i < c->outside) == e && e)
					pre[used++] = NULL;
			}
		}
		lvl = sign > 0 && pc->merged ? lvl->parent : NULL;
		raw_spin_unlock(&pc->lock);
	}
	conc->merged = sign > 0;
	raw_spin_unlock_irqrestore(&conc->lock, flags);
	list_for_each_entry_safe(c, e, &gone, node)
		kfree_rcu(c, rcu);
}

static unsigned int conc_nr_masks(struct tg_concurrency *conc)
{
	struct conc_class *c;
	unsigned int n = 0;

	guard(raw_spinlock_irqsave)(&conc->lock);
	list_for_each_entry(c, &conc->masks, node)
		n++;
	return n;
}

static struct cpumask *conc_alloc_scratch(void)
{
	return kmalloc(cpumask_size(), GFP_KERNEL);
}

/*
 * Slow path of conc_claim(): the (@tg, @cpu) slot does not exist or is not
 * accepting entities. Adds the slot when the limit allows it, propagating
 * one entity to the parent when the parent counts this group.
 *
 * Group locks never nest: the parent entity is claimed with our lock
 * dropped and the slot state is re-validated afterwards. A parent entity
 * that ends up unused is released again; parent slots are sticky, so that
 * is harmless.
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
		/* The arrival a reservation was made for fills it, see conc_resv_make(). */
		if (p && conc_slot_reserved(v) && READ_ONCE(p->conc_resv_cpu) == cpu) {
			atomic_inc(slot);
			ret = CONC_CLAIMED;
			goto out;
		}
		/* Draining, reserved or evicting: arrivals are refused; entities may be forced. */
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

	/* No slot: none is added on a CPU going offline unless forced. */
	if (!force && cpumask_test_cpu(cpu, &conc_offlining_cpus)) {
		ret = CONC_REFUSED;
		goto out;
	}
	/*
	 * Add one when the limit allows it. An evicting slot still counts:
	 * its tasks run there until they are moved. Headroom is kept for
	 * the affinity classes that no used CPU hits, so that their members
	 * find a CPU when they wake up; a slot on a CPU inside such a class
	 * hits it and needs no headroom for it, whoever claims it.
	 */
	if (!__conc_room_for(conc, cpu)) {
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
		 * The parent counts us, or stopped counting since and claimed
		 * nothing: it stopped our propagation first, which the next
		 * pass sees, see conc_detach_children().
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
 * A late arrival: claim an evicting or draining slot of @tg on @cpu, which
 * still holds the group's concurrency IDs there. Serialized against the
 * set-move stopper. Returns false when the slot is not in such a state
 * (any more), in which case the caller selects again.
 */
static bool conc_claim_late(struct task_group *tg, int cpu)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, cpu);
	int v = atomic_read(slot);

	if (!conc_slot_evicting(v) && v != CONC_SLOT_DRAINING)
		return false;

	guard(raw_spinlock)(&conc_sm_lock);
	guard(raw_spinlock)(&conc->lock);
	v = atomic_read(slot);
	if (conc_slot_evicting(v)) {
		atomic_inc(slot);
	} else if (v == CONC_SLOT_DRAINING) {
		atomic_set(slot, 1);	/* the cleaner notices and keeps the slot */
	} else {
		return false;
	}
	return true;
}

/*
 * A cpuset change reaches the tasks one at a time, and the presence of a
 * group moves with the cpuset ahead of that, see conc_task_entitled(). A
 * task waking up with a mask behind its cpuset is brought up to date
 * here, as the cpuset's pass would moments later. Under p->pi_lock, the
 * task not queued. Returns true when the mask changed.
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
	scoped_guard(__task_rq_lock, p)
		do_set_cpus_allowed(p, &ac);
	return true;
}

/*
 * @p gets the slot claimed for it on @cpu. A task being woken up or forked
 * is not queued: p->pi_lock keeps every other path off its accounting. A
 * held one is queued, out of its class, for conc_try_place(), and the
 * selection may have returned it to its class meanwhile: a fallback
 * widening on the way (select_fallback_rq(), or the pinned task of a CPU
 * going offline in conc_select_cpu()) changes the mask of a queued task in
 * place, which dequeues it, which unholds it where it is, without a slot
 * and asked to hold again (conc_unhold()). There it can be picked, or moved,
 * under its runqueue lock alone, counted on @cpu while it runs elsewhere.
 * Give it the slot under that lock, while it is still held; otherwise the
 * slot goes back and the task goes on as the unhold left it.
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
 * Placement post-filter for wakeups and forks, see select_task_rq(). @cpu
 * is the class's choice. Returns the CPU to use; the task holds a slot on
 * it when the group counts.
 *
 * A CPU is usable when the task's group can claim it: a CPU of its own
 * used set, or one of an active ancestor's used set when the group has
 * headroom to add a slot there.
 *
 * The classes already search within the used set (conc_select_prepare()
 * for fair, conc_constrain_mask() for rt and deadline); this catches what
 * their searches miss: the used set changing meanwhile, a wakeup's
 * previous CPU taken as is, an rt or deadline search that found no used
 * CPU at the priority level it looked at.
 * The rt and deadline searches are narrowed after cpupri_find() and
 * cpudl_find() chose a priority level.
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
			/* The previous CPU first, it is the most likely to be cache hot. */
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
		 * No used CPU: a CPU inside a class that none hits yet, when the
		 * mask allows one. The slot added there is the one the headroom is
		 * kept for, see conc_claim_slow(), and serves that class's sleeping
		 * members when they wake up.
		 */
		if (conc_unhit_cpus(p)) {
			for_each_cpu_wrap(c, this_cpu_cpumask_var_ptr(conc_unhit_mask), prev + 1) {
				if (!is_cpu_allowed(p, c))
					continue;
				ret = conc_claim(tg, c, false, p);
				if (ret != CONC_REFUSED) {
					cpu = c;
					goto done;
				}
			}
		}

		/*
		 * No usable CPU. A slot of the own group that is being evicted or
		 * drained still holds the group's concurrency IDs on that CPU: land
		 * there, as a late arrival the eviction picks up, rather than
		 * without a slot on a new CPU.
		 * When the slot is gone by the time the claim is serialized, the
		 * used set has changed: select again.
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

	/* The mask may be behind the cpuset the presence moved with: once more. */
	if (!updated && conc_task_mask_behind(p)) {
		updated = true;
		if (!cpumask_test_cpu(cpu, p->cpus_ptr) || !is_cpu_allowed(p, cpu))
			cpu = select_fallback_rq(prev, p);
		goto again;
	}

	/*
	 * Pinned to CPUs going offline alone, waking up before the unplug
	 * widens it, see conc_widen_pinned(): give it the affinity that
	 * select_fallback_rq() would give it once they are gone, now, so
	 * that it joins a used CPU rather than adding a slot in excess on a
	 * dying one, where an ID above the bound would follow.
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
	 * Nothing at all: the task's mask does not intersect the used sets
	 * and there is no headroom. A held task stays held, see
	 * conc_hold_work_fn(). Any other goes without a slot, on the class's
	 * choice or on another allowed CPU when that one is going offline,
	 * to finish in kernel mode what it was woken for and be held at its
	 * return to user space, see conc_hold_request(); it takes no ID.
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
 * Called from sched_move_task() with the task dequeued and both locks held,
 * after task_group(p) was switched away from @old_tg.
 */
void conc_task_change_group(struct task_struct *p, struct task_group *old_tg)
{
	int cpu = task_cpu(p);

	if (p->conc_accounted) {
		p->conc_accounted = 0;
		conc_release(old_tg, cpu);
	}
	/* Its class leaves the old chain; conc_attach_finish() registers it anew. */
	conc_task_class_del_from(p, old_tg);
	/* Held: nothing to claim; the new group may have room for it. */
	if (p->conc_held) {
		conc_hold_kick();
		return;
	}
	/* Present: queued, or in flight between selection and enqueue. */
	if (!conc_task_active(p) ||
	    (!task_on_rq_queued(p) && READ_ONCE(p->__state) != TASK_WAKING))
		return;
	/* No room in the new group: held at its next return to user space. */
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
	return !tg->conc.effective ||
	       READ_ONCE(tg->conc.nr_used) + READ_ONCE(tg->conc.unhit) < tg->conc.effective;
}

/*
 * Lockless pre-check for the load balancer: can @p plausibly move to @cpu?
 * The move itself is validated by conc_task_move().
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
 * Restrict a CPU search @mask to the used set of @p's group when the group
 * has no headroom, to its spread over cores when it has. Returns false
 * when nothing is left.
 */
bool conc_constrain_mask(struct task_struct *p, struct cpumask *mask)
{
	struct tg_concurrency *conc;
	int cpu;

	if (!conc_task_active(p))
		return true;
	conc = &task_group(p)->conc;
	if (!conc_tg_has_headroom(task_group(p))) {
		struct cpumask *unhit = conc_unhit_cpus(p);

		/* The headroom kept for a class of @p no used CPU hits is its own. */
		if (unhit) {
			cpumask_or(unhit, unhit, conc->used);
			return cpumask_and(mask, mask, unhit);
		}
		return cpumask_and(mask, mask, conc->used);
	}
	/*
	 * Spread over cores, see conc_spread(), when that leaves a CPU: an rt
	 * or deadline task takes the sibling of a used CPU rather than wait.
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
 * Destination for an affinity change: a CPU of @allowed that is @valid,
 * preferring the used set of @p's group.
 */
int conc_pick_dest(struct task_struct *p, const struct cpumask *valid,
		 const struct cpumask *allowed, struct affinity_context *ctx)
{
	int cpu;

	if (ctx->conc.resv) {
		cpu = conc_resv_cpu(ctx->conc.resv);
		if (cpumask_test_cpu(cpu, valid) && cpumask_test_cpu(cpu, allowed))
			return cpu;
	}
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

/* A slot went away: an excess noted while it existed may be gone with it. */
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
	/* Room for a held task, maybe. */
	conc_hold_kick();
}

#ifdef CONFIG_SCHED_MM_CID
/* A reference on the object a domain is embedded in. */
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
	 * The task running here owns the cached ID while it is present. One
	 * that blocked already, still current while the runqueue picks its
	 * successor with the lock dropped (sched_balance_newidle()), gave its
	 * slot back: the slot may be going away with the ID, which the
	 * task's switch-out would drop too late for a CPU that allocates in
	 * between.
	 */
	if (!cid_task_in_domain(rq->curr, dom) || !task_on_rq_queued(rq->curr))
		cid_drop_on_cpu(dom, per_cpu_ptr(dom->pcpu, cpu));
}

/*
 * A slot is going away: drop the CPU-owned CIDs that the domains of @tg's
 * tasks cache on @cpu, so that a CPU outside the used set never holds an
 * ID and the space below the bound cannot be exhausted by stale caches.
 * Child groups are drained before their parent, so their tasks were
 * handled already.
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
	 * The domains whose CID the CPU owns, rather than the tasks of the
	 * group: the CIDs of the other groups' domains are dropped too, which
	 * costs them an allocation at their next schedule-in here, where a
	 * walk of the group's tasks would cost every slot drain O(tasks).
	 */
	guard(rq_lock_irq)(rq);
	list_for_each_entry_safe(pcp, tmp, per_cpu_ptr(&cid_cpu_doms, cpu), node) {
		struct cid_domain *dom = pcp->dom;

		if (!cid_task_in_domain(rq->curr, dom) || !task_on_rq_queued(rq->curr))
			cid_drop_on_cpu(dom, pcp);
	}
}

/* Refresh the CID bounds of the domains of @t. */
static void conc_task_update_bounds(struct task_struct *t)
{
	struct cid_domain *doms[2];
	int i, n = conc_task_domains(t, doms);

	for (i = 0; i < n; i++) {
		conc_dom_update_bound(doms[i]);
		cid_domain_put(doms[i]);
	}
}

/* Refresh the CID bounds of the tasks being attached. */
static __maybe_unused void conc_update_tset_mm_bounds(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct task_struct *task;

	cgroup_taskset_for_each(task, css, tset)
		conc_task_update_bounds(task);
}

/* Refresh the CID bounds of every domain with a task in the subtree of @top. */
static void conc_update_mm_bounds(struct task_group *top)
{
	struct cgroup_subsys_state *css;

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
			conc_task_update_bounds(t);
		css_task_iter_end(&it);
		rcu_read_lock();
		css_put(css);
	}
	rcu_read_unlock();
}

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
			/* Remembered until the migration is over, see conc_slot_removed(). */
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

	/* Set and cleared by the migrations alone, serialized by cgroup_mutex. */
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
 * Apply the concurrency bounds of every domain with a task in @top's
 * subtree, then let the effective files of the subtree show the values
 * the bounds follow now: a drop recorded meanwhile (conc_report_drop())
 * keeps its floor for the cleaner's next pass. Under conc_mutex.
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
 * Remove the (@tg, @cpu) slot if it is idle through the whole hierarchy:
 * child groups using the CPU are drained first, since their slots are
 * entities of ours. Returns -EBUSY when a task is present.
 *
 * Locks are taken one group at a time, never a child's under a parent's.
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
		 * A dying child is drained as any other: its sticky slot is
		 * an entity of ours. conc_tg_offline() resets what remains.
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
	/* Only an idle slot can be drained; a lockless claim may win the race. */
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
	/* The parent's slot was this group's entity only: it is idle now, up the chain. */
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
 * Drain the idle slots that child groups of @tg keep on @cpu: sticky slots
 * of groups whose tasks have left the CPU, or the group, still count as
 * entities of @tg and inflate its view. Busy children are left alone.
 */
static __maybe_unused void conc_drain_idle_children(struct task_group *tg, int cpu)
{
	struct task_group *child;

	lockdep_assert_held(&conc_mutex);

	rcu_read_lock();
	list_for_each_entry_rcu(child, &tg->children, siblings) {
		if (!cpumask_test_cpu(cpu, child->conc.used))
			continue;
		if (!css_tryget_online(&child->css))
			continue;
		rcu_read_unlock();
		__conc_drain_cpu(child, cpu, false);
		rcu_read_lock();
		css_put(&child->css);
	}
	rcu_read_unlock();
}

/*
 * The cleaner: reclaim one idle slot of a group at its limit so that the
 * next arrival on a new CPU finds headroom. Queued from conc_claim_slow().
 * It also applies the concurrency bounds after the value reported dropped
 * outside a configuration change, queued from conc_used_del().
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
	if (!conc->active || !conc->effective ||
	    READ_ONCE(conc->nr_used) + READ_ONCE(conc->unhit) < conc->effective)
		goto out;
	cpumask_copy(cpus, conc->used);

	/*
	 * Idle slots first, those sharing their SMT core with another used
	 * CPU ahead (the headroom made goes to a core of its own, see
	 * conc_spread()), then slots whose only entities are idle child
	 * groups, until a slot fits again with the headroom of the unhit
	 * classes kept. A slot that is the only used CPU hitting some
	 * affinity class is kept: draining it would only turn into headroom
	 * reserved for that class.
	 */
	for (pass = 0; pass < 3 && !done; pass++) {
		for_each_cpu(cpu, cpus) {
			int v = atomic_read(per_cpu_ptr(conc->present, cpu));

			if (pass == 2 ? v <= 0 : v != 0)
				continue;
			if (!pass && cpumask_weight_and(cpu_smt_mask(cpu), conc->used) < 2)
				continue;
			if (conc_slot_sole_hitter(conc, cpu))
				continue;
			if (!conc_drain_cpu(tg, cpu) &&
			    READ_ONCE(conc->nr_used) + READ_ONCE(conc->unhit) < conc->effective) {
				done = true;
				break;
			}
		}
	}
out:
	/* The value reported dropped (see conc_used_del()): the IDs follow, then the file. */
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
 * Register the masks of @tg's own tasks (unclassed ones) when it starts
 * counting, at every counting level of their chain: the ancestors that
 * count were visited first by the configuration walk, and a group that
 * starts counting has no list a merge could add, see conc_update_tree().
 * Called under RCU from that walk: atomic allocations only.
 */
static void conc_classes_build(struct task_group *tg)
{
	struct css_task_iter it;
	struct task_struct *t;

	css_task_iter_start(&tg->css, 0, &it);
	while ((t = css_task_iter_next(&it))) {
		struct conc_class **pre;
		int n;

		if (READ_ONCE(t->conc_class))
			continue;
		n = conc_chain_depth(t);
		pre = conc_class_prealloc(n, GFP_KERNEL);
		scoped_guard(raw_spinlock_irq, &t->pi_lock) {
			if (task_group(t) == tg)
				conc_task_register(t, pre, n);
		}
		conc_class_prealloc_free(pre, n);
	}
	css_task_iter_end(&it);
}

/*
 * @tg stops counting: drop its list and unclass its direct tasks, the dead
 * ones too, which keep their entry until they are reaped.
 */
static void conc_classes_teardown(struct task_group *tg)
{
	struct css_task_iter it;
	struct task_struct *t;

	css_task_iter_start(&tg->css, CSS_TASK_ITER_WITH_DEAD, &it);
	while ((t = css_task_iter_next(&it))) {
		scoped_guard(raw_spinlock_irq, &t->pi_lock) {
			if (task_group(t) == tg) {
				t->conc_class = NULL;
				t->conc_outside = 0;
			}
		}
	}
	css_task_iter_end(&it);
	conc_classes_drop(&tg->conc);
}

/*
 * @tg goes offline with dead tasks only, which keep their entry until they
 * are reaped: unregister them from the chain while it counts, or the
 * counting ancestors keep their masks. Under conc_mutex.
 */
static __maybe_unused void conc_classes_unregister_dead(struct task_group *tg)
{
	struct css_task_iter it;
	struct task_struct *t;

	css_task_iter_start(&tg->css, CSS_TASK_ITER_WITH_DEAD, &it);
	while ((t = css_task_iter_next(&it))) {
		scoped_guard(raw_spinlock_irq, &t->pi_lock) {
			if (task_group(t) == tg)
				conc_task_class_del(t);
		}
	}
	css_task_iter_end(&it);
}

/*
 * Claim slots for the tasks of @tg that are present but were never
 * accounted, when the group starts counting. Tasks claimed concurrently
 * carry the bit already and are skipped; the ones beyond the limit are
 * held at their next return to user space.
 */

/*
 * The tasks of @tg, which stops counting, give their slots back: a task is
 * accounted only while its group counts, which is what lets the hot paths
 * skip the accounted flag behind the key (conc_task_release()). A held task
 * is placed by the placement work, which the caller kicks.
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

/* What cpu.concurrency.effective reports: 0 is "max". */
static unsigned int conc_effective_report(struct task_group *tg)
{
	struct tg_concurrency *conc = &tg->conc;
	unsigned int eff = READ_ONCE(conc->effective);

	if (eff && READ_ONCE(conc->shrink_pending))
		eff = max(eff, READ_ONCE(conc->nr_used));
	return eff;
}

/*
 * What cpu.concurrency.effective shows: the value reported, but never
 * less than a bound the concurrency IDs of its domains may still be given
 * under, see conc_report_drop(). 0 for "max".
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
 * The width of @n distinct masks: the largest number of them no two of
 * which are nested. By Dilworth's theorem it is the fewest chains
 * covering them, @n minus a maximum matching of strict inclusion (König),
 * found here with one augmenting-path search per mask. Why the width:
 * the classes of any set of tasks the subtree can be left with, by exits,
 * migrations or CPUs going offline (which restrict every live part alike
 * and keep nested masks nested), are pairwise un-nested masks of these,
 * so at most the width of them; admitting while the width is within the
 * limit keeps the classes within it whatever follows, while the number
 * of minimal masks alone can grow when a mask that others contained goes
 * away. The price is that masks which overlap without being nested count
 * apart even when one CPU would serve them all. Allocates with @gfp;
 * @n, which no width exceeds, when it cannot.
 */
static unsigned int conc_masks_width(const char *masks, unsigned int n, gfp_t gfp)
{
	size_t words = BITS_TO_LONGS(n);
	unsigned long *adj = NULL, *seen = NULL;
	int *lmatch = NULL, *rmatch = NULL, *prev = NULL, *queue = NULL;
	unsigned int i, j, s, matched = 0;

#define CONC_MASK(i)	((const struct cpumask *)(masks + (size_t)(i) * cpumask_size()))
	if (n < 2)
		return n;
	adj = kcalloc(n * words, sizeof(long), gfp);
	seen = bitmap_zalloc(n, gfp);
	lmatch = kmalloc_array(n, sizeof(int), gfp);
	rmatch = kmalloc_array(n, sizeof(int), gfp);
	prev = kmalloc_array(n, sizeof(int), gfp);
	queue = kmalloc_array(n, sizeof(int), gfp);
	if (!adj || !seen || !lmatch || !rmatch || !prev || !queue) {
		matched = 0;
		goto out;
	}
	for (i = 0; i < n; i++) {
		lmatch[i] = rmatch[i] = -1;
		for (j = 0; j < n; j++) {
			if (j != i && cpumask_subset(CONC_MASK(i), CONC_MASK(j)))
				__set_bit(j, adj + i * words);
		}
	}
	for (s = 0; s < n; s++) {
		int head = 0, tail = 0, found = -1;

		bitmap_zero(seen, n);
		queue[tail++] = s;
		while (head < tail && found < 0) {
			int u = queue[head++];

			for_each_set_bit(j, adj + u * words, n) {
				if (__test_and_set_bit(j, seen))
					continue;
				prev[j] = u;
				if (rmatch[j] < 0) {
					found = j;
					break;
				}
				queue[tail++] = rmatch[j];
			}
		}
		if (found < 0)
			continue;
		matched++;
		for (j = found; (int)j >= 0; ) {
			int u = prev[j], next = lmatch[u];

			lmatch[u] = j;
			rmatch[j] = u;
			j = next;
		}
	}
out:
#undef CONC_MASK
	kfree(adj);
	bitmap_free(seen);
	kfree(lmatch);
	kfree(rmatch);
	kfree(prev);
	kfree(queue);
	return n - matched;
}

/*
 * The subtree of @top in pre-order, each css referenced, for the walks
 * that allocate or take a task's locks at length: css_for_each_descendant_pre()
 * needs RCU, which the configuration paths would otherwise hold across
 * every task of the subtree, allocating with GFP_ATOMIC. A css being
 * destroyed, which css_tryget() refuses, has no task left. Dying groups
 * are listed as the RCU walk lists them; the callers skip them as before.
 */
static int conc_subtree_snapshot(struct task_group *top, struct cgroup_subsys_state ***out, int *nr)
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
 * Add @ent to the distinct masks collected so far. Returns false when
 * the array could not grow.
 */
static bool conc_recount_add(char **masks, unsigned int *n, unsigned int *cap,
			   const struct cpumask *ent)
{
	unsigned int i;

	for (i = 0; i < *n; i++) {
		if (cpumask_equal((struct cpumask *)(*masks + i * cpumask_size()), ent))
			return true;
	}
	if (*n == *cap) {
		char *bigger;

		*cap = *cap ? *cap * 2 : 8;
		bigger = krealloc(*masks, *cap * cpumask_size(), GFP_KERNEL);
		if (!bigger)
			return false;
		*masks = bigger;
	}
	cpumask_copy((struct cpumask *)(*masks + *n * cpumask_size()), ent);
	(*n)++;
	return true;
}

/*
 * The width of @tg's subtree, recounted from the masks its tasks are
 * configured to run on now, which the registrations converge to: a
 * relaxation that the registered masks ask for is confirmed this way,
 * since they can be behind the tasks for an instant while a cpuset change
 * reaches them one at a time; and changes are judged this way. @trial,
 * when given, replaces the cpuset part of a task's mask (a cpuset write
 * being checked); @skip are tasks left out (a migration being checked)
 * and @extra masks counted in their place. Under conc_mutex, which keeps
 * the classes it reads. UINT_MAX when it could not be computed: a check
 * refuses.
 */
static unsigned int __conc_recount_width(struct task_group *tg,
				       bool (*trial)(struct task_struct *t, struct cpumask *mask,
						     bool *fixed, void *arg),
				       void *arg, struct task_struct **skip, int nr_skip,
				       const char *extra, int nr_extra)
{
	struct cgroup_subsys_state **csses;
	struct css_task_iter it;
	struct task_struct *t;
	cpumask_var_t ent;
	char *masks = NULL;
	unsigned int n = 0, cap = 0, i, width = UINT_MAX;
	int nr_css, k;
	bool fixed, trial_changes;

	if (!alloc_cpumask_var(&ent, GFP_KERNEL))
		return UINT_MAX;
	if (conc_subtree_snapshot(tg, &csses, &nr_css)) {
		free_cpumask_var(ent);
		return UINT_MAX;
	}
	/* Every task of the subtree: the children's css hold their own. */
	for (k = 0; k < nr_css; k++) {
		if (!csses[k])
			continue;
		css_task_iter_start(csses[k], 0, &it);
		while ((t = css_task_iter_next(&it))) {
			if (kthread_is_per_cpu(t))
				continue;
			for (i = 0; i < nr_skip; i++) {
				if (skip[i] == t)
					break;
			}
			if (i < nr_skip)
				continue;
			/* The mask it would be configured to run on under @trial, else is. */
			trial_changes = false;
			if (trial) {
				scoped_guard(rcu)
					trial_changes = trial(t, ent, &fixed, arg);
			}
			if (trial_changes) {
				guard(raw_spinlock_irq)(&t->pi_lock);
				if (t->user_cpus_ptr && cpumask_intersects(ent, t->user_cpus_ptr))
					cpumask_and(ent, ent, t->user_cpus_ptr);
			} else {
				conc_task_class_mask_now(t, ent);
			}
			if (!conc_mask_counts(t, ent))
				continue;
			if (!conc_recount_add(&masks, &n, &cap, ent)) {
				css_task_iter_end(&it);
				goto out;
			}
		}
		css_task_iter_end(&it);
	}
	for (i = 0; i < nr_extra; i++) {
		const struct cpumask *m = (const struct cpumask *)(extra + i * cpumask_size());

		if (cpumask_subset(cpu_possible_mask, m))
			continue;
		if (!conc_recount_add(&masks, &n, &cap, m))
			goto out;
	}
	width = conc_masks_width(masks, n, GFP_KERNEL);
out:
	conc_subtree_release(csses, nr_css);
	kfree(masks);
	free_cpumask_var(ent);
	return width;
}

static unsigned int conc_recount_width(struct task_group *tg)
{
	return __conc_recount_width(tg, NULL, NULL, NULL, 0, NULL, 0);
}

/*
 * The width of the masks registered at @conc, with one task's mask @del
 * taken out and @add put in when given (a change being judged).
 * Snapshot under the lock, computed outside it. UINT_MAX when it could
 * not be computed.
 */
static unsigned int conc_level_width(struct tg_concurrency *conc, const struct cpumask *del,
				   const struct cpumask *add, gfp_t gfp)
{
	unsigned int n, cap, width;
	struct conc_class *c;
	unsigned long flags;
	char *masks;
	bool have_add = false;

	cap = conc_nr_masks(conc) + 1;
retry:
	masks = kmalloc_array(cap, cpumask_size(), gfp);
	if (!masks)
		return UINT_MAX;
	n = 0;
	raw_spin_lock_irqsave(&conc->lock, flags);
	list_for_each_entry(c, &conc->masks, node) {
		if (del && c->tasks == 1 && cpumask_equal(conc_class_mask(c), del))
			continue;
		if (n == cap - 1) {
			/* Grew meanwhile. */
			raw_spin_unlock_irqrestore(&conc->lock, flags);
			kfree(masks);
			cap *= 2;
			goto retry;
		}
		cpumask_copy((struct cpumask *)(masks + n * cpumask_size()), conc_class_mask(c));
		if (add && cpumask_equal(conc_class_mask(c), add))
			have_add = true;
		n++;
	}
	raw_spin_unlock_irqrestore(&conc->lock, flags);
	if (add && !have_add)
		cpumask_copy((struct cpumask *)(masks + n++ * cpumask_size()), add);
	width = conc_masks_width(masks, n, gfp);
	kfree(masks);
	return width;
}

/*
 * The width of the masks a level allows: the effective limit, or the
 * effective ceiling when no limit applies, 0 when neither does.
 */
static __maybe_unused unsigned int conc_class_cap(struct tg_concurrency *conc)
{
	unsigned int cap = READ_ONCE(conc->effective), ceiling = READ_ONCE(conc->ceiling_effective);

	if (ceiling && (!cap || cap > ceiling))
		cap = ceiling;
	return cap;
}

/* The same against the requested limit, for sched_setaffinity(2). */
static __maybe_unused unsigned int conc_class_cap_requested(struct tg_concurrency *conc)
{
	unsigned int cap = READ_ONCE(conc->request), ceiling = READ_ONCE(conc->ceiling_effective);

	if (ceiling && (!cap || cap > ceiling))
		cap = ceiling;
	return cap;
}

/*
 * @tg is about to stop counting: stop its children from propagating their
 * slots into it first, releasing the entities they hold there, so that a
 * child's claim that finds @tg inactive finds its own propagation stopped
 * too (conc_claim_slow()). A child that counts on its own goes on doing so;
 * its list, which @tg is about to drop, is merged no more.
 */
static void conc_detach_children(struct task_group *tg, struct cpumask *scratch)
{
	struct task_group *child;
	unsigned long flags;
	bool was;
	int cpu;

	list_for_each_entry_rcu(child, &tg->children, siblings) {
		struct tg_concurrency *conc = &child->conc;

		raw_spin_lock_irqsave(&conc->lock, flags);
		was = conc->propagate;
		if (was) {
			cpumask_copy(scratch, conc->used);
			conc->propagate = false;
			conc->merged = false;
		}
		raw_spin_unlock_irqrestore(&conc->lock, flags);
		if (!was)
			continue;
		for_each_cpu(cpu, scratch)
			conc_release(tg, cpu);
	}
}

/*
 * Recompute the effective limit of every group in the subtree of @top:
 *
 *   effective = max(min(request, effective(parent)), width)
 *
 * and switch counting on or off accordingly. Walked in pre-order so that a
 * parent's state is final when its children are visited. Shrinking is
 * left to the caller.
 */
static void conc_update_tree(struct task_group *top, struct cpumask *scratch)
{
	struct cgroup_subsys_state **csses, *css;
	int nr_css, k;

	lockdep_assert_held(&conc_mutex);

	/*
	 * A walk that allocates, builds lists and visits every task: on a
	 * referenced snapshot of the subtree rather than under RCU. A group
	 * created meanwhile is walked by conc_tg_online() once linked in.
	 */
	if (conc_subtree_snapshot(top, &csses, &nr_css)) {
		pr_warn_once("conc: no memory to update the limits of css %d\n", top->css.id);
		return;
	}
	for (k = 0; k < nr_css; k++) {
		struct task_group *tg, *parent;
		css = csses[k];
		if (!css)
			continue;
		tg = css_tg(css);
		parent = READ_ONCE(tg->parent);
		struct tg_concurrency *conc = &tg->conc;
		unsigned int eff = conc->request, ceiling = conc->ceiling, was;
		bool should_active, new_prop, walk = false, teardown = false;
		unsigned long flags;
		int cpu, attach = 0;

		/*
		 * Not linked in yet (sched_online_group()): conc_tg_online()
		 * walks it once it is, and no task can join it before. Dying:
		 * no task can join it either, and conc_tg_offline() resets it.
		 */
		if (!parent || css_is_dying(css))
			continue;

		if (parent->conc.effective && (!eff || eff > parent->conc.effective))
			eff = parent->conc.effective;
		if (parent->conc.ceiling_effective && (!ceiling || ceiling > parent->conc.ceiling_effective))
			ceiling = parent->conc.ceiling_effective;
		/*
		 * A limit below the width of the masks is relaxed to it,
		 * once a recount of the tasks confirms it (a group starting to
		 * count has no list yet: the recount alone), and never past the
		 * ceiling, which the refusals keep the width within.
		 */
		if (eff) {
			unsigned int w = conc->active ? conc_level_width(conc, NULL, NULL, GFP_KERNEL) :
						      conc_recount_width(tg);

			if (w > eff)
				eff = max(eff, conc->active ? min(w, conc_recount_width(tg)) : w);
			if (ceiling && eff > ceiling)
				eff = ceiling;
		}
		/* A ceiling alone caps the width, see conc_check_affinity(). */
		should_active = eff || ceiling || parent->conc.active;
		new_prop = should_active && parent->conc.active;
		if (!should_active && conc->active)
			conc_detach_children(tg, scratch);

		raw_spin_lock_irqsave(&conc->lock, flags);
		was = conc_report_ceiling(conc);
		conc->effective = eff;
		conc->ceiling_effective = ceiling;
		if (should_active && !conc->active) {
			conc->active = true;
			walk = true;
		} else if (!should_active && conc->active) {
			conc->active = false;
			teardown = true;
		}
		if (conc->propagate != new_prop) {
			/*
			 * Attach to or detach from the parent as entities. The
			 * slots are snapshotted together with the flag flip, so
			 * that a concurrent slot add either propagates itself
			 * or is in the snapshot; slot removal holds conc_mutex.
			 * Group locks never nest, so this happens below.
			 */
			cpumask_copy(scratch, conc->used);
			conc->propagate = new_prop;
			attach = new_prop ? 1 : -1;
		}
		/*
		 * A newly counting group has an empty list, merged as it is
		 * when its parent counts. One its parent no longer counts is
		 * merged no more: the parent dropped its list. A counting group
		 * its parent starts counting is merged by the copy below.
		 */
		if (walk || !new_prop)
			conc->merged = new_prop;
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
		/*
		 * The parent's list follows: a group that was counting already
		 * is merged or unmerged here. A newly counting group builds its
		 * list below, registering its tasks at every counting level,
		 * the parent's included, since a group starts counting only
		 * along with its counting ancestors: merging that list too
		 * would count its tasks twice above it, and the copy an exit
		 * leaves behind would be a class no task has, keeping the
		 * cleaner off the slot it alone seems to hit.
		 */
		if (attach && !walk) {
			int n = attach > 0 ? conc_nr_masks(conc) * conc_merge_depth(tg) : 0;
			struct conc_class **pre = conc_class_prealloc(n, GFP_KERNEL);

			/*
			 * Every counting level from the parent up counts the
			 * subtree now, the levels the parent's list is merged
			 * into: the ones that started counting in this walk
			 * built their lists before this group's turn, from
			 * their own tasks. A group stops propagating only when
			 * its parent stops counting, which drops the parent's
			 * list: then nothing above counts it.
			 */
			conc_classes_merge(tg, attach, pre, n);
			conc_class_prealloc_free(pre, n);
		}

		if (walk) {
			conc_activate_tasks(tg);
			conc_classes_build(tg);
		}
		if (teardown) {
			conc_deactivate_tasks(tg);
			conc_classes_teardown(tg);
		}
		/* Classes built or merged here may lack their headroom. */
		if (conc->active && conc->effective &&
		    READ_ONCE(conc->nr_used) + READ_ONCE(conc->unhit) > conc->effective)
			conc_want_headroom(conc);
	}
	conc_subtree_release(csses, nr_css);
}

/* Raise cgroup_file_notify() where the reported effective value changed. */
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
 * Eviction: empty a CPU of a group's presence without ever letting the
 * group be present on more CPUs than before, which is what keeps the
 * concurrency IDs of its processes below their bound.
 *
 * Two mechanisms:
 *
 *  - join: a task moves to a CPU its own group already uses. No slot is
 *    added, so the group's CPU count never grows. Used for shrinking.
 *
 *  - set-move: the whole presence of a subtree on a CPU is relocated to
 *    one new CPU under stop_two_cpus(): the tasks are moved, the slots
 *    relabelled and the CPU-owned mm CIDs transferred in one step while
 *    neither CPU runs anything else. Used when a CPU must be vacated
 *    (hot-unplug, an affinity change) and its tasks cannot join. The
 *    innermost group is exact; an active parent outside the moved
 *    subtree sees one transient extra CPU between the reservation of
 *    the destination and the stopper.
 *
 * Arrivals on the CPU are stopped first by making its slot values
 * negative, then synchronize_rcu() lets the claims already made finish
 * their enqueue (they run with interrupts disabled from claim to enqueue)
 * and the stopper flushes the wakelist, so that the stopper sees every
 * task that holds a slot on the CPU.
 */

/*
 * A used CPU of @p's own group that @p may join when evicted from @cpu,
 * judged by @mask when given (an affinity change in progress), and
 * @within when given.
 */
static int conc_join_dest(struct task_struct *p, int cpu, const struct cpumask *mask,
			const struct cpumask *within)
{
	struct task_group *tg;
	int c, best = -1;

	guard(rcu)();
	tg = task_group(p);
	for_each_cpu_and(c, tg->conc.used, mask ?: p->cpus_ptr) {
		if (c == cpu || !conc_cpu_usable(c))
			continue;
		if (within && !cpumask_test_cpu(c, within))
			continue;
		if (atomic_read(per_cpu_ptr(tg->conc.present, c)) < 0)
			continue;	/* in a transient state */
		if (available_idle_cpu(c))
			return c;
		if (best < 0)
			best = c;
	}
	return best;
}

/* Mark the (@tg, @cpu) slot evicting; false if it is in another transient state. */
static bool conc_mark_evicting(struct task_group *tg, int cpu)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, cpu);
	unsigned long flags;
	bool ok;

	raw_spin_lock_irqsave(&conc->lock, flags);
	ok = atomic_read(slot) >= 0;
	if (ok) {
		atomic_sub(CONC_SLOT_EVICT_BIAS, slot);
		conc->nr_evicting++;
	}
	raw_spin_unlock_irqrestore(&conc->lock, flags);
	return ok;
}

static void conc_unmark_evicting(struct task_group *tg, int cpu)
{
	struct tg_concurrency *conc = &tg->conc;
	unsigned long flags;

	raw_spin_lock_irqsave(&conc->lock, flags);
	atomic_add(CONC_SLOT_EVICT_BIAS, per_cpu_ptr(conc->present, cpu));
	conc->nr_evicting--;
	raw_spin_unlock_irqrestore(&conc->lock, flags);
}

/*
 * The mask @t is entitled to: its cpuset's, masked by its requested mask
 * as __set_cpus_allowed_ptr() does for a change from the kernel side. A
 * cpuset change reaches the tasks one at a time; this is ahead of the
 * ones it has not reached, and lets a presence move with the cpuset as
 * a whole.
 */
static void __conc_task_entitled(struct task_struct *t, const struct cpumask *user,
			       struct cpumask *mask)
{
	scoped_guard(rcu)
		cpuset_task_cpus_rcu(t, mask);
	if (user && cpumask_intersects(mask, user))
		cpumask_and(mask, mask, user);
}

static __maybe_unused void conc_task_entitled(struct task_struct *t, struct cpumask *mask)
{
	guard(raw_spinlock_irq)(&t->pi_lock);
	__conc_task_entitled(t, t->user_cpus_ptr, mask);
}

/*
 * Tasks whose mask the limit never changes on its own: kernel threads and
 * the tasks nothing may set the affinity of, and deadline tasks, whose
 * bandwidth is admitted per root domain and would have to be checked
 * again for a mask reaching another (dl_task_check_affinity()).
 */
static inline bool conc_task_mask_fixed(struct task_struct *t)
{
	return t->flags & (PF_KTHREAD | PF_NO_SETAFFINITY) || dl_task(t);
}

/*
 * Make headroom in @tg for one more slot, on @cpu: drain idle slots, or
 * evict used CPUs whose tasks can all join, until the slot fits within
 * the limit with the headroom of the unhit classes kept, see
 * __conc_room_for(). A slot that is the only used CPU hitting a class is
 * kept, as the cleaner keeps it. Returns true with headroom.
 */
static __maybe_unused bool conc_make_headroom(struct task_group *tg, int cpu)
{
	struct tg_concurrency *conc = &tg->conc;
	cpumask_var_t cpus;
	int c;

	lockdep_assert_held(&conc_mutex);

	if (conc_room_for(conc, cpu))
		return true;
	if (!alloc_cpumask_var(&cpus, GFP_KERNEL))
		return false;
	cpumask_copy(cpus, conc->used);
	/* Idle at this level, or idle through idle children: drain. */
	for_each_cpu(c, cpus) {
		if (atomic_read(per_cpu_ptr(conc->present, c)) < 0 || conc_slot_sole_hitter(conc, c))
			continue;
		if (!conc_drain_cpu(tg, c) && conc_room_for(conc, cpu))
			goto out;
	}
	for_each_cpu(c, cpus) {
		if (atomic_read(per_cpu_ptr(conc->present, c)) <= 0 || conc_slot_sole_hitter(conc, c))
			continue;
		if (!conc_evict_cpu(tg, c, false, false, NULL, NULL, NULL) && conc_room_for(conc, cpu))
			goto out;
	}
out:
	free_cpumask_var(cpus);
	return conc_room_for(conc, cpu);
}

static int conc_resv_cpu(struct conc_resv *resv)
{
	return resv->cpu;
}

static void conc_hold_irq_work_fn(struct irq_work *work)
{
	mod_delayed_work(system_dfl_wq, &conc_hold_work, 0);
}

/* An event that may let a held task in: run the placement work. */
void conc_hold_kick(void)
{
	if (atomic_read(&conc_nr_held))
		irq_work_queue(&conc_hold_irq_work);
}

#ifndef CONFIG_SCHED_MAX_CONCURRENCY_DEBUG
static inline void conc_debug_hold_start(struct task_struct *p) { }

static inline void conc_debug_hold_stop(struct task_struct *p) { }

static inline void conc_debug_hold_end(struct task_struct *p, const char *how) { }

static inline void conc_debug_hold_arm(void) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY_DEBUG */

/*
 * Hold @p at its next return to user space, for @why. Under p->pi_lock or
 * task_rq(p)->lock. False when @p never returns there (a kernel thread, a
 * task on its way out): it runs on as it is, without a slot, and takes no
 * concurrency ID that way. A kernel thread is the user of no domain. A
 * task is on its way out from exit_signals() on, which do_exit() calls
 * after sched_mm_cid_exit() made it leave its domains: mm_cid_schedin()
 * and ns_cid_schedin() find it inactive, so that it can neither take an ID
 * below the bound from the threads that need one nor show one to user
 * space, which it never returns to.
 */
bool conc_hold_request(struct task_struct *p, u8 why)
{
	if (p->conc_hold_pending) {
		p->conc_hold_pending |= why;
		return true;
	}
	if (p->flags & (PF_KTHREAD | PF_EXITING))
		return false;
	p->conc_hold_pending = why;
	if (task_work_add(p, &p->conc_hold_work, TWA_RESUME)) {
		p->conc_hold_pending = 0;
		return false;
	}
	return true;
}

/*
 * The IDs @p, current, took none of while a hold was pending. Given
 * outside a context switch, they reach user space only through the rseq
 * update forced here: the exit to user space writes changed IDs after a
 * context switch only, and clears the change otherwise.
 */
static void conc_cid_refresh(struct task_struct *p)
{
#ifdef CONFIG_SCHED_MM_CID
	mm_cid_schedin(p);
	rseq_force_update();
#endif
}

/*
 * Hold @p, current, with task_rq_lock() held: out of its class, queued
 * for the core, in TASK_RUNNING. A wakeup leaves it alone, it is not
 * asleep; the placement work enqueues it again.
 */
static void conc_hold(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_held(&p->pi_lock);
	lockdep_assert_rq_held(rq);
	WARN_ON_ONCE(p != current || p->conc_held || !task_on_rq_queued(p));

	/* The class only; the core's accounting stays, see dequeue_task(). */
	WARN_ON_ONCE(!p->sched_class->dequeue_task(rq, p, DEQUEUE_SLEEP | DEQUEUE_SPECIAL |
						     DEQUEUE_NOCLOCK));
	conc_task_release(p, cpu_of(rq));
	p->conc_held = 1;
	conc_debug_hold_start(p);
	get_task_struct(p);
	atomic_inc(&conc_nr_held);
	/* The placement work may have listed it already, see conc_hold_work_fn(). */
	scoped_guard(raw_spinlock, &conc_hold_lock) {
		if (list_empty(&p->conc_hold_node))
			list_add_tail(&p->conc_hold_node, &conc_held_list);
	}
	/* Room may exist already, elsewhere in its mask. */
	conc_hold_kick();
	resched_curr(rq);
}

/*
 * A held task blocks, from dequeue_task() with DEQUEUE_SLEEP under the
 * runqueue lock: a plain sleeper from here on, whose wakeup places it.
 */
void conc_held_blocks(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_rq_held(rq);
	p->conc_held = 0;
	conc_debug_hold_stop(p);
	conc_debug_hold_end(p, "blocked");
	scoped_guard(raw_spinlock, &conc_hold_lock)
		list_del_init(&p->conc_hold_node);
	if (atomic_dec_and_test(&conc_nr_held))
		swake_up_one(&conc_held_wq);
	put_task_struct(p);
}

/*
 * Return held @p to its class on its runqueue, as woken up there, under
 * that runqueue's lock. The class saw it go to sleep at the hold; a
 * wakeup where it went to sleep is what keeps the class's own
 * bookkeeping (deadline bandwidth and timers, load tracking) right for
 * whatever the core does with the task next, which the class then sees
 * done to a queued task: a migration's dequeue and enqueue, a class
 * change's switched_from() and switched_to(). From dequeue_task() for
 * anything but a sleep, with @rehold: the task is asked to hold again and
 * goes on to its return to user space without a slot. From
 * conc_try_place(), which has a slot for it, or with @rehold for a task
 * with SIGKILL pending, which goes without one.
 */
void conc_unhold(struct rq *rq, struct task_struct *p, bool rehold)
{
	int on_rq = p->on_rq;

	lockdep_assert_rq_held(rq);
	/* Queued for the core: the class sees a wakeup, see deactivate_task(). */
	WARN_ON_ONCE(on_rq != TASK_ON_RQ_QUEUED);
	p->conc_held = 0;
	conc_debug_hold_stop(p);
	p->sched_class->enqueue_task(rq, p, ENQUEUE_WAKEUP | ENQUEUE_NOCLOCK);
	/*
	 * A wakeup tells the CPU, a migration its destination. A change made
	 * in place (a cpuset widened by a CPU coming online, a priority
	 * change) tells nobody: an idle CPU would leave the task queued, held
	 * no more, for good.
	 */
	if (!task_current(rq, p))
		resched_curr(rq);
	scoped_guard(raw_spinlock, &conc_hold_lock)
		list_del_init(&p->conc_hold_node);
	if (atomic_dec_and_test(&conc_nr_held))
		swake_up_one(&conc_held_wq);
	if (rehold)
		conc_hold_request(p, CONC_HOLD_UNPLACED);
	else
		conc_debug_hold_end(p, "placed");
	put_task_struct(p);
}

/*
 * Task work at the return to user space, see conc_hold_request(). A user
 * worker (io-wq's, vhost's) never returns there, but runs that work at
 * the top of its loop: it is held there like the rest of its process.
 */
void conc_hold_task_work(struct callback_head *work)
{
	struct task_struct *p = container_of(work, struct task_struct, conc_hold_work);
	u8 why;

	WARN_ON_ONCE(p != current);
	if (p->flags & PF_EXITING) {
		if (IS_ENABLED(CONFIG_SCHED_MAX_CONCURRENCY_DEBUG)) {
			scoped_guard(task_rq_lock, p)
				conc_debug_hold_end(p, "exiting");
		}
		return;
	}
	scoped_guard(task_rq_lock, p) {
		why = p->conc_hold_pending;
		if (!why)
			return;
		/* Never held with SIGKILL pending, see conc_fatal_signal(). */
		if (fatal_signal_pending(p)) {
			conc_debug_hold_end(p, "killed");
			return;
		}
		p->conc_hold_pending = 0;
		/*
		 * Placed meanwhile (a load-balancer move, a group change, a
		 * set-move), or its CPU has room now: it stays, with IDs,
		 * unless that CPU is being emptied (CONC_HOLD_EVICT).
		 */
		if (!(why & CONC_HOLD_EVICT) &&
		    (!conc_task_active(p) || p->conc_accounted || conc_task_claim(p, task_cpu(p)))) {
			conc_debug_hold_end(p, "placed at its return");
			conc_cid_refresh(p);
			return;
		}
		update_rq_clock(scope.rq);
		conc_hold(scope.rq, p);
	}
}

/**
 * conc_fatal_signal - SIGKILL ends a hold
 * @t: a task SIGKILL was queued for, from signal_wake_up_state() under its
 *     siglock
 *
 * A held task is not asleep: the signal's wakeup leaves it alone, and it
 * would die only once a slot let it in, possibly never when the masks
 * went past the bound unchecked (the rmdir of a partition root, see
 * Documentation/scheduler/sched-max-concurrency.rst). Instead it is never
 * held with SIGKILL pending (conc_hold_task_work()), and a held one is let
 * go: the placement work returns it to its class where it is, without a
 * slot (conc_try_place()). Either way its hold stays pending, so that it
 * takes no ID (conc_task_unplaced()), and the exit to user space delivers
 * the signal before it leaves; the rseq update the exit may run before
 * that, right after the task works on architectures without the generic
 * entry code, skips a task with SIGKILL pending
 * (rseq_slowpath_update_usr()).
 * do_exit() then takes it out of its ID domains. What remains is a CPU it
 * may run on beyond the bound, without an ID, as a task on its way out
 * does.
 *
 * Serialized with the hold by p->pi_lock: either the task work reads
 * SIGKILL, or this reads conc_held set.
 */
void conc_fatal_signal(struct task_struct *t)
{
	if (!static_branch_unlikely(&sched_conc_enabled))
		return;
	guard(raw_spinlock_irqsave)(&t->pi_lock);
	if (t->conc_held && fatal_signal_pending(t))
		conc_hold_kick();
}

/*
 * Place @p, held: a selection as for a wakeup, under p->pi_lock alone as
 * in try_to_wake_up(), since the selection may change the task's mask
 * under the runqueue lock (a fallback widening, which returns the task to
 * its class, see conc_select_account()); then the runqueue, or both as in
 * move_queued_task(). Returns 1 when @p was placed or is held no more, 0
 * when it stays held, -EAGAIN when it is still on its CPU, in its last
 * instructions before it schedules out.
 */
static int conc_try_place(struct task_struct *p)
{
	int wake_flags = WF_TTWU, cpu, ret;
	struct rq_flags rf;
	struct rq *rq;

	guard(raw_spinlock_irqsave)(&p->pi_lock);

	if (!p->conc_held)
		return 1;
	if (smp_load_acquire(&p->on_cpu))
		return -EAGAIN;
	/*
	 * Let go where it is, without a slot, see conc_fatal_signal(); from a
	 * fallback CPU when its own went offline while it was held, as
	 * balance_push() would have pushed it. Held, out of its class, it
	 * does not move under its runqueue lock alone: task_cpu() is stable.
	 */
	if (fatal_signal_pending(p)) {
		cpu = cpu_active(task_cpu(p)) ? -1 : select_fallback_rq(task_cpu(p), p);
		rq = __task_rq_lock(p, &rf);
		if (p->conc_held) {
			update_rq_clock(rq);
			conc_unhold(rq, p, true);
			if (cpu >= 0 && !cpu_active(cpu_of(rq)))
				rq = __migrate_task(rq, &rf, p, cpu, true);
			else
				wakeup_preempt(rq, p, 0);
		}
		rq_unlock(rq, &rf);
		return 1;
	}
	/*
	 * No active CPU in its mask: the last one went offline while it was
	 * held, out of its class. balance_push() pushes the queued tasks of a
	 * dying CPU, and the unplug widens the tasks of the groups using it
	 * that are present on it (conc_widen_pinned()); neither sees a held
	 * task, which may sit on another runqueue, or belong to a group with
	 * no slot there. Its cpuset's hotplug pass gives it the cpuset's CPUs
	 * later, and in the top cpuset none: it would wait for the CPU to
	 * come back. Queued without a limit, it would have been widened by
	 * select_fallback_rq() and pushed: do the same. The widening changes
	 * the mask of a queued task, which returns it to its class where it
	 * is and asks for the hold again (conc_unhold()). A runqueue no longer
	 * active does not run it: it moves, without a slot if need be, as
	 * balance_push() moves it, and is held again at its return to user
	 * space, from where it is placed by its new mask.
	 */
	if (!cpumask_intersects(p->cpus_ptr, cpu_active_mask)) {
		cpu = select_fallback_rq(task_cpu(p), p);
		rq = __task_rq_lock(p, &rf);
		if (task_on_rq_queued(p) && !p->conc_held && !cpu_active(cpu_of(rq))) {
			update_rq_clock(rq);
			rq = __migrate_task(rq, &rf, p, cpu, true);
		}
		rq_unlock(rq, &rf);
		return 1;
	}
	cpu = select_task_rq(p, task_cpu(p), &wake_flags);
	if (cpu < 0)
		return 0;

	rq = __task_rq_lock(p, &rf);
	if (!p->conc_held) {
		/*
		 * Blocked meanwhile, a sleeper whose wakeup places it: give the
		 * slot back. Or unheld by the selection's own widening, which
		 * gave it none.
		 */
		conc_task_release(p, cpu);
		ret = 1;
		goto out;
	}
	update_rq_clock(rq);
	/* Woken where it is, then moved as move_queued_task() moves a queued task. */
	conc_unhold(rq, p, false);
	if (cpu != cpu_of(rq)) {
		deactivate_task(rq, p, DEQUEUE_NOCLOCK);
		set_task_cpu(p, cpu);
		rq_unlock(rq, &rf);
		rq = cpu_rq(cpu);
		rq_lock(rq, &rf);
		update_rq_clock(rq);
		activate_task(rq, p, ENQUEUE_NOCLOCK);
		wake_flags |= WF_MIGRATED;
	}
	wakeup_preempt(rq, p, wake_flags);
	ret = 1;
out:
	rq_unlock(rq, &rf);
	return ret;
}

static void conc_hold_work_fn(struct work_struct *work)
{
	bool again = false;
	int todo;

	for (todo = atomic_read(&conc_nr_held); todo > 0; todo--) {
		struct task_struct *p = NULL;
		cpumask_var_t mask;
		int ret;

		scoped_guard(raw_spinlock_irq, &conc_hold_lock) {
			p = list_first_entry_or_null(&conc_held_list, struct task_struct, conc_hold_node);
			if (p) {
				list_del_init(&p->conc_hold_node);
				get_task_struct(p);
			}
		}
		if (!p)
			break;
		ret = conc_try_place(p);
		if (!ret && alloc_cpumask_var(&mask, GFP_KERNEL)) {
			struct task_group *tg = conc_pin_task_group(p);

			/*
			 * Nothing fits: make a CPU of its mask reachable, as an
			 * affinity change does for a sleeper, and try once more.
			 */
			if (tg) {
				scoped_guard(raw_spinlock_irq, &p->pi_lock)
					cpumask_copy(mask, p->cpus_ptr);
				mutex_lock(&conc_mutex);
				conc_make_reachable(tg, mask);
				mutex_unlock(&conc_mutex);
				css_put(&tg->css);
				ret = conc_try_place(p);
			}
			free_cpumask_var(mask);
		}
		if (ret <= 0) {
			/*
			 * Still held, or held again meanwhile: unheld by a
			 * migration and held at its return to user space, in
			 * which case conc_hold() lists it, maybe before this.
			 */
			scoped_guard(raw_spinlock_irq, &conc_hold_lock) {
				if (READ_ONCE(p->conc_held) && list_empty(&p->conc_hold_node))
					list_add_tail(&p->conc_hold_node, &conc_held_list);
			}
			if (ret < 0)
				again = true;
		}
		put_task_struct(p);
	}
	if (again)
		mod_delayed_work(system_dfl_wq, &conc_hold_work, 1);
	conc_debug_hold_arm();
}

/* Hold @t, if it is present on @cpu as a task of @tg. */
static bool conc_hold_task_on(struct task_struct *t, struct task_group *tg, int cpu)
{
	guard(raw_spinlock_irq)(&t->pi_lock);
	if (task_cpu(t) != cpu || !READ_ONCE(t->conc_accounted) || task_group(t) != tg)
		return true;
	return conc_hold_request(t, CONC_HOLD_EVICT);
}

/*
 * Evict by holding: the tasks of @tg's subtree present on @cpu that could
 * neither join nor be set-moved are held at their next return to user
 * space, with arrivals on the CPU refused meanwhile, until the slot is
 * empty and drained. -EBUSY when one cannot be held (a kernel thread) or
 * did not get there in time; the slot stays, shrink_pending with it.
 */
static int conc_hold_cpu(struct task_group *tg, int cpu)
{
	struct cgroup_subsys_state *css;
	struct task_group **groups;
	int i, n = 0, nr = 0, marked, round, ret = -EBUSY;

	lockdep_assert_held(&conc_mutex);

	rcu_read_lock();
	css_for_each_descendant_pre(css, &tg->css)
		if (cpumask_test_cpu(cpu, css_tg(css)->conc.used))
			nr++;
	rcu_read_unlock();
	if (!nr)
		return conc_drain_cpu(tg, cpu);
	groups = kvcalloc(nr, sizeof(*groups), GFP_KERNEL);
	if (!groups)
		return -ENOMEM;
	rcu_read_lock();
	css_for_each_descendant_pre(css, &tg->css) {
		if (!cpumask_test_cpu(cpu, css_tg(css)->conc.used))
			continue;
		if (n == nr || !css_tryget_online(css))
			break;
		groups[n++] = css_tg(css);
	}
	rcu_read_unlock();

	for (round = 0; round < CONC_EVICT_ROUNDS; round++) {
		bool ok = true;

		/* Stop arrivals at every level, the top first. */
		for (marked = 0; marked < n; marked++) {
			if (!conc_mark_evicting(groups[marked], cpu))
				break;
		}
		if (marked == n) {
			for (i = 0; i < n && ok; i++) {
				struct css_task_iter it;
				struct task_struct *t;

				css_task_iter_start(&groups[i]->css, 0, &it);
				while ((t = css_task_iter_next(&it))) {
					if (!conc_hold_task_on(t, groups[i], cpu))
						ok = false;
				}
				css_task_iter_end(&it);
			}
			if (ok)
				schedule_timeout_uninterruptible(1);
		}
		for (i = 0; i < marked; i++)
			conc_unmark_evicting(groups[i], cpu);
		if (marked < n || !ok) {
			ret = -EBUSY;
			break;
		}
		ret = conc_drain_cpu(tg, cpu);
		if (!ret)
			break;
	}
	for (i = 0; i < n; i++)
		css_put(&groups[i]->css);
	kvfree(groups);
	return ret;
}

/*
 * Empty @cpu of the presence of @tg's subtree: tasks join used CPUs of
 * their own group, with @set_move the remainder is set-moved to a new
 * CPU, taken from @within when given, and with @hold what is left is
 * held off the CPUs, see conc_hold_cpu(). @mask_task, when given, is
 * judged by @mask: an affinity change in progress. Returns -EBUSY and
 * leaves the slot in place when nothing works.
 */
static int conc_evict_cpu(struct task_group *tg, int cpu, bool set_move, bool hold,
			struct task_struct *mask_task, const struct cpumask *mask,
			const struct cpumask *within)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, cpu);
	cpumask_var_t scratch;
	int ret = -EBUSY, v, round, remaining = 0;

	lockdep_assert_held(&conc_mutex);

	v = atomic_read(slot);
	if (v == CONC_SLOT_UNUSED || v == CONC_SLOT_DRAINING)
		return 0;
	/*
	 * An idle slot is drained at once. A waking task claims a slot
	 * without the lock and may take it first: evict what it brought then,
	 * as from a busy slot. Giving up instead, a shrink that lost this
	 * race on every CPU it tried left its group over the limit with no
	 * task held, and nothing to try again.
	 */
	if (v == 0) {
		if (!conc_drain_cpu(tg, cpu))
			return 0;
		v = atomic_read(slot);
		if (v == CONC_SLOT_UNUSED || v == CONC_SLOT_DRAINING)
			return 0;
	}
	if (v < 0)
		return -EBUSY;
	if (!conc_mark_evicting(tg, cpu))
		return -EBUSY;

	/* Join: move each task to a CPU its group already uses. */
	for (round = 0; round < CONC_EVICT_ROUNDS; round++) {
		struct cgroup_subsys_state *css;

		remaining = 0;
		rcu_read_lock();
		css_for_each_descendant_pre(css, &tg->css) {
			struct css_task_iter it;
			struct task_struct *t;

			if (!cpumask_test_cpu(cpu, css_tg(css)->conc.used))
				continue;
			if (!css_tryget_online(css))
				continue;
			rcu_read_unlock();
			css_task_iter_start(css, 0, &it);
			while ((t = css_task_iter_next(&it))) {
				struct migration_arg arg = { .task = t };
				int dest;

				/* Present: a sleeper holds no slot and is not moved. */
				if (task_cpu(t) != cpu || !READ_ONCE(t->conc_accounted))
					continue;
				dest = conc_join_dest(t, cpu, t == mask_task ? mask : NULL, within);
				if (dest < 0) {
					remaining++;
					continue;
				}
				arg.dest_cpu = dest;
				stop_one_cpu(cpu, migration_cpu_stop, &arg);
			}
			css_task_iter_end(&it);
			rcu_read_lock();
			css_put(css);
		}
		rcu_read_unlock();

		/* Everything joined: the slots left are idle and can be drained. */
		if (!remaining) {
			conc_unmark_evicting(tg, cpu);
			ret = conc_drain_cpu(tg, cpu);
			if (!ret)
				return 0;
			/* A straggler or an idle child slot that is not idle after all. */
			if (!conc_mark_evicting(tg, cpu))
				return -EBUSY;
			schedule_timeout_uninterruptible(1);
			continue;
		}
		break;
	}

	conc_unmark_evicting(tg, cpu);
	if (!remaining)
		return ret;
	if (!alloc_cpumask_var(&scratch, GFP_KERNEL))
		return -ENOMEM;

	/*
	 * Consolidate: a child subtree whose tasks could not join within the
	 * child is set-moved onto a CPU this level already uses, which adds
	 * nothing at this level. Then the children are checked again.
	 */
	for (round = 0; round < CONC_EVICT_ROUNDS; round++) {
		struct task_group *child, *pinned = NULL;

		rcu_read_lock();
		list_for_each_entry_rcu(child, &tg->children, siblings) {
			if (!cpumask_test_cpu(cpu, child->conc.used) ||
			    !css_tryget_online(&child->css))
				continue;
			pinned = child;
			break;
		}
		rcu_read_unlock();
		if (!pinned)
			break;
		v = conc_set_move_dest(pinned, cpu, scratch, tg->conc.used, mask_task, mask);
		ret = v >= 0 ? conc_set_move(pinned, cpu, v, mask_task, mask) : -EBUSY;
		css_put(&pinned->css);
		if (ret)
			break;
	}
	ret = conc_drain_cpu(tg, cpu);
	if (!ret) {
		free_cpumask_var(scratch);
		return 0;
	}
	if (!set_move) {
		free_cpumask_var(scratch);
		return hold ? conc_hold_cpu(tg, cpu) : -EBUSY;
	}

	/* Set-move: relocate what could not join, all at once. */
	v = conc_set_move_dest(tg, cpu, scratch, within, mask_task, mask);
	free_cpumask_var(scratch);
	if (v >= 0 && !conc_set_move(tg, cpu, v, mask_task, mask))
		return 0;
	/* No common destination: hold what is left, when the caller wants that. */
	return hold ? conc_hold_cpu(tg, cpu) : -EBUSY;
}

/*
 * Evict CPUs until @tg is within its effective limit, emptiest first, by
 * joining, then by holding the tasks that cannot join (a set-move would
 * not reduce the count). Leaves shrink_pending set when neither works.
 */
static void conc_shrink(struct task_group *tg)
{
	struct tg_concurrency *conc = &tg->conc;
	unsigned long flags;
	cpumask_var_t tried;

	lockdep_assert_held(&conc_mutex);

	if (!zalloc_cpumask_var(&tried, GFP_KERNEL))
		goto out;

	while (conc->active && conc->effective && READ_ONCE(conc->nr_used) > conc->effective) {
		int cpu, victim = -1, best = INT_MAX;

		for_each_cpu(cpu, conc->used) {
			int v = atomic_read(per_cpu_ptr(conc->present, cpu));

			if (v < 0 || cpumask_test_cpu(cpu, tried))
				continue;
			if (v < best) {
				best = v;
				victim = cpu;
			}
		}
		if (victim < 0)
			break;
		cpumask_set_cpu(victim, tried);
		conc_evict_cpu(tg, victim, false, true, NULL, NULL, NULL);
	}
	free_cpumask_var(tried);
out:
	raw_spin_lock_irqsave(&conc->lock, flags);
	conc->shrink_pending = conc->effective && conc->nr_used > conc->effective;
	raw_spin_unlock_irqrestore(&conc->lock, flags);
}

/* Shrink every group of the subtree that is over its limit, innermost first. */
static void conc_shrink_tree(struct task_group *top)
{
	struct cgroup_subsys_state *css;

	lockdep_assert_held(&conc_mutex);

	rcu_read_lock();
	css_for_each_descendant_post(css, &top->css) {
		struct task_group *tg = css_tg(css);

		if (!tg->conc.active || !tg->conc.effective ||
		    READ_ONCE(tg->conc.nr_used) <= tg->conc.effective)
			continue;
		/* The reference pins the position across the sleep. */
		if (!css_tryget_online(css))
			continue;
		rcu_read_unlock();
		conc_shrink(tg);
		rcu_read_lock();
		css_put(css);
	}
	rcu_read_unlock();
}

/*
 * Retry events, see the design document: re-evaluate the limits covering
 * @tg after an event that can change feasibility. @tg must be pinned.
 */
static void __conc_retry(struct task_group *tg)
{
	struct task_group *top = tg;
	struct cpumask *scratch;

	lockdep_assert_held(&conc_mutex);

	/* The topmost active group: effective values flow down from there. */
	if (!top->conc.active)
		return;
	while (top->parent && top->parent->conc.active)
		top = top->parent;

	scratch = conc_alloc_scratch();
	if (!scratch)
		return;
	conc_update_tree(top, scratch);
	conc_shrink_tree(top);
	conc_bounds_apply(top);
	conc_notify_tree(top);
	kfree(scratch);
	conc_hold_kick();
}

static __maybe_unused void conc_retry(struct task_group *tg)
{
	if (!READ_ONCE(tg->conc.active))
		return;
	mutex_lock(&conc_mutex);
	__conc_retry(tg);
	mutex_unlock(&conc_mutex);
}

/*
 * The usable CPUs changed: the live part of every mask follows, and the
 * classes and the unhit ones are counted again from them. A sleeping task
 * pinned to CPUs that went away keeps its mask, as it would without a
 * limit, and is no class meanwhile (select_fallback_rq() widens it if it
 * wakes up before they come back); the masks around it may be classes
 * now, and the ones that come back ask for their headroom. One whose
 * tasks all run outside it stays dormant until cpuset gives them its CPUs
 * back, see conc_task_set_outside(). Every counting
 * group keeps its own list, so each one is visited. The configured masks,
 * by which changes are admitted, are untouched: the width of their live
 * parts never exceeds theirs, so the classes stay within the limit.
 */
static __maybe_unused void conc_refresh_live(void)
{
	struct task_group *tg;

	rcu_read_lock();
	list_for_each_entry_rcu(tg, &task_groups, list) {
		struct tg_concurrency *conc = &tg->conc;
		struct conc_class *c;
		unsigned long flags;

		if (!READ_ONCE(conc->active))
			continue;
		raw_spin_lock_irqsave(&conc->lock, flags);
		list_for_each_entry(c, &conc->masks, node)
			conc_class_set_live(c);
		conc_classes_recount(conc);
		raw_spin_unlock_irqrestore(&conc->lock, flags);
	}
	rcu_read_unlock();
}

/* Pin @p's group for a retry; NULL when it does not count. */
static struct task_group *conc_pin_task_group(struct task_struct *p)
{
	struct task_group *tg;

	if (!static_branch_unlikely(&sched_conc_enabled))
		return NULL;
	guard(rcu)();
	tg = READ_ONCE(p->sched_task_group);
	if (!READ_ONCE(tg->conc.active) || !css_tryget_online(&tg->css))
		return NULL;
	return tg;
}

/*
 * A cpuset change, from cpuset_update_tasks_cpumask() before it reaches
 * the tasks one at a time: the presence of every counting subtree under
 * the cpuset moves into the new mask as a whole first, judged by the
 * entitled masks (see conc_task_entitled()), so that each task's own change
 * then finds a used CPU of its mask and no task wakes up to none. The
 * registered masks change as a whole too: every task is registered with
 * the mask it is configured to run on with the cpuset as written, so
 * that the old and the new masks are never both counted with the limit
 * relaxed to their width in between. A hotplug event comes this way and
 * changes no registered mask.
 */
void conc_cpuset_update(struct cgroup_subsys_state *css, const struct cpumask *cpus)
{
	struct task_group **tops = NULL;
	struct css_task_iter it;
	struct task_struct *t;
	cpumask_var_t outside, ent;
	int i, n = 0, cap = 0, cpu;
	bool have_ent;

	if (!static_branch_unlikely(&sched_conc_enabled))
		return;

	have_ent = alloc_cpumask_var(&ent, GFP_KERNEL);
	/*
	 * The topmost counting groups of the tasks, each once, and the
	 * classes switched to the new masks as a whole: under the
	 * configuration mutex, so that no retry sees old and new mixed.
	 */
	mutex_lock(&conc_mutex);
	css_task_iter_start(css, 0, &it);
	while ((t = css_task_iter_next(&it))) {
		struct task_group *tg = conc_pin_task_group(t), *top;

		if (!tg)
			continue;
		if (have_ent && !kthread_is_per_cpu(t)) {
			struct conc_class **pre = NULL;
			int npre = 0;

			/*
			 * The mask it is configured to run on with the cpuset
			 * as written, see __conc_task_class_mask(): unchanged by a
			 * hotplug event, which reaches the tasks this way too.
			 */
			/*
			 * Its outside state follows the mask the pass applies
			 * next, see conc_task_set_outside(): back inside as a whole
			 * with the cpuset, as the presence moves.
			 */
			scoped_guard(raw_spinlock_irq, &t->pi_lock) {
				struct cpumask *applied = this_cpu_cpumask_var_ptr(conc_entitled_mask);

				__conc_task_class_mask(t, t->user_cpus_ptr, ent);
				__conc_task_entitled(t, t->user_cpus_ptr, applied);
				if (conc_task_registered_with(t, ent, applied))
					goto registered;
			}
			npre = conc_chain_depth(t);
			pre = conc_class_prealloc(npre, GFP_KERNEL);
			scoped_guard(raw_spinlock_irq, &t->pi_lock) {
				struct cpumask *applied = this_cpu_cpumask_var_ptr(conc_entitled_mask);

				__conc_task_entitled(t, t->user_cpus_ptr, applied);
				conc_task_class_del(t);
				if (conc_mask_counts(t, ent) && READ_ONCE(task_group(t)->conc.active))
					conc_task_class_add(t, ent, applied, pre, npre);
			}
			conc_class_prealloc_free(pre, npre);
		}
registered:
		top = tg;
		while (top->parent && READ_ONCE(top->parent->conc.active))
			top = top->parent;
		for (i = 0; i < n; i++) {
			if (tops[i] == top)
				break;
		}
		if (i == n && n == cap) {
			/* The iterator pins its position: sleeping is fine here. */
			struct task_group **bigger;

			cap = cap ? cap * 2 : 16;
			bigger = kvrealloc(tops, cap * sizeof(*tops), GFP_KERNEL);
			if (!bigger) {
				css_put(&tg->css);
				break;
			}
			tops = bigger;
		}
		if (i == n && css_tryget_online(&top->css))
			tops[n++] = top;
		css_put(&tg->css);
	}
	css_task_iter_end(&it);
	if (have_ent)
		free_cpumask_var(ent);
	if (!n) {
		mutex_unlock(&conc_mutex);
		kvfree(tops);
		return;
	}

	if (alloc_cpumask_var(&outside, GFP_KERNEL)) {
		for (i = 0; i < n; i++) {
			struct task_group *top = tops[i];

			cpumask_andnot(outside, top->conc.used, cpus);
			for_each_cpu(cpu, outside) {
				if (!READ_ONCE(top->conc.active))
					break;
				if (atomic_read(per_cpu_ptr(top->conc.present, cpu)) < 0)
					continue;
				/* Best effort: the tasks' own changes finish the job. */
				conc_evict_cpu(top, cpu, true, false, NULL, NULL, cpus);
			}
		}
		free_cpumask_var(outside);
	}
	mutex_unlock(&conc_mutex);
	for (i = 0; i < n; i++)
		css_put(&tops[i]->css);
	kvfree(tops);
}

/* Is a limit covering @tg stalled or relaxed, so that a retry can matter? */
static __maybe_unused bool conc_needs_retry(struct task_group *tg)
{
	struct task_group *lvl;

	for_each_conc_level_from(lvl, tg) {
		if (READ_ONCE(lvl->conc.shrink_pending) ||
		    (lvl->conc.request && READ_ONCE(lvl->conc.effective) > lvl->conc.request))
			return true;
	}
	return false;
}

#ifdef CONFIG_SCHED_MM_CID
/*
 * Glue between the cgroup concurrency limit and the cid domains, see
 * Documentation/scheduler/sched-max-concurrency.rst. The bound of a
 * domain is the effective limit of the innermost counting group at or
 * above the lowest common ancestor of its users' groups when there is
 * one (an mm's threads, usually), else the sum over the topmost counting
 * groups covering the users (a namespace's members, usually).
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
 * Is @t pinned to its cpu cgroup? Its mm is read under task_lock(), as
 * execve(2) replaces it. A kernel thread borrowing an mm is not.
 */
static __maybe_unused bool conc_task_pinned(struct task_struct *t)
{
	bool pinned;

	if (t->flags & PF_KTHREAD)
		return false;
	task_lock(t);
	pinned = t->mm && READ_ONCE(t->mm->conc_pin);
	task_unlock(t);
	return pinned;
}

/* The innermost counting group at or above the domain's common ancestor. */
static struct task_group *conc_dom_group(struct cid_domain *dom)
{
	struct task_group *tg;

	for (tg = dom->lca; tg; tg = tg->parent) {
		if (READ_ONCE(tg->conc.active))
			return tg;
	}
	return NULL;
}

/*
 * The bound of a domain: the effective limit of the innermost counting
 * group at or above the lowest common ancestor of its users' groups when
 * there is one. Otherwise the users span several counting subtrees, or
 * some are in none: the sum of the effective limits of the topmost
 * counting groups covering them, since each bounds the presence of its
 * own subtree, and no bound at all as soon as a user is uncovered.
 * Called with the domain's lock held when it has users.
 */
static unsigned int __conc_dom_bound(struct cid_domain *dom)
{
	struct task_group *tg = conc_dom_group(dom), *stack_tops[8], **tops = stack_tops;
	struct cid_user *u;
	unsigned int sum = 0, cap = ARRAY_SIZE(stack_tops);
	int n = 0, i;

	/*
	 * What is enforced so far, see conc_effective_report(): a decrease
	 * reaches the IDs once the group is within the new limit, not while
	 * its excess is still being evicted.
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

		for (; g && READ_ONCE(g->conc.active); g = g->parent)
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
			 * More topmost groups than the buffer holds, which
			 * cannot grow under this raw lock: keep the bound as
			 * it is and have cid_apply_bound() grow it and compute
			 * again.
			 */
			dom->tops_wanted = cap * 2;
			if (!dom->bound_update) {
				dom->bound_update = 1;
				irq_work_queue(&dom->irq_work);
			}
			return dom->bound;
		}
		tops[n++] = top;
		sum += conc_effective_report(top);
	}
	return sum;
}

unsigned int conc_dom_bound(struct cid_domain *dom)
{
	return __conc_dom_bound(dom);
}

/* Give the scan above the room it asked for. Process context, the domain's mutex held. */
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
 * A user is about to be added. The first user of an mm (fork(2) or
 * execve(2)) sets the ancestor exactly, since the task may have been
 * placed in a cgroup other than its parent's; a later user can only widen
 * it, and the bound can only widen with it, so no fixup is needed. The
 * bound is computed once the user is linked, see cid_add_user().
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

/* Recompute the ancestor of all users; task groups are read racily. */
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
 * A task of @prev's group left this CPU for good, or @prev blocks as the
 * last present task of its group here: a CPU-owned ID would stay behind
 * on a CPU the group may give up without any user of the domain left in
 * the group to visit at that point. Drop it; the next user here
 * allocates again. Under the rq lock.
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
	 * The ID this CPU owns: the exiting task cannot drop it at its last
	 * schedule-out, when its mm is gone, see conc_cid_left_cpu(). Drop it
	 * now when the domain is bounded; a user queued here allocates
	 * again.
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
 * A task changed group: recompute the ancestor of each of its domains
 * and defer the bound update, since sched_move_task() holds the runqueue
 * lock. The cgroup attach path applies it synchronously right after, see
 * conc_attach_finish(); the deferred work covers the other callers.
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
	if (p->mm)
		conc_dom_task_moved(&p->mm->mm_cid);
}

/*
 * Drop CPU-owned CIDs that are at or above max_cids, or cached on a CPU
 * outside the used set of the domain's group. Called with the domain's
 * mutex held and no transition in progress.
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

/* Diagnostics for an ID handed out above the bound: who holds the others? */
void conc_cid_violation(struct cid_domain *dom, unsigned int cid)
{
	struct task_group *tg = conc_dom_group(dom);
	const struct cpumask *used = tg ? tg->conc.used : cpu_none_mask;
	unsigned int live = tg ? conc_effective_report(tg) : dom->bound;
	unsigned long flags;
	static int reported;
	int cpu;

	/*
	 * A limit increase is visible to placement before the bounds of the
	 * subtree are refreshed; an ID below the live limit is fine. With no
	 * counting group over the users' common ancestor, the live bound is a
	 * sum over the groups covering them, or none once one is uncovered,
	 * as when a limit is removed: computed afresh, or no report when the
	 * domain's lock is taken (the caller may hold it), rather than one
	 * against a cached bound such a removal leaves stale.
	 */
	if (!tg) {
		if (!raw_spin_trylock_irqsave(&dom->lock, flags))
			return;
		live = __conc_dom_bound(dom);
		raw_spin_unlock_irqrestore(&dom->lock, flags);
	}
	if (!live || cid < live)
		return;
	/* Under the runqueue lock: the console output is deferred. */
	printk_deferred_enter();
	if (!IS_ENABLED(CONFIG_SCHED_MAX_CONCURRENCY_DEBUG)) {
		WARN_ONCE(1, "%s cid %u above bound %u\n",
			  dom->kind == CID_DOMAIN_MM ? "mm" : "ns", cid, live);
		printk_deferred_exit();
		return;
	}
	if (reported++ > 2) {
		printk_deferred_exit();
		return;
	}
	pr_warn("%s cid %u above bound %u (max_cids %u, users %u, mode %#x) on CPU %d, %s/%d\n",
		dom->kind == CID_DOMAIN_MM ? "mm" : "ns", cid, dom->bound, dom->max_cids,
		dom->users, dom->mode, raw_smp_processor_id(), current->comm, current->pid);
	pr_warn("  group used=%*pbl eff=%u nr_used=%u nr_evicting=%u unhit=%u\n",
		cpumask_pr_args(used),
		tg ? tg->conc.effective : 0, tg ? tg->conc.nr_used : 0,
		tg ? tg->conc.nr_evicting : 0, tg ? tg->conc.unhit : 0);
	for_each_possible_cpu(cpu) {
		unsigned int v = per_cpu_ptr(dom->pcpu, cpu)->cid;
		int slot = tg ? atomic_read(per_cpu_ptr(tg->conc.present, cpu)) : 0;

		if (cid_on_cpu(v) || cid_in_transit(v) || slot != CONC_SLOT_UNUSED)
			pr_warn("  cpu%d pcp=%#x slot=%d\n", cpu, v, slot);
	}
	pr_warn("  cidmask=%*pbl\n", (int)num_possible_cpus(), dom->cidmask);
	dump_stack();
	printk_deferred_exit();
}

/*
 * A limit write, a retry or a cgroup migration changed effective values
 * or the domain's group: refresh @dom's bound. Even with the bound
 * unchanged, a migration can leave IDs cached on CPUs the new group does
 * not use: the old group drains those slots later without finding the
 * domain.
 */
static void conc_dom_update_bound(struct cid_domain *dom)
{
	bool changed;

	guard(mutex)(&dom->mutex);
	if (!dom->users)
		return;
	scoped_guard(raw_spinlock_irq, &dom->lock) {
		/* The scan may ask for a deferred update itself: read the flag after it. */
		unsigned int bound = conc_dom_bound(dom);

		changed = dom->bound_update || dom->bound != bound;
	}
	if (changed)
		cid_apply_bound(dom);
	else
		conc_dom_drop_stale_cids(dom);
}

/*
 * A bound is applied between configuration walks. In the middle of one, a
 * group counts before its present tasks have a slot or a hold
 * (conc_activate_tasks()), and the ID of a CPU such a task runs on would be
 * kept past the bound applied, the task taken for its owner
 * (conc_dom_drop_stale_cids()). The walks hold conc_mutex and apply the bounds
 * themselves once over (conc_bounds_apply()); a deferred update and a
 * migration's refresh take it too.
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
