// SPDX-License-Identifier: GPL-2.0

/*
 * cpu.concurrency.max limits the number of CPUs a cgroup uses at once. See
 * Documentation/scheduler/sched-max-concurrency.rst. The hooks the
 * scheduler calls are inline in concurrency.h.
 */

#include <linux/delay.h>
#include <linux/sched/isolation.h>
#include <linux/sched/clock.h>
#include <linux/sched/mm.h>
#include <linux/cpuset.h>
#include <linux/stop_machine.h>
#include <linux/ipc_namespace.h>
#include <linux/nsproxy.h>
#include <linux/resume_user_mode.h>
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

#define CONC_EVICT_ROUNDS	10
/*
 * conc_hold_cpu() waits for the tasks of a slot to be held. It looks every
 * 50 to 100 us, this many times: 20 to 40 ms in all. A task is held at its
 * next return to user space, which a reschedule forces.
 */
#define CONC_HOLD_ROUNDS	400
#define conc_sm_ent_mask(arg, i)	\
	((struct cpumask *)((arg)->ent_masks + (i) * cpumask_size()))

/* One group of the moved subtree which uses the source CPU. */

struct conc_sm_group {
	struct task_group	*tg;
	int			n;		/* accounted candidates seen by the stopper */
	int			extra;		/* unaccounted tasks moved, now accounted */
	int			children;	/* child groups in the set using the CPU */
	int			merge;		/* children that used the destination already */
	bool			evicting;
	bool			reserved;
};

/* An ancestor of the top group, outside the moved set, that counts it. */
struct conc_sm_ancestor {
	struct task_group	*tg;
	bool			reserved;	/* its dst slot was created by us */
	bool			entity;		/* it counts the top group on dst */
	bool			drained;	/* its src slot emptied by the move */
};

/* The argument of the set-move stopper, see conc_set_move_stop(). */
struct conc_set_move_arg {
	int			src, dst;
	struct conc_sm_group	*groups;
	struct conc_sm_ancestor	*ancestors;
	int			nr_groups;
	int			nr_ancestors;
	/* Every domain of the groups' tasks, whether or not one is on src. */
	struct cid_domain	**doms;
	int			nr_doms;
	/* Tasks on src that may run on dst by their entitled mask only. */
	int			nr_ent;
	struct task_struct	**ent_tasks;
	char			*ent_masks;
	/* An affinity change in progress: @mask_task's mask is @mask. */
	struct task_struct	*mask_task;
	const struct cpumask	*mask;
};

/* A destination slot reserved for one task, with its ancestors' entities. */
struct conc_resv {
	struct task_group	*tg;
	struct conc_sm_ancestor	*anc;
	int			cpu;
	int			nr_anc;
};

/*
 * A source group of a cgroup migration. can_attach() records the source
 * groups while the tasks still belong to them. attach() retries the limits of
 * the source and destination groups. The list is serialized by cgroup_mutex.
 */
struct conc_attach_src {
	struct list_head	node;
	struct task_group	*tg;
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
/*
 * Serializes conc_key_put(). The put of the last reference waits until no
 * task is held, then turns the key off.
 */
static DEFINE_MUTEX(conc_key_mutex);
static DECLARE_SWAIT_QUEUE_HEAD(conc_held_wq);	/* woken from conc_held_wake_work */
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

/*
 * Serialize a set-move stopper against a late arrival. The stopper holds it
 * from its count check to its relabel. A late arrival holds it to claim an
 * evicting slot. If the claim comes before the check, the task is moved.
 * Otherwise the claim finds the slot gone and the task selects again. Take
 * it before tg->conc.lock.
 */
static DEFINE_RAW_SPINLOCK(conc_sm_lock);

/*
 * A cgroup migration claims slots thread by thread, on the current CPU of
 * each thread. A concurrent set-move would move these slots away. So a
 * pending migration is counted here, under conc_mutex, in can_attach(). A
 * set-move waits until none is pending before it starts. It drops
 * conc_mutex while it waits. A set-move which is nested in the headroom of
 * a reservation does not wait (conc_reserving).
 */
static atomic_t conc_attach_pending;
static DECLARE_WAIT_QUEUE_HEAD(conc_attach_wq);

/*
 * Non-zero while conc_reserve_ancestors() makes headroom with slots reserved.
 * Protected by conc_mutex.
 *
 * A set-move normally waits for the pending changes (conc_wait_attach()), and
 * that wait drops conc_mutex. With slots reserved it must not drop it. The
 * reserved slots have no entity above them yet. conc_detach_children() and
 * conc_update_tree() release or claim one entity per used slot, and could
 * then take these slots. So a set-move skips the wait while this is set.
 *
 * Skipping is safe. A wait found no pending change before the slots were
 * reserved. conc_mutex is held since then, and a change is checked under
 * conc_mutex. The count can only be non-zero for a migration which tests the
 * static key with no lock (conc_attach_check()). That migration is checked
 * under the locks afterwards.
 */
static unsigned int conc_reserving;

static void conc_wait_attach(void);
static int conc_subtree_snapshot(struct task_group *top, struct cgroup_subsys_state ***out,
				 int *nr);
static void conc_subtree_release(struct cgroup_subsys_state **arr, int n);

static void conc_irq_work(struct irq_work *work);
static void conc_cleaner_work(struct work_struct *work);
static int conc_resv_cpu(struct conc_resv *resv);

static inline bool conc_task_mask_fixed(struct task_struct *t);
static void conc_task_entitled(struct task_struct *t, struct cpumask *mask);

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

static int conc_evict_cpu(struct task_group *tg, int cpu, bool set_move, bool hold,
			struct task_struct *mask_task, const struct cpumask *mask,
			const struct cpumask *within);
static int conc_hold_cpu(struct task_group *tg, int cpu);
static bool conc_make_headroom(struct task_group *tg);
static bool conc_needs_retry(struct task_group *tg);

#ifdef CONFIG_SCHED_MM_CID
static void conc_sm_transfer_cids(struct conc_set_move_arg *arg);
#endif /* CONFIG_SCHED_MM_CID */

/*
 * Holding
 *
 * A task which finds no CPU within its group's bound does not run
 * user-space code beyond it.
 *
 * The task finds no slot during a wakeup, a migration or a group change. It
 * may hold locks there, conc_mutex among them, so it cannot stop. It runs
 * the kernel-mode part without a slot and takes no concurrency ID, see
 * conc_task_unplaced(). It is held at its next return to user space. The
 * per-task throttle of CFS bandwidth parks tasks in the same way.
 *
 * A held task is:
 *  - dequeued from its class, in TASK_RUNNING;
 *  - still queued for the core: p->on_rq, PSI, uclamp and the load it
 *    migrates with are unchanged;
 *  - on conc_held_list until a slot of its mask frees.
 *
 * The class sees a sleep at the hold and a wakeup at the placement. It sees
 * nothing else:
 *  - a held task which blocks is a plain sleeper, see conc_held_blocks();
 *  - the core returns the task to its class before it migrates it or
 *    changes its class, priority or group. A new hold is requested, see
 *    conc_unhold().
 *
 * Kernel threads never return to user space. They are never held and run
 * without a slot.
 *
 * CONC_HOLD_EVICT uses the same hold for the tasks of a slot which is being
 * emptied.
 *
 * SIGKILL ends a hold, or prevents one. The task goes to do_exit() with or
 * without a slot. Its hold stays pending, so that it takes no ID on the
 * way, see conc_fatal_signal().
 *
 * Placement
 *
 * conc_hold_work_fn() places the held tasks with a selection as for a
 * wakeup. conc_hold_kick() queues it after an event which can make room:
 *  - a slot empties or is removed;
 *  - a reservation is settled;
 *  - a set-move or a retry ran;
 *  - the mask or the group of a held task changed;
 *  - a CPU went online or offline.
 *
 * For a held task the selection never returns a CPU without a slot. The
 * task stays held when nothing within the bound fits.
 *
 * An rt or deadline task places itself instead, see conc_place_self().
 */
static LIST_HEAD(conc_held_list);
static DEFINE_RAW_SPINLOCK(conc_hold_lock);
atomic_t conc_nr_held;

/*
 * The rotation period, in milliseconds. A task which stays held for one
 * period is given a slot which runs tasks of its own scheduling class, see
 * conc_rotate_for(). The file sched/conc_rotation_ms of debugfs sets it.
 */
unsigned int conc_rotation_ms __read_mostly = 100;

static u64 conc_rotation_ns(void)
{
	return (u64)max(READ_ONCE(conc_rotation_ms), 1U) * NSEC_PER_MSEC;
}

/*
 * Take a reference on the static key. Each group with a limit or a ceiling
 * holds one.
 *
 * The hot paths test the key before the per-task state (conc_task_held(),
 * conc_task_release(), conc_select_done()). So the key is turned on with that
 * state clean, and turned off only when no task is held.
 *
 * The first reference resets the per-CPU selection pointers. This is done
 * outside conc_key_mutex. A get can find the key still on while the last put
 * turns it off, and then skips the reset. A stale pointer can then narrow one
 * CPU selection. select_task_rq() checks the selected CPU against the mask of
 * the task.
 */
static void conc_key_get(void)
{
	int cpu;

	if (!static_key_enabled(&sched_conc_enabled)) {
		for_each_possible_cpu(cpu)
			per_cpu(conc_select_cpus, cpu) = NULL;
	}
	static_branch_inc(&sched_conc_enabled);
	/* A put which waits for the held tasks must not turn the key off now. */
	swake_up_one(&conc_held_wq);
}

/*
 * True while a BPF scheduler is loaded. It places the tasks, so no limit can
 * start. Protected by conc_mutex.
 */
static bool conc_scx_active;

int conc_scx_enable(void)
{
	guard(mutex)(&conc_mutex);
	if (static_key_enabled(&sched_conc_enabled))
		return -EBUSY;
	conc_scx_active = true;
	return 0;
}

void conc_scx_disable(void)
{
	guard(mutex)(&conc_mutex);
	conc_scx_active = false;
}

static void conc_key_put(void)
{
	guard(mutex)(&conc_key_mutex);
	if (static_key_count(&sched_conc_enabled.key) == 1) {
		/*
		 * The last reference goes. No group counts any more, so every
		 * held task has a CPU to go to. Wait until no task is held.
		 *
		 * Stop waiting if conc_key_get() takes a reference meanwhile.
		 * A limit which starts can hold tasks for as long as it
		 * exists. The key then stays on, and this put drops one
		 * reference of several.
		 */
		conc_hold_kick();
		swait_event_exclusive(conc_held_wq, !atomic_read(&conc_nr_held) ||
				      static_key_count(&sched_conc_enabled.key) > 1);
	}
	static_branch_dec(&sched_conc_enabled);
}

static void conc_hold_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(conc_hold_work, conc_hold_work_fn);

static void conc_hold_irq_work_fn(struct irq_work *work);
static struct irq_work conc_hold_irq_work = IRQ_WORK_INIT_HARD(conc_hold_irq_work_fn);

/*
 * Wake the waiter on conc_held_wq when the last held task is let go. The
 * static key is turned off only once no task is held.
 *
 * An irq_work is used because the hold ends under the pi_lock and the
 * runqueue lock of the held task. The wakeup takes the same locks of the
 * waiter, which may be on the same runqueue.
 */
static void conc_held_wake_fn(struct irq_work *work)
{
	swake_up_one(&conc_held_wq);
}
static struct irq_work conc_held_wake_work = IRQ_WORK_INIT_HARD(conc_held_wake_fn);

static LIST_HEAD(conc_attach_srcs);

static inline bool conc_cpu_usable(int cpu)
{
	return cpu_active(cpu) && !cpumask_test_cpu(cpu, &conc_offlining_cpus);
}

/*
 * A set-move waits for the pending migrations and affinity changes to be
 * applied (conc_attach_begin(), conc_prepare_affinity()). A limit write
 * waits for them too, because it looks for the deadline tasks where they
 * are. It waits under cpuset_mutex. This is safe because it took the mutex
 * with cpuset_lock_no_attach(). No migration between cpusets is in progress
 * then, so no pending change needs cpuset_mutex to finish.
 */

/*
 * Count a migration which the cpu controller checked as pending, until
 * conc_attach_finish(). Call under conc_mutex.
 */
static void conc_attach_begin(void)
{
	lockdep_assert_held(&conc_mutex);
	atomic_inc(&conc_attach_pending);
	current->conc_attaching = 1;
}

/*
 * Wait until no cgroup migration and no affinity change is pending
 * (conc_attach_pending). The caller is about to look at where the tasks
 * are. It must find them where the migration or the affinity change puts
 * them. Drop conc_mutex while waiting.
 *
 * Why the wait cannot deadlock.
 *
 * The waiters
 *
 * A set-move and a reservation wait when they start, see conc_set_move() and
 * conc_resv_make(). A limit write waits before it looks for deadline tasks,
 * see conc_set_request(). A waiter holds conc_mutex and the locks of its
 * caller. A caller which holds cpuset_mutex is in one of these cases:
 *  - it took cpuset_mutex with cpuset_lock_no_attach();
 *  - it holds cgroup_mutex: cpuset_attach(), cpuset_fork() into another
 *    cpuset, cpuset_css_killed();
 *  - it holds the hotplug lock for writing.
 * No migration between cpusets is in flight in any of these cases.
 *
 * A kernel caller of set_cpus_allowed_ptr() may wait there too. It must
 * hold no lock which the callbacks of a migration take, such as the
 * mmap_lock which cpuset_attach_task() takes.
 *
 * The loop drops conc_mutex while it waits. No waiter holds a spinlock.
 *
 * What it waits for
 *
 * (A) A cgroup migration which the cpu controller checked in can_attach()
 * and which is not applied yet.
 *
 * From can_attach() to attach() it holds cgroup_mutex and the hotplug lock
 * for reading, see cgroup_attach_lock(). It takes conc_mutex, which the
 * loop drops.
 *
 * The attach() of the cpuset controller runs first. It needs cpuset_mutex
 * only for a migration between cpusets. Such a migration marks the attach
 * context in progress from cpuset_can_attach() on. A waiter took
 * cpuset_mutex with none in progress. None can start while it is held,
 * because cpuset_can_attach() needs cpuset_mutex.
 *
 * (B) An affinity change which was prepared and is not applied yet. It is
 * counted with the migrations.
 *
 * From the count to its end, a change takes only spinlocks. It waits only
 * for the stopper of its task's CPU, and for the task to leave a
 * migrate_disable() section, see affine_move_task(). It takes conc_mutex
 * only after the count dropped. So it needs nothing which a waiter holds.
 *
 * Other waits under conc_mutex
 *
 *  - A set-move waits for an expedited grace period and for the stoppers of
 *    its two CPUs, see conc_set_move().
 *  - An eviction waits for the stopper of one CPU, see conc_evict_cpu().
 *  - The repairs sleep between bounded retries.
 *
 * None of these needs a lock which a waiter may hold. The stoppers take
 * spinlocks only. The grace period takes none of the locks above. It does
 * not take the hotplug lock either, which CPU hotplug holds for writing
 * while it takes conc_mutex.
 *
 * A CPU going offline keeps its stopper until sched_cpu_wait_empty() has
 * run. A stopper which is gone is refused at once (-ENOENT). The set-move
 * stopper checks its destination again, see conc_cpu_usable().
 */
static void conc_wait_attach(void)
{
	lockdep_assert_held(&conc_mutex);
	/*
	 * current runs a cgroup migration and is between can_attach() and
	 * attach(). The attach of the cpuset controller changes the masks
	 * of the threads. That may need a set-move in their old groups,
	 * where they still are. No slot was claimed for the migration yet,
	 * so there is nothing to wait for.
	 */
	if (current->conc_attaching)
		return;
	while (atomic_read(&conc_attach_pending)) {
		lockdep_assert(!conc_reserving);
		mutex_unlock(&conc_mutex);
		wait_event(conc_attach_wq, !atomic_read(&conc_attach_pending));
		mutex_lock(&conc_mutex);
	}
}

#ifndef CONFIG_SCHED_MM_CID
void sched_cid_resume(void) { }
#endif /* !CONFIG_SCHED_MM_CID */

/*
 * Allocate the per-CPU counters and the masks of group @tg. Return 0 or
 * -ENOMEM. Call under conc_mutex.
 *
 * The state is allocated when the group first counts (conc_tg_take_ceiling(),
 * conc_tg_online(), conc_set_request()). Most groups never count, and the
 * per-CPU allocation is costly.
 *
 * A group has its state from its first count on. Every reader of @present and
 * @used relies on this. The per-task paths run for counting groups only. The
 * walks over the groups below a counting group test for the state, see
 * conc_tg_uses().
 */
static int conc_tg_state_alloc(struct task_group *tg)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t __percpu *present;
	int cpu;

	lockdep_assert_held(&conc_mutex);
	if (conc->present)
		return 0;
	if (!zalloc_cpumask_var(&conc->used, GFP_KERNEL))
		return -ENOMEM;
	if (!alloc_cpumask_var(&conc->unused_cores, GFP_KERNEL)) {
		free_cpumask_var(conc->used);
		return -ENOMEM;
	}
	cpumask_setall(conc->unused_cores);
	present = alloc_percpu(atomic_t);
	if (!present) {
		free_cpumask_var(conc->unused_cores);
		free_cpumask_var(conc->used);
		return -ENOMEM;
	}
	conc->since = alloc_percpu(u64);
	if (!conc->since) {
		free_percpu(present);
		free_cpumask_var(conc->unused_cores);
		free_cpumask_var(conc->used);
		return -ENOMEM;
	}
	for_each_possible_cpu(cpu)
		atomic_set(per_cpu_ptr(present, cpu), CONC_SLOT_UNUSED);
	/*
	 * Publish @present last. conc_core_online() tests it without
	 * conc_mutex, and then uses the masks.
	 */
	smp_store_release(&conc->present, present);
	return 0;
}

/*
 * Return true if @tg uses @cpu. This serves the walks over the groups below a
 * counting group. Such a walk can meet a group without state. css_create()
 * links a group before cpu_cgroup_css_online() gives it any.
 * conc_subtree_state_alloc() skips a dying group which never counted. The
 * masks of such a group are NULL under CONFIG_CPUMASK_OFFSTACK. conc_mutex
 * orders the test with conc_tg_state_alloc().
 */
static inline bool conc_tg_uses(struct task_group *tg, int cpu)
{
	lockdep_assert_held(&conc_mutex);
	return tg->conc.present && cpumask_test_cpu(cpu, tg->conc.used);
}

/*
 * Allocate the state of every group of the subtree of @top, for a limit which
 * starts over it.
 */
static int conc_subtree_state_alloc(struct task_group *top)
{
	struct cgroup_subsys_state **csses;
	int nr_css, k, ret = 0;

	ret = conc_subtree_snapshot(top, &csses, &nr_css);
	if (ret)
		return ret;
	for (k = 0; k < nr_css && !ret; k++) {
		if (csses[k] && !css_is_dying(csses[k]))
			ret = conc_tg_state_alloc(css_tg(csses[k]));
	}
	conc_subtree_release(csses, nr_css);
	return ret;
}

int alloc_conc_sched_group(struct task_group *tg)
{
	struct tg_concurrency *conc = &tg->conc;

	raw_spin_lock_init(&conc->lock);
	INIT_LIST_HEAD(&conc->pass_node);
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
	free_percpu(tg->conc.since);
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
 * Add @cpu to the used set. Record the time: the rotation takes the oldest
 * slot first. The SMT core of @cpu is no longer unused.
 */
static void conc_used_add(struct tg_concurrency *conc, int cpu)
{
	WRITE_ONCE(*per_cpu_ptr(conc->since, cpu), ktime_get_ns());
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

static bool conc_has_room(struct tg_concurrency *conc)
{
	guard(raw_spinlock_irqsave)(&conc->lock);
	return __conc_has_room(conc);
}

static struct cpumask *conc_alloc_scratch(void)
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
 * Claim a slot of @tg on @cpu which is being evicted or drained. Used for a
 * task which found no other CPU within the limit (a late arrival). The
 * group's concurrency IDs are still on that CPU. conc_sm_lock orders this
 * against the set-move stopper.
 *
 * Return false if the slot is not evicting or draining any more. The caller
 * then selects again.
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
	if (conc_slot_evicting(v))
		atomic_inc(slot);
	else if (v == CONC_SLOT_DRAINING)
		atomic_set(slot, 1);	/* the cleaner notices and keeps the slot */
	else
		return false;
	return true;
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
 * Return true if every active CPU of @p's mask is going offline. The unplug
 * will widen the mask of such a task. Kernel threads and tasks with
 * migration disabled are excluded.
 */
static bool conc_task_pinned_offlining(struct task_struct *p)
{
	int c;

	if (p->flags & PF_KTHREAD || is_migration_disabled(p))
		return false;
	for_each_cpu_and(c, p->cpus_ptr, cpu_active_mask) {
		if (!cpumask_test_cpu(c, &conc_offlining_cpus))
			return false;
	}
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
		 *
		 * A held task takes no such slot. It needs no CPU now, and
		 * the eviction may have just held it to empty that slot,
		 * see conc_hold_cpu().
		 */
		for_each_cpu_and(c, tg->conc.used, p->cpus_ptr) {
			if (held)
				break;
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
static void conc_update_tset_mm_bounds(struct cgroup_taskset *tset)
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
static void conc_attach_drop_idle_cids(struct cgroup_taskset *tset)
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

static void conc_attach_forget_mms(void)
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

static void conc_update_tset_mm_bounds(struct cgroup_taskset *tset) { }

static void conc_attach_drop_idle_cids(struct cgroup_taskset *tset) { }

static void conc_attach_forget_mms(void) { }

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
		if (!conc_tg_uses(child, cpu))
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
 * Drain the idle slots which the child groups of @tg keep on @cpu. A slot
 * is sticky. It stays after the tasks of the child left the CPU or the
 * group, and still counts as an entity of @tg. Leave the busy children
 * alone.
 */
static void conc_drain_idle_children(struct task_group *tg, int cpu)
{
	struct task_group *child;

	lockdep_assert_held(&conc_mutex);

	rcu_read_lock();
	list_for_each_entry_rcu(child, &tg->children, siblings) {
		if (!conc_tg_uses(child, cpu))
			continue;
		/* A dying child is drained too, see __conc_drain_cpu(). */
		if (!css_tryget(&child->css))
			continue;
		rcu_read_unlock();
		__conc_drain_cpu(child, cpu, false);
		rcu_read_lock();
		css_put(&child->css);
	}
	rcu_read_unlock();
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
 * Return true if @p is a deadline task which the deadline admission accounts
 * for. It has the SCHED_DEADLINE policy, admission is on, and it is not a
 * special task.
 *
 * Such a task is admitted against every CPU of its root domain
 * (sched_dl_overflow()). This is why its affinity may not be narrower than
 * that domain (dl_task_check_affinity()). A limit gives the task fewer CPUs
 * in the same way, and that admission does not see it. So the callers refuse
 * such a task in a subtree which counts.
 *
 * Priority inheritance into the deadline class is not refused. The kernel
 * does it, it is transient, and it does not change the policy.
 *
 * Call under p->pi_lock or task_rq(p)->lock.
 */
static inline bool conc_dl_task(struct task_struct *p)
{
	return task_has_dl_policy(p) && dl_bandwidth_enabled() && !dl_entity_is_special(&p->dl);
}

/*
 * Check the deadline tasks of the subtree of @tg, on which a limit starts.
 * Return 0 if the limit may start, or a negative error code. The caller
 * holds cpuset_mutex and conc_mutex.
 *
 * A deadline task (conc_dl_task()) is refused.
 */
static int conc_subtree_dl_start(struct task_group *tg)
{
	struct cgroup_subsys_state **csses;
	struct css_task_iter it;
	struct task_struct *t;
	int ret, nr_css, k;

	ret = conc_subtree_snapshot(tg, &csses, &nr_css);
	if (ret)
		return ret;
	for (k = 0; k < nr_css && !ret; k++) {
		if (!csses[k])
			continue;
		css_task_iter_start(csses[k], 0, &it);
		while (!ret && (t = css_task_iter_next(&it))) {
			guard(raw_spinlock_irq)(&t->pi_lock);
			if (!conc_dl_task(t))
				continue;
			trace_sched_conc_refuse(t, "cpu.concurrency.max",
					      "a SCHED_DEADLINE task", -EBUSY);
			ret = -EBUSY;
		}
		css_task_iter_end(&it);
	}
	conc_subtree_release(csses, nr_css);
	return ret;
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
 *
 * Return 1 if a value changed which the concurrency ID bounds or the
 * effective files depend on, see __conc_retry(), and 0 if none did. Return
 * -ENOMEM if the subtree could not be walked. Nothing was changed then.
 */
static int conc_update_tree(struct task_group *top, struct cpumask *scratch)
{
	struct cgroup_subsys_state **csses, *css;
	bool changed = false;
	int nr_css, k;

	lockdep_assert_held(&conc_mutex);

	/*
	 * This walk visits every task and may sleep. So it runs on a
	 * referenced snapshot of the subtree, not under RCU. A group created
	 * meanwhile is walked once it is linked in.
	 */
	if (conc_subtree_snapshot(top, &csses, &nr_css)) {
		pr_warn_once("conc: no memory to update the limits of css %d\n", top->css.id);
		return -ENOMEM;
	}
	for (k = 0; k < nr_css; k++) {
		bool should_active, new_prop, walk = false, teardown = false;
		struct task_group *tg, *parent;
		unsigned int eff, ceiling, was;
		struct tg_concurrency *conc;
		unsigned long flags;
		int cpu, attach = 0;
		bool stalled;

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
		/*
		 * A group which was linked in after the caller allocated the
		 * state of the subtree has none yet, see conc_tg_prepare().
		 * Allocate it before the group counts.
		 */
		if (should_active && !conc->present && conc_tg_state_alloc(tg)) {
			pr_warn_once("conc: no memory for css %d, which does not count\n", css->id);
			continue;
		}
		if (!should_active && conc->active)
			conc_detach_children(tg, scratch);

		raw_spin_lock_irqsave(&conc->lock, flags);
		was = conc_report_ceiling(conc);
		/*
		 * A drop of the reported value which is not applied to the
		 * bounds yet counts as a change too, see
		 * conc_report_drop().
		 */
		changed |= eff != conc->effective || ceiling != conc->ceiling_effective ||
			   should_active != conc->active || new_prop != conc->propagate ||
			   conc->bounds_pending;
		stalled = conc->shrink_pending;
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
		changed |= conc->shrink_pending != stalled;
		/* The caller applies the bounds, see conc_bounds_apply(). */
		if (conc_report_ceiling(conc) < was)
			conc_report_drop(conc, was);
		raw_spin_unlock_irqrestore(&conc->lock, flags);
		/*
		 * The shown value may also differ from the one last
		 * notified. A stalled group reports the number of CPUs it
		 * uses, see conc_report_ceiling().
		 */
		changed |= conc_effective_shown(tg) != conc->reported ||
			   ceiling != conc->reported_ceiling;

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
	return changed;
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
 * Eviction
 *
 * Eviction empties a CPU of the tasks of a group. The tasks of that group
 * never hold slots on more CPUs than before. This keeps the concurrency IDs
 * of its processes below their bound.
 *
 * There are two mechanisms:
 *
 *  - join: a task moves to a CPU which its own group already uses. No slot
 *    is added, so the CPU count of the group does not grow. Used for
 *    shrinking.
 *
 *  - set-move: all the tasks of a subtree on a CPU move to one new CPU
 *    under stop_two_cpus(). The tasks are moved, the slots are relabelled
 *    and the CPU-owned concurrency IDs are transferred in one step, while
 *    neither CPU runs anything else. Used when a CPU must be vacated
 *    (hot-unplug, an affinity change) and its tasks cannot join.
 *
 * The used set during a set-move
 *
 * The destination slot is reserved before the stopper runs. A reserved slot
 * is in the used set, but it refuses the claims of tasks. Take a moved
 * group which did not use the destination. Until the stopper runs, its used
 * set holds one CPU more than its tasks hold slots on. This can be one CPU
 * above the limit.
 *
 * The same holds for an ancestor whose only entity on the source CPU is the
 * moved subtree. Any other ancestor without a slot on the destination gets
 * one only if its limit has room for it.
 *
 * Stopping the arrivals before a set-move
 *
 * Arrivals on the source CPU are stopped in three steps, so that the
 * stopper sees every task which holds a slot there:
 *  - the slot values are made negative, so new claims are refused;
 *  - synchronize_rcu() waits for the claims already made, which run with
 *    interrupts disabled from the claim to the enqueue;
 *  - the stopper flushes the wakelist.
 */

/*
 * Return a used CPU of @p's group which @p can move to from @cpu, or -1.
 * Prefer an idle CPU. Skip a slot in a transient state. Use @mask in place
 * of @p's mask when given (an affinity change is in progress). Keep to
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

/*
 * Mark the slot of @tg on @cpu as evicting. Return false if there is no
 * slot, or if the slot is in another transient state.
 */
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
 * Reserve the slot of @tg on @cpu for a set-move, or for the arrival of one
 * task, see conc_resv_make(). Return 1 if the slot was reserved, 0 if the
 * slot existed already, -EBUSY if the limit or a transient state refuses
 * it.
 *
 * A reserved slot is in the used set and counts against the limit. It
 * refuses the claims of other tasks until the stopper, or that task, fills
 * it.
 *
 * With @swap, the slot takes the place of a slot which the move empties. It
 * needs no headroom. The used set can then hold one CPU more than the limit
 * until the move.
 *
 * The caller claims the entities of the parent, see
 * conc_reserve_ancestors().
 */
static int conc_reserve_slot(struct task_group *tg, int cpu, bool swap)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, cpu);
	unsigned long flags;
	int v, ret;

	lockdep_assert_held(&conc_mutex);

	raw_spin_lock_irqsave(&conc->lock, flags);
	v = atomic_read(slot);
	if (v >= 0) {
		ret = 0;
	} else if (v != CONC_SLOT_UNUSED) {
		ret = -EBUSY;
	} else if (!swap && cpumask_test_cpu(cpu, &conc_offlining_cpus)) {
		ret = -EBUSY;
	} else if (!swap && !__conc_has_room(conc)) {
		ret = -EBUSY;
	} else {
		atomic_set(slot, -CONC_SLOT_RESERVE_BIAS);
		conc_used_add(conc, cpu);
		ret = 1;
	}
	raw_spin_unlock_irqrestore(&conc->lock, flags);
	return ret;
}

/*
 * Remove the reserved, empty slot of @tg on @cpu. The caller holds
 * conc_mutex and found the slot in that state under the group lock.
 *
 * Nothing can fill the slot meanwhile:
 *  - the task it was reserved for had its conc_resv_cpu cleared before;
 *  - a forced claim needs conc_mutex;
 *  - the lockless claim refuses a negative value.
 *
 * A task which arrived and left by migration may have left a concurrency ID
 * cached on the CPU. Drop the IDs before the slot is removed, so that no ID
 * outlives the slot. Nothing can arrive in between, so no ID is cached
 * after the drop.
 */
static void conc_remove_reserved(struct task_group *tg, int cpu)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, cpu);
	unsigned long flags;

	lockdep_assert_held(&conc_mutex);

	conc_slot_removed(tg, cpu);
	raw_spin_lock_irqsave(&conc->lock, flags);
	if (WARN_ON_ONCE(atomic_read(slot) != -CONC_SLOT_RESERVE_BIAS)) {
		/* Something landed after all: it stays a normal slot. */
		if (conc_slot_reserved(atomic_read(slot)))
			atomic_add(CONC_SLOT_RESERVE_BIAS, slot);
	} else {
		/*
		 * Go straight from the reserved state to unused. Through
		 * zero, the lockless claim of conc_claim() could take the
		 * slot between the unreserve and the removal. The task
		 * would hold a slot which is gone. The group would run on
		 * more CPUs than its limit, and its IDs would pass the
		 * bound.
		 */
		atomic_set(slot, CONC_SLOT_UNUSED);
		conc_used_del(conc, cpu);
		conc_slot_count_dropped(conc);
	}
	raw_spin_unlock_irqrestore(&conc->lock, flags);
}

/*
 * Undo conc_reserve_slot() when the set-move did not happen. Remove the
 * slot if it is empty, see conc_remove_reserved(). Otherwise an entity
 * landed, and the slot stays as a normal slot.
 */
static void conc_unreserve_slot(struct task_group *tg, int cpu)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, cpu);
	unsigned long flags;
	bool empty;

	lockdep_assert_held(&conc_mutex);

	raw_spin_lock_irqsave(&conc->lock, flags);
	empty = atomic_read(slot) == -CONC_SLOT_RESERVE_BIAS;
	if (!empty && conc_slot_reserved(atomic_read(slot)))
		atomic_add(CONC_SLOT_RESERVE_BIAS, slot);
	raw_spin_unlock_irqrestore(&conc->lock, flags);
	if (empty)
		conc_remove_reserved(tg, cpu);
}

/* Turn a reservation into a normal slot once it was filled. */
static void conc_unreserve_filled(struct task_group *tg, int cpu)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, cpu);
	unsigned long flags;

	raw_spin_lock_irqsave(&conc->lock, flags);
	if (conc_slot_reserved(atomic_read(slot)))
		atomic_add(CONC_SLOT_RESERVE_BIAS, slot);
	raw_spin_unlock_irqrestore(&conc->lock, flags);
}

/*
 * Prepare the ancestors of @top for its move from @src to @dst. @src is -1
 * for a reservation without a source. Return 0 or a negative error. On
 * return *@ancp holds *@nrp ancestors: index 0 is the topmost, the last one
 * is the parent of @top. The caller frees it, also on error.
 *
 * Each ancestor without a slot on @dst gets a reserved one, see
 * conc_reserve_slot(). None of its own tasks can then claim a slot there
 * before the move. If the limit refuses the slot, headroom is made first,
 * see conc_make_headroom().
 *
 * If @top is the only entity of an ancestor on @src, the ancestor loses
 * @src when it gains @dst. Its slot is then reserved without a headroom
 * check (a swap).
 *
 * With @entity, the slot of @top on @dst is new. The parent then counts one
 * more entity on @dst. So does each level above, as long as the slot below
 * it is new too.
 */
static int conc_reserve_ancestors(struct task_group *top, int src, int dst,
				struct conc_sm_ancestor **ancp, int *nrp, bool entity)
{
	struct conc_sm_ancestor *anc;
	struct task_group *tg;
	int i, n = 0;

	*ancp = NULL;
	*nrp = 0;
	for (tg = top; tg->conc.propagate; tg = tg->parent)
		n++;
	if (!n)
		return 0;
	anc = kvcalloc(n, sizeof(*anc), GFP_KERNEL);
	if (!anc)
		return -ENOMEM;
	for (tg = top, i = n - 1; tg->conc.propagate; tg = tg->parent, i--)
		anc[i].tg = tg->parent;
	*ancp = anc;
	*nrp = n;

	for (i = 0; i < n; i++) {
		struct task_group *tg = anc[i].tg;
		bool swap;
		int r;

		/* Idle slots of other children on src only inflate the count. */
		if (src >= 0 && atomic_read(per_cpu_ptr(tg->conc.present, src)) > 1)
			conc_drain_idle_children(tg, src);
		swap = src >= 0 && atomic_read(per_cpu_ptr(tg->conc.present, src)) == 1;
		r = conc_reserve_slot(tg, dst, swap);
		if (r == -EBUSY && !cpumask_test_cpu(dst, tg->conc.used)) {
			bool room;

			/*
			 * Slots are reserved. Do not wait meanwhile, see
			 * conc_reserving.
			 */
			conc_reserving++;
			room = conc_make_headroom(tg);
			conc_reserving--;
			if (room)
				r = conc_reserve_slot(tg, dst, false);
		}
		if (r < 0)
			return r;
		anc[i].reserved = r == 1;
	}
	/*
	 * The top group gets a new slot on dst: its parent counts one entity
	 * more there, and so does each level above while the slot below it is
	 * new as well. A slot that was there already is counted above it.
	 */
	for (i = n - 1; i >= 0 && entity; i--) {
		conc_claim(anc[i].tg, dst, true, NULL);
		anc[i].entity = true;
		if (!anc[i].reserved)
			break;
	}
	return 0;
}

/* Nothing moved: give the ancestors back what the reservation took. */
static void conc_unreserve_ancestors(struct conc_sm_ancestor *anc, int nr, int dst)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (anc[i].entity)
			conc_release(anc[i].tg, dst);
	}
	for (i = nr - 1; i >= 0; i--) {
		if (anc[i].reserved)
			conc_unreserve_slot(anc[i].tg, dst);
	}
}

/*
 * Update the ancestors after the move. Called from the set-move stopper,
 * and from conc_resv_finish() with @src == -1.
 *
 * Turn their reserved slots on @dst into normal slots, so that they can
 * place their own tasks there.
 *
 * Then walk up from the parent and remove one entity from the slot on @src.
 * Stop at the first slot which is not empty afterwards. Set an empty slot
 * to draining. conc_finish_ancestors() removes it, unless a late arrival
 * claimed it meanwhile (conc_claim_late()). Set shrink_pending on an
 * ancestor which uses more CPUs than its limit.
 */
static void conc_ancestors_moved(struct conc_sm_ancestor *anc, int nr, int src, int dst)
{
	int i;

	for (i = 0; i < nr; i++)
		conc_unreserve_filled(anc[i].tg, dst);

	for (i = nr - 1; i >= 0 && src >= 0; i--) {
		struct tg_concurrency *conc = &anc[i].tg->conc;
		atomic_t *slot = per_cpu_ptr(conc->present, src);
		unsigned long flags;
		int v;

		raw_spin_lock_irqsave(&conc->lock, flags);
		/*
		 * The entity of the level below leaves src. An empty slot is
		 * set draining from zero in one step, as conc_drain_cpu() does:
		 * the lockless claim of conc_claim() may take it in between.
		 */
		v = atomic_dec_return(slot);
		if (v == 0) {
			if (atomic_cmpxchg(slot, 0, CONC_SLOT_DRAINING) == 0)
				anc[i].drained = true;
			else
				v = 1;
		}
		if (conc->effective && conc->nr_used > conc->effective)
			conc->shrink_pending = true;
		raw_spin_unlock_irqrestore(&conc->lock, flags);
		if (v != 0)
			break;
	}
}

/*
 * Remove the src slots which conc_ancestors_moved() set to draining. Free
 * @anc. Called in process context.
 */
static void conc_finish_ancestors(struct conc_sm_ancestor *anc, int nr, int src)
{
	int i;

	for (i = nr - 1; i >= 0; i--) {
		struct tg_concurrency *conc = &anc[i].tg->conc;
		atomic_t *slot = per_cpu_ptr(conc->present, src);
		unsigned long flags;

		if (!anc[i].drained)
			break;
		conc_slot_removed(anc[i].tg, src);
		raw_spin_lock_irqsave(&conc->lock, flags);
		/* conc_ancestors_moved() released the entity above already. */
		if (atomic_read(slot) == CONC_SLOT_DRAINING) {
			atomic_set(slot, CONC_SLOT_UNUSED);
			conc_used_del(conc, src);
			conc_slot_count_dropped(conc);
		}
		raw_spin_unlock_irqrestore(&conc->lock, flags);
	}
	kvfree(anc);
}

static struct conc_sm_group *conc_sm_group_of(struct conc_set_move_arg *arg, struct task_group *tg)
{
	int i;

	for (i = 0; i < arg->nr_groups; i++) {
		if (arg->groups[i].tg == tg)
			return &arg->groups[i];
	}
	return NULL;
}

#ifdef CONFIG_SCHED_MM_CID
/*
 * Hand the CPU-owned ID of @dom on @src over to @dst. The caller holds both
 * runqueue locks, and neither CPU runs a task.
 *
 * The tasks which move to @dst then find their ID there. Otherwise they
 * would allocate one while @src still holds theirs. If @dst owns an ID of
 * @dom already, drop the one on @src.
 */
static void conc_transfer_cid(struct cid_domain *dom, int src, int dst)
{
	struct cid_pcpu *spcp = per_cpu_ptr(dom->pcpu, src);
	struct cid_pcpu *dpcp = per_cpu_ptr(dom->pcpu, dst);
	unsigned int cid = spcp->cid;

	/* Nothing owned, or owned by a mode transition in progress. */
	if (!cid_on_cpu(cid) || cid_in_transit(cid))
		return;
	if (cid_on_cpu(dpcp->cid)) {
		cid_drop_on_cpu(dom, spcp);
		return;
	}
	cid_pcpu_write(dpcp, cid);
	/* No longer owned by @src, see cid_drop_on_cpu(). */
	cid_pcpu_write(spcp, cpu_cid_to_cid(cid));
}

/*
 * Collect the distinct domains of all the tasks of the groups. Take a
 * reference on each. Return 0 or -ENOMEM.
 */
static int conc_sm_collect_mms(struct conc_set_move_arg *arg)
{
	int i, cap = 0, n = 0;
	bool full = false;

	for (i = 0; i < arg->nr_groups; i++) {
		struct css_task_iter it;

		css_task_iter_start(&arg->groups[i].tg->css, 0, &it);
		while (css_task_iter_next(&it))
			cap += 2;
		css_task_iter_end(&it);
	}
	cap += 16;
	arg->doms = kvmalloc_array(cap, sizeof(*arg->doms), GFP_KERNEL);
	if (!arg->doms)
		return -ENOMEM;

	for (i = 0; i < arg->nr_groups; i++) {
		struct css_task_iter it;
		struct task_struct *t;

		css_task_iter_start(&arg->groups[i].tg->css, 0, &it);
		while ((t = css_task_iter_next(&it))) {
			struct cid_domain *doms[2];
			int k, nt = conc_task_domains(t, doms);

			for (k = 0; k < nt; k++) {
				int j;

				for (j = 0; j < n; j++) {
					if (arg->doms[j] == doms[k])
						break;
				}
				if (j < n || n == cap) {
					/* Tasks arrived since the array was sized. */
					if (j == n)
						full = true;
					cid_domain_put(doms[k]);
					continue;
				}
				arg->doms[n++] = doms[k];
			}
		}
		css_task_iter_end(&it);
	}
	arg->nr_doms = n;
	/*
	 * A domain which is not collected would keep its ID on the source
	 * CPU. Let the caller collect again.
	 */
	return full ? -EAGAIN : 0;
}

static void conc_sm_free_mms(struct conc_set_move_arg *arg)
{
	int i;

	for (i = 0; i < arg->nr_doms; i++)
		cid_domain_put(arg->doms[i]);
	kvfree(arg->doms);
	arg->doms = NULL;
	arg->nr_doms = 0;
}
#else /* !CONFIG_SCHED_MM_CID */
static inline int conc_sm_collect_mms(struct conc_set_move_arg *arg) { return 0; }

static inline void conc_sm_free_mms(struct conc_set_move_arg *arg) { }

static inline void conc_sm_transfer_cids(struct conc_set_move_arg *arg) { }
#endif /* !CONFIG_SCHED_MM_CID */

/*
 * Compute into @mask the mask @t is entitled to. Start from the CPUs of
 * its cpuset. If they intersect @user, the mask @t requested, restrict
 * them to @user. __set_cpus_allowed_ptr() restricts a change from the
 * kernel side the same way.
 *
 * A cpuset change reaches the tasks one at a time. This mask is ahead of
 * the tasks which the change has not reached yet. It lets the presence of
 * a group move with the cpuset as a whole.
 */
static void __conc_task_entitled(struct task_struct *t, const struct cpumask *user,
			       struct cpumask *mask)
{
	scoped_guard(rcu)
		cpuset_task_cpus_rcu(t, mask);
	if (user && cpumask_intersects(mask, user))
		cpumask_and(mask, mask, user);
}

static void conc_task_entitled(struct task_struct *t, struct cpumask *mask)
{
	guard(raw_spinlock_irq)(&t->pi_lock);
	__conc_task_entitled(t, t->user_cpus_ptr, mask);
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

static int conc_sm_entitled_idx(struct conc_set_move_arg *arg, struct task_struct *p)
{
	int i;

	for (i = 0; i < arg->nr_ent; i++) {
		if (arg->ent_tasks[i] == p)
			return i;
	}
	return -1;
}

static const struct cpumask *conc_sm_entitled(struct conc_set_move_arg *arg, struct task_struct *p)
{
	int i = conc_sm_entitled_idx(arg, p);

	return i >= 0 ? conc_sm_ent_mask(arg, i) : NULL;
}

static void conc_sm_free_entitled(struct conc_set_move_arg *arg)
{
	int i;

	for (i = 0; i < arg->nr_ent; i++)
		put_task_struct(arg->ent_tasks[i]);
	kvfree(arg->ent_tasks);
	kvfree(arg->ent_masks);
	arg->ent_tasks = NULL;
	arg->ent_masks = NULL;
	arg->nr_ent = 0;
}

/*
 * Record the tasks on src whose mask does not allow dst, but whose entitled
 * mask does. Record that mask too. The stopper applies it when it moves
 * them. Called in process context before each attempt. Return 0 or -ENOMEM.
 */
static int conc_sm_collect_entitled(struct conc_set_move_arg *arg)
{
	cpumask_var_t ent;
	int i, cap = 0, n = 0;

	conc_sm_free_entitled(arg);
	if (!alloc_cpumask_var(&ent, GFP_KERNEL))
		return -ENOMEM;
	for (i = 0; i < arg->nr_groups; i++) {
		struct css_task_iter it;
		struct task_struct *t;

		css_task_iter_start(&arg->groups[i].tg->css, 0, &it);
		while ((t = css_task_iter_next(&it))) {
			if (task_cpu(t) == arg->src)
				cap++;
		}
		css_task_iter_end(&it);
	}
	cap += 8;
	arg->ent_tasks = kvcalloc(cap, sizeof(*arg->ent_tasks), GFP_KERNEL);
	arg->ent_masks = kvmalloc_array(cap, cpumask_size(), GFP_KERNEL);
	if (!arg->ent_tasks || !arg->ent_masks) {
		conc_sm_free_entitled(arg);
		free_cpumask_var(ent);
		return -ENOMEM;
	}
	for (i = 0; i < arg->nr_groups && n < cap; i++) {
		struct css_task_iter it;
		struct task_struct *t;

		css_task_iter_start(&arg->groups[i].tg->css, 0, &it);
		while ((t = css_task_iter_next(&it)) && n < cap) {
			if (task_cpu(t) != arg->src || t == arg->mask_task ||
			    conc_task_mask_fixed(t) || cpumask_test_cpu(arg->dst, t->cpus_ptr))
				continue;
			conc_task_entitled(t, ent);
			if (!cpumask_test_cpu(arg->dst, ent))
				continue;
			get_task_struct(t);
			arg->ent_tasks[n] = t;
			cpumask_copy(conc_sm_ent_mask(arg, n), ent);
			n++;
		}
		css_task_iter_end(&it);
	}
	arg->nr_ent = n;
	free_cpumask_var(ent);
	return 0;
}

/*
 * Walk every task queued on @rq that can hold a slot, class by class, and
 * call @fn on those belonging to the groups being moved. Safe against @fn
 * dequeuing the task. Returns the first non-zero result of @fn.
 *
 * Tasks of the fair class are on rq->cfs_tasks, except those throttled by
 * a bandwidth limit, which sit on the limbo list of their group's cfs_rq.
 * RT and deadline tasks that may move at all are on the pushable lists.
 */
static int conc_sm_walk(struct conc_set_move_arg *arg, struct rq *rq,
		      int (*fn)(struct conc_set_move_arg *, struct task_struct *,
				struct conc_sm_group *))
{
	struct task_struct *p, *n;
	struct conc_sm_group *g;
	struct rb_node *node;
	int ret;

#define conc_sm_visit(p)							\
	({								\
		struct task_struct *__p = (p);				\
		int __ret = 0;						\
		if (!task_is_blocked(__p)) {				\
			g = conc_sm_group_of(arg, task_group(__p));	\
			if (g)						\
				__ret = fn(arg, __p, g);		\
		}							\
		__ret;							\
	})

	list_for_each_entry_safe(p, n, &rq->cfs_tasks, se.group_node) {
		ret = conc_sm_visit(p);
		if (ret)
			return ret;
	}
#ifdef CONFIG_CFS_BANDWIDTH
	for (int i = 0; i < arg->nr_groups; i++) {
		struct cfs_rq *cfs_rq = per_cpu_ptr(arg->groups[i].tg->cfs_rq, cpu_of(rq));

		list_for_each_entry_safe(p, n, &cfs_rq->throttled_limbo_list, throttle_node) {
			ret = conc_sm_visit(p);
			if (ret)
				return ret;
		}
	}
#endif
	plist_for_each_entry_safe(p, n, &rq->rt.pushable_tasks, pushable_tasks) {
		ret = conc_sm_visit(p);
		if (ret)
			return ret;
	}
	for (node = rb_first_cached(&rq->dl.pushable_dl_tasks_root); node; ) {
		p = rb_entry(node, struct task_struct, pushable_dl_tasks);
		node = rb_next(node);
		ret = conc_sm_visit(p);
		if (ret)
			return ret;
	}
#ifdef CONFIG_SCHED_CLASS_EXT
	list_for_each_entry_safe(p, n, &rq->scx.runnable_list, scx.runnable_node) {
		ret = conc_sm_visit(p);
		if (ret)
			return ret;
	}
#endif
#undef conc_sm_visit
	return 0;
}

/*
 * Check that the stopper can move @p to dst. Count @p in @g if it holds a
 * slot. Return -EAGAIN if @p cannot move: its migration is disabled, or
 * neither its mask nor its entitled mask allows dst.
 */
static int conc_sm_check(struct conc_set_move_arg *arg, struct task_struct *p,
		       struct conc_sm_group *g)
{
	const struct cpumask *mask = p == arg->mask_task ? arg->mask : p->cpus_ptr;

	if (is_migration_disabled(p))
		return -EAGAIN;
	if (!cpumask_test_cpu(arg->dst, mask) &&
	    (p == arg->mask_task || conc_task_mask_fixed(p) || !conc_sm_entitled(arg, p)))
		return -EAGAIN;
	if (p->conc_accounted)
		g->n++;
	return 0;
}

/*
 * Move @p from src to dst, from the stopper. Apply its entitled mask if its
 * own mask does not allow dst. A task which held no slot holds one
 * afterwards, and @g counts it.
 */
static int conc_sm_move(struct conc_set_move_arg *arg, struct task_struct *p,
		      struct conc_sm_group *g)
{
	struct rq *src_rq = cpu_rq(arg->src), *dst_rq = cpu_rq(arg->dst);
	const struct cpumask *ent = NULL;
	int i = -1;

	if (p != arg->mask_task && !cpumask_test_cpu(arg->dst, p->cpus_ptr))
		i = conc_sm_entitled_idx(arg, p);
	if (i >= 0)
		ent = conc_sm_ent_mask(arg, i);
	if (ent) {
		/*
		 * The cpuset of @p is ahead of its mask. Apply the entitled
		 * mask while @p is dequeued, as do_set_cpus_allowed() would.
		 * The pass of the cpuset then finds nothing left to do for
		 * @p. @p is queued on src and the lock of src is held. That
		 * excludes every other change of its mask and of its
		 * requested mask.
		 */
		struct affinity_context ac = { .new_mask = ent };

		deactivate_task(src_rq, p, 0);
		p->sched_class->set_cpus_allowed(p, &ac);
		set_task_cpu(p, arg->dst);
		activate_task(dst_rq, p, 0);
		wakeup_preempt(dst_rq, p, 0);
	} else {
		__move_queued_task_locked(src_rq, dst_rq, p);
	}
	if (!p->conc_accounted) {
		p->conc_accounted = 1;
		g->extra++;
	}
	return 0;
}

/* Runs on @src with @dst stopped too, see stop_two_cpus(). */
static int conc_set_move_stop(void *data)
{
	struct conc_set_move_arg *arg = data;
	struct rq *src_rq = cpu_rq(arg->src), *dst_rq = cpu_rq(arg->dst);
	int i, ret;

	/*
	 * Enqueue the wakeups already queued for this CPU, see
	 * migration_cpu_stop().
	 */
	flush_smp_call_function_queue();

	guard(double_rq_lock)(src_rq, dst_rq);
	/*
	 * A late arrival either claims its slot before the check, or
	 * selects again. If it claimed the slot and its task is not
	 * enqueued here yet, the count below does not match. Return -EAGAIN
	 * then. The caller tries again, up to CONC_EVICT_ROUNDS times.
	 */
	guard(raw_spinlock)(&conc_sm_lock);

	/*
	 * The destination started to go offline after it was chosen. Choose
	 * again.
	 */
	if (!conc_cpu_usable(arg->dst))
		return -EAGAIN;

	for (i = 0; i < arg->nr_groups; i++)
		arg->groups[i].n = arg->groups[i].extra = 0;

	/* Every task present on src must be movable ... */
	ret = conc_sm_walk(arg, src_rq, conc_sm_check);
	if (ret)
		return ret;
	/*
	 * ... and the tasks found must make up the whole slot count of each
	 * group on src.
	 */
	for (i = 0; i < arg->nr_groups; i++) {
		struct conc_sm_group *g = &arg->groups[i];
		int v = atomic_read(per_cpu_ptr(g->tg->conc.present, arg->src));

		if (v + CONC_SLOT_EVICT_BIAS - g->children != g->n)
			return -EAGAIN;
	}

	/* Hand the concurrency IDs over before any task can run on dst. */
	conc_sm_transfer_cids(arg);

	/* Move the tasks. Their slots follow with the relabel below. */
	conc_sm_walk(arg, src_rq, conc_sm_move);

	/*
	 * Relabel every slot. The entities counted on src are now counted
	 * on dst.
	 */
	for (i = 0; i < arg->nr_groups; i++) {
		struct conc_sm_group *g = &arg->groups[i];
		struct tg_concurrency *conc = &g->tg->conc;
		atomic_t *sslot = per_cpu_ptr(conc->present, arg->src);
		atomic_t *dslot = per_cpu_ptr(conc->present, arg->dst);
		int v, base;

		raw_spin_lock(&conc->lock);
		v = atomic_read(dslot);
		base = conc_slot_reserved(v) ? v + CONC_SLOT_RESERVE_BIAS : v;
		WARN_ON_ONCE(base < 0);
		/*
		 * Add, do not set. The presence can merge into a live slot. The
		 * lockless claim of conc_claim() may then count an arrival from
		 * another CPU between the read and the store.
		 */
		atomic_add(base - v + atomic_read(sslot) + CONC_SLOT_EVICT_BIAS + g->extra -
			   g->merge, dslot);
		atomic_set(sslot, CONC_SLOT_UNUSED);
		conc_used_del(conc, arg->src);
		conc->nr_evicting--;
		conc_slot_count_dropped(conc);
		raw_spin_unlock(&conc->lock);
	}
	/*
	 * The ancestors now count the top group on dst, and no longer on
	 * src.
	 */
	conc_ancestors_moved(arg->ancestors, arg->nr_ancestors, arg->src, arg->dst);
	return 0;
}

/*
 * Set-move the tasks of the subtree of @top from @src to @dst. With
 * @mask_task, that task is about to get @mask as its affinity, and @mask is
 * used in place of its own mask. Return 0 on success. On failure nothing
 * moved.
 */
static int conc_set_move(struct task_group *top, int src, int dst,
		       struct task_struct *mask_task, const struct cpumask *mask)
{
	struct conc_set_move_arg arg = {
		.src = src, .dst = dst, .mask_task = mask_task, .mask = mask,
	};
	struct cgroup_subsys_state *css;
	int i, nr, ret = -EBUSY, round;

	lockdep_assert_held(&conc_mutex);

	/*
	 * Skip the wait while conc_reserve_ancestors() makes headroom, see
	 * conc_reserving.
	 */
	if (!conc_reserving)
		conc_wait_attach();

	/* The groups of the subtree using src, top first. */
	nr = 0;
	rcu_read_lock();
	css_for_each_descendant_pre(css, &top->css)
		if (conc_tg_uses(css_tg(css), src))
			nr++;
	rcu_read_unlock();
	if (!nr)
		return 0;
	arg.groups = kvcalloc(nr, sizeof(*arg.groups), GFP_KERNEL);
	if (!arg.groups)
		return -ENOMEM;
	rcu_read_lock();
	css_for_each_descendant_pre(css, &top->css) {
		struct task_group *tg = css_tg(css);

		if (!conc_tg_uses(tg, src))
			continue;
		if (arg.nr_groups == nr || !css_tryget_online(css))
			break;
		arg.groups[arg.nr_groups++].tg = tg;
	}
	rcu_read_unlock();
	if (arg.nr_groups != nr)
		goto out_groups;
	for (i = 1; i < nr; i++) {
		struct conc_sm_group *pg = conc_sm_group_of(&arg, arg.groups[i].tg->parent);

		if (pg)
			pg->children++;
	}

	/*
	 * Stop arrivals on src and reserve dst, at every level. Start with
	 * the top group. If its slot on dst is new, the ancestors outside
	 * the set gain an entity there.
	 */
	for (i = 0; i < nr; i++) {
		struct conc_sm_group *g = &arg.groups[i];
		int r;

		if (!conc_mark_evicting(g->tg, src)) {
			ret = -EBUSY;
			goto out_revert;
		}
		g->evicting = true;
		/*
		 * The stopper removes its own slot on src, so this is a
		 * swap. It needs no headroom, see conc_reserve_slot().
		 */
		r = conc_reserve_slot(g->tg, dst, true);
		if (r < 0) {
			ret = r;
			goto out_revert;
		}
		g->reserved = r == 1;
		if (i == 0) {
			ret = conc_reserve_ancestors(top, src, dst, &arg.ancestors,
						   &arg.nr_ancestors, r == 1);
			if (ret)
				goto out_revert;
		} else if (r == 0) {
			struct conc_sm_group *pg = conc_sm_group_of(&arg, g->tg->parent);

			if (pg)
				pg->merge++;
		}
	}

	/* Claims made before the slots went negative finish their enqueue. */
	synchronize_rcu_expedited();

	/*
	 * Collect the domains whose CPU-owned ID on src must move to dst.
	 * These are the domains of every task of the groups, sleeping
	 * threads included. Then no thread which wakes up on dst allocates
	 * an ID while src still holds one for it.
	 */
	for (round = 0; round < CONC_EVICT_ROUNDS; round++) {
		/*
		 * Collect again before each attempt. A migration may have
		 * come and gone.
		 */
		conc_sm_free_mms(&arg);
		ret = conc_sm_collect_mms(&arg);
		if (ret == -EAGAIN)
			continue;
		if (ret)
			break;
		ret = conc_sm_collect_entitled(&arg);
		if (ret)
			break;
		ret = stop_two_cpus(src, dst, conc_set_move_stop, &arg);
		if (ret != -EAGAIN)
			break;
		schedule_timeout_uninterruptible(1);
	}
	conc_sm_free_entitled(&arg);
	if (!ret) {
		/*
		 * Safety net for the domains which appeared since the
		 * collection.
		 */
		for (i = 0; i < nr; i++)
			conc_slot_removed(arg.groups[i].tg, src);
	}

out_revert:
	conc_sm_free_mms(&arg);
	if (!ret) {
		/* Frees the ancestors. */
		conc_finish_ancestors(arg.ancestors, arg.nr_ancestors, src);
		/* A merge may have made room for a held task. */
		conc_hold_kick();
		goto out_groups;
	}
	for (i = nr - 1; i >= 0; i--) {
		struct conc_sm_group *g = &arg.groups[i];

		if (g->reserved)
			conc_unreserve_slot(g->tg, dst);
		if (g->evicting)
			conc_unmark_evicting(g->tg, src);
	}
	conc_unreserve_ancestors(arg.ancestors, arg.nr_ancestors, dst);
	kvfree(arg.ancestors);
out_groups:
	for (i = 0; i < arg.nr_groups; i++)
		css_put(&arg.groups[i].tg->css);
	kvfree(arg.groups);
	return ret;
}

/*
 * Make headroom in @tg for one more slot. Return true if there is headroom.
 *
 * Drain idle slots first. Then evict used CPUs whose tasks can all join.
 * Stop when one more slot fits within the limit.
 */
static bool conc_make_headroom(struct task_group *tg)
{
	struct tg_concurrency *conc = &tg->conc;
	cpumask_var_t cpus;
	int c;

	lockdep_assert_held(&conc_mutex);

	if (conc_has_room(conc))
		return true;
	if (!alloc_cpumask_var(&cpus, GFP_KERNEL))
		return false;
	cpumask_copy(cpus, conc->used);
	/*
	 * Drain the slots which are idle at this level, or which hold idle
	 * children only.
	 */
	for_each_cpu(c, cpus) {
		if (atomic_read(per_cpu_ptr(conc->present, c)) < 0)
			continue;
		if (!conc_drain_cpu(tg, c) && conc_has_room(conc))
			goto out;
	}
	for_each_cpu(c, cpus) {
		if (atomic_read(per_cpu_ptr(conc->present, c)) <= 0)
			continue;
		if (!conc_evict_cpu(tg, c, false, false, NULL, NULL, NULL) && conc_has_room(conc))
			goto out;
	}
out:
	free_cpumask_var(cpus);
	return conc_has_room(conc);
}

static int conc_resv_cpu(struct conc_resv *resv)
{
	return resv->cpu;
}

/*
 * Reserve a new CPU of @mask for a task of @tg. Make headroom first at
 * every level which needs it. Return the reservation, or NULL.
 *
 * The caller then:
 *  - sets the conc_resv_cpu of the task to the CPU, so that its arrival
 *    fills the slot, see conc_claim_slow();
 *  - migrates the task there;
 *  - calls conc_resv_finish().
 */
static struct conc_resv *conc_resv_make(struct task_group *tg, const struct cpumask *mask)
{
	struct task_group *lvl;
	struct conc_resv *resv;
	int c, dst = -1, r;

	lockdep_assert_held(&conc_mutex);

	conc_wait_attach();

	/* Prefer a CPU the parent uses, so that its count is unchanged. */
	if (tg->conc.propagate) {
		for_each_cpu_and(c, mask, tg->parent->conc.used) {
			if (conc_cpu_usable(c) && !cpumask_test_cpu(c, tg->conc.used)) {
				dst = c;
				break;
			}
		}
	}
	if (dst < 0) {
		for_each_cpu_and(c, mask, cpu_active_mask) {
			if (conc_cpu_usable(c) && !cpumask_test_cpu(c, tg->conc.used)) {
				dst = c;
				break;
			}
		}
	}
	if (dst < 0)
		return NULL;

	/* Headroom at every level that gets a new slot. */
	for (lvl = tg; ; lvl = lvl->parent) {
		if (!cpumask_test_cpu(dst, lvl->conc.used) && !conc_make_headroom(lvl))
			return NULL;
		if (!lvl->conc.propagate)
			break;
	}

	resv = kzalloc(sizeof(*resv), GFP_KERNEL);
	if (!resv)
		return NULL;
	resv->tg = tg;
	resv->cpu = dst;
	if (conc_reserve_ancestors(tg, -1, dst, &resv->anc, &resv->nr_anc, true)) {
		conc_unreserve_ancestors(resv->anc, resv->nr_anc, dst);
		kvfree(resv->anc);
		kfree(resv);
		return NULL;
	}
	r = conc_reserve_slot(tg, dst, false);
	if (r < 0) {
		conc_unreserve_ancestors(resv->anc, resv->nr_anc, dst);
		kvfree(resv->anc);
		kfree(resv);
		return NULL;
	}
	if (r == 0) {
		/*
		 * A lockless claim created the slot after @dst was chosen.
		 * That claim propagated its own entity. Release the
		 * entities claimed above for a new slot. Otherwise the slot
		 * would stay counted twice.
		 */
		for (c = 0; c < resv->nr_anc; c++) {
			if (!resv->anc[c].entity)
				continue;
			conc_release(resv->anc[c].tg, dst);
			resv->anc[c].entity = false;
		}
	}
	/* The task can leave @tg before conc_resv_finish(). */
	css_get(&tg->css);
	return resv;
}

/*
 * Settle the slots of @resv after the task landed on the reserved CPU, or
 * did not. Free @resv.
 *
 * Remove an empty slot, see conc_remove_reserved(). It is not left idle.
 * The caller cleared the conc_resv_cpu of the task before, so nothing can
 * land once the slot is found empty here.
 */
static void conc_resv_finish(struct conc_resv *resv)
{
	struct tg_concurrency *conc = &resv->tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, resv->cpu);
	unsigned long flags;
	bool landed;

	lockdep_assert_held(&conc_mutex);

	raw_spin_lock_irqsave(&conc->lock, flags);
	landed = atomic_read(slot) != -CONC_SLOT_RESERVE_BIAS;
	if (landed && conc_slot_reserved(atomic_read(slot)))
		atomic_add(CONC_SLOT_RESERVE_BIAS, slot);
	raw_spin_unlock_irqrestore(&conc->lock, flags);
	if (!landed)
		conc_remove_reserved(resv->tg, resv->cpu);

	if (landed)
		conc_ancestors_moved(resv->anc, resv->nr_anc, -1, resv->cpu);
	else
		conc_unreserve_ancestors(resv->anc, resv->nr_anc, resv->cpu);
	kvfree(resv->anc);
	css_put(&resv->tg->css);
	kfree(resv);
	/* A slot was settled or removed: a held task may fit now. */
	conc_hold_kick();
}

/*
 * Make sure that a task of @tg which is not present finds a CPU when it
 * wakes up with @mask. Return 0 on success, -EBUSY otherwise.
 *
 * First try to make headroom for a CPU of @mask at every level. The wakeup
 * then adds the slot. If that fails, set-move the tasks which the group has
 * on one of its CPUs onto a CPU of @mask.
 */
static int conc_make_reachable(struct task_group *tg, const struct cpumask *mask)
{
	struct task_group *lvl;
	struct conc_resv *resv;
	int cpu;

	lockdep_assert_held(&conc_mutex);

	resv = conc_resv_make(tg, mask);
	if (resv) {
		/* Headroom exists now. Drop the empty reservation. */
		conc_resv_finish(resv);
		return 0;
	}
	/*
	 * Set-move the tasks on one used CPU onto @mask. Try the CPUs of
	 * the group itself first. Then try each counting level above which
	 * has no used CPU in @mask. Such a level can be at its limit
	 * because of another child. The busy tasks of a sibling group keep
	 * the parent's slots outside @mask. Nothing below the parent can
	 * move them.
	 */
	for (lvl = tg; lvl; lvl = lvl->conc.propagate ? lvl->parent : NULL) {
		if (lvl != tg && cpumask_intersects(lvl->conc.used, mask))
			break;
		for_each_cpu(cpu, lvl->conc.used) {
			if (atomic_read(per_cpu_ptr(lvl->conc.present, cpu)) < 0)
				continue;
			if (!conc_evict_cpu(lvl, cpu, true, false, NULL, NULL, mask))
				return 0;
		}
	}
	return -EBUSY;
}

static void conc_hold_irq_work_fn(struct irq_work *work)
{
	mod_delayed_work(system_dfl_wq, &conc_hold_work, 0);
}

/* Run the placement work after an event which may let a held task in. */
void conc_hold_kick(void)
{
	if (atomic_read(&conc_nr_held))
		irq_work_queue(&conc_hold_irq_work);
}

#ifndef CONFIG_SCHED_MAX_CONCURRENCY_DEBUG
static inline void conc_debug_hold_start(struct task_struct *p) { }

static inline void conc_debug_hold_stop(struct task_struct *p) { }

static inline void conc_debug_hold_end(struct task_struct *p, const char *how) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY_DEBUG */

/*
 * Count the start of a hold of @p, see cpu.concurrency.stat. Each group
 * counts its subtree. Call under the runqueue lock of @p.
 */
static void conc_stat_hold_start(struct task_struct *p)
{
	struct task_group *tg;

	p->conc_hold_at = ktime_get_ns();
	/* A move ends a hold and asks for a new one. The wait goes on. */
	if (!p->conc_wait_at)
		WRITE_ONCE(p->conc_wait_at, p->conc_hold_at);
	for (tg = task_group(p); tg; tg = tg->parent) {
		atomic_inc(&tg->conc.st_nr_held);
		atomic_long_inc(&tg->conc.st_nr_holds);
	}
}

/* The hold of @p ends. With @last, its wait ends too. */
static void conc_stat_hold_stop(struct task_struct *p, bool last)
{
	u64 delta = ktime_get_ns() - p->conc_hold_at;
	bool rotated = READ_ONCE(p->conc_rotated);
	struct task_group *tg;

	for (tg = task_group(p); tg; tg = tg->parent) {
		atomic_dec(&tg->conc.st_nr_held);
		atomic64_add(delta, &tg->conc.st_held_ns);
		if (rotated)
			atomic64_add(delta, &tg->conc.st_rotated_ns);
	}
	if (last) {
		WRITE_ONCE(p->conc_rotated, 0);
		WRITE_ONCE(p->conc_wait_at, 0);
	}
}

/*
 * Ask for @p to be held at its next return to user space, for @why. Return
 * false if @p cannot be held. The caller holds p->pi_lock.
 *
 * The hold runs from resume_user_mode_work(), see conc_hold_resume(). No
 * lock is held there. The over-high throttling of memcg waits in the same
 * place. A task work is not used: task_work_run() has callers which hold
 * locks, such as the SQPOLL thread of io_uring under its ring's lock.
 *
 * A task which cannot be held runs on without a slot and takes no
 * concurrency ID:
 *  - A kernel thread never returns to user space.
 *  - An io_uring worker (PF_IO_WORKER) is not held. The SQPOLL thread runs
 *    the work of that return under its ring's lock when it tears the ring
 *    down.
 *  - An exiting task (PF_EXITING) never returns to user space. do_exit()
 *    calls sched_mm_cid_exit() before exit_signals() sets the flag, so the
 *    task has left its ID domains.
 *
 * A kernel thread, an io_uring worker and a vhost worker are users of no ID
 * domain, see sched_mm_cid_fork().
 *
 * A vhost worker is held. It runs the work of that return at the top of its
 * loop, where nothing is locked.
 */
bool conc_hold_request(struct task_struct *p, u8 why)
{
	lockdep_assert_held(&p->pi_lock);
	if (p->conc_hold_pending) {
		p->conc_hold_pending |= why;
		return true;
	}
	if (p->flags & (PF_KTHREAD | PF_IO_WORKER | PF_EXITING))
		return false;
	p->conc_hold_pending = why;
	/*
	 * The store above and set_notify_resume() are fully ordered against
	 * resume_user_mode_work(), which clears the flag before it reads
	 * conc_hold_pending.
	 */
	set_notify_resume(p);
	return true;
}

/*
 * @p is current. Take the IDs which it did not take while a hold was
 * pending. These IDs are given outside a context switch. The exit to user
 * space writes changed IDs only after a context switch, and clears the
 * change otherwise. So force the rseq update here.
 *
 * Skip a domain in which @p owns a transit ID. A mode switch gave it to
 * @p while it runs, see cid_transit_to_cpu(). A schedule in never finds
 * such an ID on the task: the schedule out before it converts it. Running
 * one over it would allocate a second ID, or lose the first one when no
 * other ID is free.
 */
static void conc_cid_refresh(struct task_struct *p)
{
#ifdef CONFIG_SCHED_MM_CID
	if (!cid_in_transit(p->mm_cid.cid))
		mm_cid_schedin(p);
	rseq_force_update();
#endif
}

/*
 * Hold @p, which is current. The caller holds task_rq_lock().
 *
 * Dequeue @p from its class. It stays queued for the core, in TASK_RUNNING.
 * It is not asleep, so a wakeup does not touch it. The placement work
 * enqueues it again.
 */
static void conc_hold(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_held(&p->pi_lock);
	lockdep_assert_rq_held(rq);
	WARN_ON_ONCE(p != current || p->conc_held || !task_on_rq_queued(p));

	/*
	 * Dequeue from the class only. The accounting of the core stays,
	 * see dequeue_task().
	 */
	WARN_ON_ONCE(!p->sched_class->dequeue_task(rq, p, DEQUEUE_SLEEP | DEQUEUE_SPECIAL |
						     DEQUEUE_NOCLOCK));
	conc_task_release(p, cpu_of(rq));
	p->conc_held = 1;
	conc_debug_hold_start(p);
	conc_stat_hold_start(p);
	get_task_struct(p);
	atomic_inc(&conc_nr_held);
	/*
	 * The placement work may have listed it already, see
	 * conc_hold_work_fn().
	 */
	scoped_guard(raw_spinlock, &conc_hold_lock) {
		if (list_empty(&p->conc_hold_node))
			list_add_tail(&p->conc_hold_node, &conc_held_list);
	}
	/* Room may exist already, elsewhere in its mask. */
	conc_hold_kick();
	resched_curr(rq);
}

/*
 * End the hold of @p because it blocks. Called from dequeue_task() with
 * DEQUEUE_SLEEP, under the runqueue lock. @p is a plain sleeper from here
 * on. Its wakeup places it.
 */
void conc_held_blocks(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_rq_held(rq);
	p->conc_held = 0;
	conc_debug_hold_stop(p);
	conc_debug_hold_end(p, "blocked");
	conc_stat_hold_stop(p, true);
	scoped_guard(raw_spinlock, &conc_hold_lock)
		list_del_init(&p->conc_hold_node);
	if (atomic_dec_and_test(&conc_nr_held))
		irq_work_queue(&conc_held_wake_work);
	put_task_struct(p);
}

/*
 * End the hold of @p. Enqueue it in its class on its runqueue, as a wakeup.
 * The caller holds the runqueue lock.
 *
 * The class saw a sleep at the hold. A wakeup on the same runqueue keeps
 * the bookkeeping of the class right (deadline bandwidth and timers, load
 * tracking). What the core does next is then done to a queued task: a
 * migration, or a class change with switched_from() and switched_to().
 *
 * With @rehold, a new hold is requested and @p goes on without a slot until
 * its return to user space.
 *
 * Callers:
 *  - dequeue_task() with @rehold, for every dequeue which is not a sleep;
 *  - deactivate_task() with @rehold;
 *  - conc_try_place() without @rehold, when it has a slot for @p;
 *  - conc_try_place() with @rehold, for a task with SIGKILL pending.
 */
void conc_unhold(struct rq *rq, struct task_struct *p, bool rehold)
{
	int on_rq = p->on_rq;

	lockdep_assert_rq_held(rq);
	/*
	 * @p must be queued for the core, so that the class sees a wakeup,
	 * see deactivate_task().
	 */
	WARN_ON_ONCE(on_rq != TASK_ON_RQ_QUEUED);
	p->conc_held = 0;
	conc_debug_hold_stop(p);
	conc_stat_hold_stop(p, !rehold);
	p->sched_class->enqueue_task(rq, p, ENQUEUE_WAKEUP | ENQUEUE_NOCLOCK);
	/*
	 * A wakeup or a migration reschedules the target CPU. A change made
	 * in place does not: a priority change, or a cpuset widened by a
	 * CPU coming online. An idle CPU would then never pick the task. So
	 * reschedule here.
	 */
	if (!task_current(rq, p))
		resched_curr(rq);
	scoped_guard(raw_spinlock, &conc_hold_lock)
		list_del_init(&p->conc_hold_node);
	if (atomic_dec_and_test(&conc_nr_held))
		irq_work_queue(&conc_held_wake_work);
	if (rehold)
		conc_hold_request(p, CONC_HOLD_UNPLACED);
	else
		conc_debug_hold_end(p, "placed");
	put_task_struct(p);
}

/*
 * Return true if @p, which is current, need not be held. @from is the CPU
 * it was on when conc_hold_resume() began. The caller holds the runqueue
 * lock.
 *
 * There are two such cases.
 *
 * Its group does not count any more: a limit was removed, and the key may
 * be off. This applies to a hold for an eviction too. The hot paths test
 * the key first and would not see the task as held.
 *
 * The task has a slot, or can claim one on its CPU. A load balancer move, a
 * group change or a set-move may have placed it. This does not apply when
 * @p is still on @from and that CPU is being emptied (CONC_HOLD_EVICT).
 */
static bool conc_hold_settled(struct task_struct *p, int from)
{
	if (!conc_task_active(p))
		return true;
	if ((p->conc_hold_pending & CONC_HOLD_EVICT) && task_cpu(p) == from)
		return false;
	return p->conc_accounted || conc_task_claim(p, task_cpu(p));
}

/*
 * Let an rt or deadline task place itself at its return to user space. This
 * includes a lock owner boosted by priority inheritance.
 *
 * The placement work runs in a kworker of the fair class. The tasks which
 * wait for the held task can keep that kworker off the CPU. So the task
 * does the work at its own priority.
 *
 * The hold stays pending meanwhile, so the task takes no concurrency ID.
 *
 * If its group uses a CPU of its mask, the task moves there, as
 * sched_exec() moves a running task. The move claims the slot, see
 * conc_task_move(). If not, the task calls conc_make_reachable() once, as
 * the placement work would. Then it tries again.
 *
 * The task tries a few times. If it is still not placed, it is held like
 * any other task.
 */
static void conc_place_self(struct task_struct *p, int from)
{
	bool repaired = false;
	int attempt, dest;

	for (attempt = 0; attempt < 4; attempt++) {
		struct migration_arg arg = { .task = p };
		struct task_group *tg;
		cpumask_var_t mask;

		scoped_guard(task_rq_lock, p) {
			if (!p->conc_hold_pending || fatal_signal_pending(p) ||
			    conc_hold_settled(p, from))
				return;
			dest = conc_join_dest(p, task_cpu(p), NULL, NULL);
		}
		if (dest >= 0) {
			arg.dest_cpu = dest;
			stop_one_cpu(task_cpu(p), migration_cpu_stop, &arg);
			continue;
		}
		if (repaired || !alloc_cpumask_var(&mask, GFP_KERNEL))
			return;
		repaired = true;
		tg = conc_pin_task_group(p);
		if (tg) {
			scoped_guard(raw_spinlock_irq, &p->pi_lock)
				cpumask_copy(mask, p->cpus_ptr);
			mutex_lock(&conc_mutex);
			conc_make_reachable(tg, mask);
			mutex_unlock(&conc_mutex);
			css_put(&tg->css);
		}
		free_cpumask_var(mask);
	}
}

/*
 * Hold current if a hold is pending, see conc_hold_request().
 *
 * Called from resume_user_mode_work(), before the rseq update which writes
 * the concurrency IDs. A vhost worker calls it at the top of its loop.
 *
 * A task which is held schedules out here and comes back when it is placed.
 * The rest of the work, the rseq update included, then runs with the IDs it
 * took. If the task was moved instead, a new hold was requested with the
 * same notification. The exit to user space checks it again.
 */
void conc_hold_resume(void)
{
	struct task_struct *p = current;
	int from = task_cpu(p);
	bool self = false, held = false;

	if (p->flags & PF_EXITING) {
		if (IS_ENABLED(CONFIG_SCHED_MAX_CONCURRENCY_DEBUG)) {
			scoped_guard(task_rq_lock, p)
				conc_debug_hold_end(p, "exiting");
		}
		return;
	}
	for (;;) {
		scoped_guard(task_rq_lock, p) {
			if (!p->conc_hold_pending)
				return;
			/* Never held with SIGKILL pending, see conc_fatal_signal(). */
			if (fatal_signal_pending(p)) {
				conc_debug_hold_end(p, "killed");
				return;
			}
			if (conc_hold_settled(p, from)) {
				p->conc_hold_pending = 0;
				conc_debug_hold_end(p, self ? "placed itself" :
						    "placed at its return");
				conc_cid_refresh(p);
				return;
			}
			if (self || !rt_or_dl_task(p)) {
				p->conc_hold_pending = 0;
				update_rq_clock(scope.rq);
				conc_hold(scope.rq, p);
				held = true;
			}
		}
		if (held)
			break;
		conc_place_self(p, from);
		self = true;
	}
	/*
	 * @p is out of its class. Schedule out until it is placed, or until
	 * a move ends the hold.
	 */
	if (READ_ONCE(p->conc_held))
		schedule();
}

/**
 * conc_fatal_signal - let a held task with SIGKILL pending go
 * @t: the task. Called from signal_wake_up_state() under its siglock.
 *
 * A held task is not asleep, so the wakeup of the signal does not reach it.
 * It would only die once a slot frees.
 *
 * A task with SIGKILL pending is never held, see conc_hold_resume(). A task
 * which is already held is let go: the placement work returns it to its
 * class without a slot, see conc_try_place().
 *
 * In both cases the hold stays pending, so the task takes no concurrency
 * ID, see conc_task_unplaced(). The exit to user space delivers the signal
 * before the task leaves the kernel. On architectures without the generic
 * entry code, the rseq update can run before that.
 * rseq_slowpath_update_usr() skips a task with SIGKILL pending. do_exit()
 * then removes the task from its ID domains.
 *
 * Until it exits, the task may run on a CPU beyond the bound, without an
 * ID.
 *
 * p->pi_lock orders this against the hold. Either conc_hold_resume() sees
 * SIGKILL pending, or this function sees conc_held set.
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
 * Try to place held task @p.
 *
 * Select a CPU as for a wakeup, under p->pi_lock only, as try_to_wake_up()
 * does. The runqueue lock is not held then, because the selection may take
 * it to change the task's mask (a fallback widening, see
 * conc_select_account()). Then take the runqueue lock and move the task as
 * move_queued_task() does.
 *
 * Return 1 if @p was placed or is not held any more. Return 0 if it stays
 * held. Return -EAGAIN if it is still on its CPU, about to schedule out.
 */
static int conc_try_place(struct task_struct *p)
{
	int wake_flags = WF_TTWU, cpu, ret;
	struct rq_flags rf;
	struct rq *rq;

	guard(raw_spinlock_irqsave)(&p->pi_lock);

	if (!p->conc_held)
		return 1;
	/* Pairs with the release of p->on_cpu in finish_task(). */
	if (smp_load_acquire(&p->on_cpu))
		return -EAGAIN;
	/*
	 * SIGKILL is pending, see conc_fatal_signal(). Let @p go where it
	 * is, without a slot. If its CPU went offline while it was held,
	 * move it to a fallback CPU, as balance_push() would have done.
	 *
	 * A held task is out of its class. It does not move under its
	 * runqueue lock alone, so task_cpu() is stable here.
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
	 * The last active CPU of @p's mask went offline while @p was held.
	 *
	 * Nothing else widens @p. balance_push() pushes the tasks which are
	 * queued in a class on the CPU going offline. A held task is not in
	 * its class, and it may be on another runqueue. The hotplug pass of
	 * its cpuset comes later, and gives a task of the top cpuset no
	 * CPU.
	 *
	 * So do what select_fallback_rq() does for a queued task: widen the
	 * mask. The change of the mask ends the hold and requests a new
	 * one, see conc_unhold(). Then move @p off the inactive runqueue,
	 * without a slot if needed. @p is held again at its return to user
	 * space and placed by its new mask.
	 *
	 * @p may already run when the runqueue lock is taken again.
	 * __migrate_task() does not move a running task. balance_push()
	 * moves it then.
	 */
	if (!cpumask_intersects(p->cpus_ptr, cpu_active_mask)) {
		cpu = select_fallback_rq(task_cpu(p), p);
		rq = __task_rq_lock(p, &rf);
		if (task_on_rq_queued(p) && !p->conc_held && !cpu_active(cpu_of(rq)) &&
		    !task_on_cpu(rq, p)) {
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
		 * @p is not held any more. Either it blocked, and its
		 * wakeup will place it: give the slot back. Or the widening
		 * of the selection ended its hold, and no slot was claimed.
		 */
		conc_task_release(p, cpu);
		ret = 1;
		goto out;
	}
	update_rq_clock(rq);
	/* Wake @p up where it is. Then move it as move_queued_task() does. */
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

/* The lowest scheduling class which a task of a cgroup can have. */
#ifdef CONFIG_SCHED_CLASS_EXT
#define CONC_LOWEST_CLASS	(&ext_sched_class)
#else
#define CONC_LOWEST_CLASS	(&fair_sched_class)
#endif

/*
 * Return the highest scheduling class among the tasks of the subtree of
 * @lvl which hold its slot on @cpu. Return NULL if no task holds it. Set
 * @fixed if one of these tasks cannot be held, see conc_hold_request().
 * The slot cannot be emptied then.
 *
 * The tasks are read without their locks. A task which claims or releases
 * the slot meanwhile can be missed. The rotation then takes a slot one
 * look early or late.
 *
 * This walks the tasks of every group of the subtree which uses @cpu. The
 * rotation calls it for a slot which it may take, at most once per look.
 */
static const struct sched_class *conc_slot_class(struct task_group *lvl, int cpu, bool *fixed)
{
	const struct sched_class *class = NULL;
	struct cgroup_subsys_state *css;

	guard(rcu)();
	css_for_each_descendant_pre(css, &lvl->css) {
		struct css_task_iter it;
		struct task_struct *t;

		if (!conc_tg_uses(css_tg(css), cpu))
			continue;
		css_task_iter_start(css, 0, &it);
		while ((t = css_task_iter_next(&it))) {
			const struct sched_class *c = READ_ONCE(t->sched_class);

			if (task_cpu(t) != cpu || !READ_ONCE(t->conc_accounted))
				continue;
			if (READ_ONCE(t->flags) & (PF_KTHREAD | PF_IO_WORKER | PF_EXITING))
				*fixed = true;
			if (!class || sched_class_above(c, class))
				class = c;
		}
		css_task_iter_end(&it);
	}
	return class;
}

/*
 * Rotation: take a slot from running tasks for held task @p.
 *
 * Every affinity mask is accepted under a limit. So the used CPUs of a
 * group at its limit can all be outside the mask of one of its tasks. That
 * task is held. It must not wait for as long as the other tasks run.
 *
 * @p is in @tg and its mask is @mask. Nothing within the limit fits it.
 * Choose a used CPU outside @mask, at the level which is at its limit.
 * Empty it: its tasks join other used CPUs, or are held (conc_evict_cpu()).
 *
 * The class of a slot is the highest class among the tasks which hold it,
 * see conc_slot_class(). A slot can be taken in two cases:
 *  - its class is below the class of @p;
 *  - its class is the class of @p, @expired is set (the hold of @p lasted
 *    a rotation period) and the slot is at least one period old.
 * A slot of a class above the class of @p is never taken. Among the slots
 * which can be taken, the victim is the one used for the longest time.
 *
 * Return 0 if a slot was emptied, -EBUSY otherwise.
 */
static int conc_rotate_for(struct task_struct *p, struct task_group *tg,
			 const struct cpumask *mask, bool expired)
{
	u64 now = ktime_get_ns(), period = conc_rotation_ns();
	const struct sched_class *pc = READ_ONCE(p->sched_class);
	struct task_group *lvl, *g;

	lockdep_assert_held(&conc_mutex);

	for (lvl = tg; lvl; lvl = lvl->conc.propagate ? lvl->parent : NULL) {
		u64 oldest = U64_MAX;
		int cpu, victim = -1;

		if (lvl != tg && cpumask_intersects(lvl->conc.used, mask))
			break;
		for_each_cpu(cpu, lvl->conc.used) {
			u64 since = READ_ONCE(*per_cpu_ptr(lvl->conc.since, cpu));
			const struct sched_class *vc;
			bool due, fixed = false;

			if (cpumask_test_cpu(cpu, mask) ||
			    atomic_read(per_cpu_ptr(lvl->conc.present, cpu)) < 0)
				continue;
			/*
			 * A slot which is not due can only be taken from a
			 * lower class. Skip the walk of the tasks if no
			 * task can be in a class below the one of @p.
			 */
			due = expired && now - since >= period;
			if (!due && !sched_class_above(pc, CONC_LOWEST_CLASS))
				continue;
			vc = conc_slot_class(lvl, cpu, &fixed);
			/* Its eviction would fail at every look. Try another slot. */
			if (fixed)
				continue;
			/*
			 * A slot without a task tells nothing: its tasks may
			 * be about to claim it. Treat it as the class of @p.
			 */
			if (!vc)
				vc = pc;
			if (sched_class_above(vc, pc))
				continue;
			if (!sched_class_above(pc, vc) && !due)
				continue;
			if (since < oldest) {
				oldest = since;
				victim = cpu;
			}
		}
		if (victim < 0)
			continue;
		if (conc_evict_cpu(lvl, victim, false, true, NULL, NULL, NULL))
			continue;
		trace_sched_conc_rotate(p, mask, victim, cgroup_id(lvl->css.cgroup));
		for (g = lvl; g; g = g->parent) {
			atomic_long_inc(&g->conc.st_nr_rotations);
			if (g->parent)
				cgroup_file_notify(&g->conc.events_file);
		}
		return 0;
	}
	return -EBUSY;
}

static void conc_rotate_work_fn(struct work_struct *work)
{
	mod_delayed_work(system_dfl_wq, &conc_hold_work, 0);
}
static DECLARE_DELAYED_WORK(conc_rotate_work, conc_rotate_work_fn);

static void conc_hold_work_fn(struct work_struct *work)
{
	bool again = false;
	LIST_HEAD(still);
	int todo;

	for (todo = atomic_read(&conc_nr_held); todo > 0; todo--) {
		struct task_struct *p = NULL;
		cpumask_var_t mask;
		int ret;

		scoped_guard(raw_spinlock_irq, &conc_hold_lock) {
			p = list_first_entry_or_null(&conc_held_list, struct task_struct,
						     conc_hold_node);
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
				ret = conc_try_place(p);
				/* Still held. Take a slot for it. */
				if (!ret) {
					u64 since = READ_ONCE(p->conc_wait_at);
					bool expired = since && ktime_get_ns() - since >=
						       conc_rotation_ns();
					struct conc_resv *resv = NULL;
					int r;

					/*
					 * Reserve the freed room for @p. Otherwise
					 * the tasks just evicted, or a wakeup, can
					 * take it first.
					 */
					mutex_lock(&conc_mutex);
					r = conc_rotate_for(p, tg, mask, expired);
					if (!r)
						resv = conc_resv_make(tg, mask);
					if (resv) {
						scoped_guard(raw_spinlock_irq, &p->pi_lock)
							p->conc_resv_cpu = conc_resv_cpu(resv);
					}
					mutex_unlock(&conc_mutex);
					if (!r) {
						struct rq_flags rf;
						struct rq *rq;

						/* The hold can end meanwhile. */
						rq = task_rq_lock(p, &rf);
						if (p->conc_held)
							WRITE_ONCE(p->conc_rotated, 1);
						task_rq_unlock(rq, p, &rf);
						ret = conc_try_place(p);
					}
					if (resv) {
						scoped_guard(raw_spinlock_irq, &p->pi_lock)
							p->conc_resv_cpu = -1;
						mutex_lock(&conc_mutex);
						conc_resv_finish(resv);
						mutex_unlock(&conc_mutex);
					}
				}
				css_put(&tg->css);
			}
			free_cpumask_var(mask);
		}
		if (ret <= 0) {
			/*
			 * The task is still held, or was held again: a
			 * migration ended its hold and it was held at its next
			 * return to user space. In the second case conc_hold()
			 * may have put it on the list already. Add it only if
			 * it is not on the list.
			 *
			 * Keep the tasks in the order of their holds. The task
			 * held first is served first when a slot comes of age.
			 */
			scoped_guard(raw_spinlock_irq, &conc_hold_lock) {
				if (READ_ONCE(p->conc_held) && list_empty(&p->conc_hold_node))
					list_add_tail(&p->conc_hold_node, &still);
			}
			if (ret < 0)
				again = true;
		}
		put_task_struct(p);
	}
	scoped_guard(raw_spinlock_irq, &conc_hold_lock)
		list_splice_init(&still, &conc_held_list);
	if (again)
		mod_delayed_work(system_dfl_wq, &conc_hold_work, 1);
	/* Look again while tasks are held: a rotation period may expire. */
	if (atomic_read(&conc_nr_held))
		mod_delayed_work(system_dfl_wq, &conc_rotate_work,
				 max(1UL, nsecs_to_jiffies(conc_rotation_ns()) / 4));
}

/*
 * Request a hold for @t if it holds a slot of @tg on @cpu. Return false if
 * @t is there and cannot be held.
 *
 * If @t is not on a CPU, release its slot now. @t is then queued without a
 * slot, with the hold pending. It runs no user space code before it is
 * held: the hold is done at its return to user space. Otherwise a task
 * which a higher class keeps off the CPU would keep its slot until it runs.
 */
static bool conc_hold_task_on(struct task_struct *t, struct task_group *tg, int cpu)
{
	struct rq_flags rf;
	struct rq *rq;

	guard(raw_spinlock_irq)(&t->pi_lock);
	if (task_cpu(t) != cpu || !READ_ONCE(t->conc_accounted) || task_group(t) != tg)
		return true;
	if (!conc_hold_request(t, CONC_HOLD_EVICT))
		return false;
	rq = __task_rq_lock(t, &rf);
	if (!task_on_cpu(rq, t))
		conc_task_release(t, cpu);
	__task_rq_unlock(rq, t, &rf);
	return true;
}

/*
 * Empty @cpu of @tg's subtree by holding its tasks.
 *
 * Request a hold for every task of the subtree which holds a slot on @cpu.
 * These are the tasks which could neither join nor be set-moved. Refuse
 * arrivals on the CPU while waiting for them. Drain the slot once it is
 * empty.
 *
 * Return -EBUSY if a task cannot be held (a kernel thread), or if the slot
 * is not empty after CONC_HOLD_ROUNDS tries. The slot then stays, and
 * shrink_pending with it.
 */
static int conc_hold_cpu(struct task_group *tg, int cpu)
{
	struct cgroup_subsys_state *css;
	struct task_group **groups;
	int i, n = 0, nr = 0, marked, round, ret = -EBUSY;

	lockdep_assert_held(&conc_mutex);

	rcu_read_lock();
	css_for_each_descendant_pre(css, &tg->css)
		if (conc_tg_uses(css_tg(css), cpu))
			nr++;
	rcu_read_unlock();
	if (!nr)
		return conc_drain_cpu(tg, cpu);
	groups = kvcalloc(nr, sizeof(*groups), GFP_KERNEL);
	if (!groups)
		return -ENOMEM;
	rcu_read_lock();
	css_for_each_descendant_pre(css, &tg->css) {
		if (!conc_tg_uses(css_tg(css), cpu))
			continue;
		if (n == nr)
			break;
		/* A dying group has no task to hold. */
		if (!css_tryget_online(css))
			continue;
		groups[n++] = css_tg(css);
	}
	rcu_read_unlock();

	for (round = 0; round < CONC_HOLD_ROUNDS; round++) {
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
			/* Shorter than a jiffy: a rotation hands a slot over. */
			if (ok)
				usleep_range(50, 100);
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

/* A descendant group whose slot an eviction marks, see conc_evict_cpu(). */
struct conc_evict_mark {
	struct task_group	*tg;
	bool			marked;
};

/*
 * Collect the descendants of @tg which use @cpu. Return the array and set
 * @nr to its length. Return NULL if there is none, or if the allocation
 * failed: the marks of the descendants are an optimization.
 */
static struct conc_evict_mark *conc_evict_marks_alloc(struct task_group *tg, int cpu, int *nr)
{
	struct cgroup_subsys_state *css;
	struct conc_evict_mark *marks;
	int n = 0, max = 0;

	*nr = 0;
	rcu_read_lock();
	css_for_each_descendant_pre(css, &tg->css)
		if (css_tg(css) != tg && conc_tg_uses(css_tg(css), cpu))
			max++;
	rcu_read_unlock();
	if (!max)
		return NULL;
	marks = kvcalloc(max, sizeof(*marks), GFP_KERNEL);
	if (!marks)
		return NULL;
	rcu_read_lock();
	css_for_each_descendant_pre(css, &tg->css) {
		if (css_tg(css) == tg || !conc_tg_uses(css_tg(css), cpu))
			continue;
		if (n == max)
			break;
		/* No task arrives in a dying group. */
		if (!css_tryget_online(css))
			continue;
		marks[n++].tg = css_tg(css);
	}
	rcu_read_unlock();
	*nr = n;
	return marks;
}

/* Mark the slots which can be marked. The others stay open to arrivals. */
static void conc_evict_marks_set(struct conc_evict_mark *marks, int nr, int cpu)
{
	int i;

	for (i = 0; i < nr; i++)
		marks[i].marked = conc_mark_evicting(marks[i].tg, cpu);
}

static void conc_evict_marks_clear(struct conc_evict_mark *marks, int nr, int cpu)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (marks[i].marked)
			conc_unmark_evicting(marks[i].tg, cpu);
		marks[i].marked = false;
	}
}

static void conc_evict_marks_free(struct conc_evict_mark *marks, int nr)
{
	int i;

	for (i = 0; i < nr; i++)
		css_put(&marks[i].tg->css);
	kvfree(marks);
}

/*
 * Return the first CPU of @mask, and of @and when given, which is a
 * housekeeping CPU of the scheduling domains (HK_TYPE_DOMAIN). If there is
 * none, return the first CPU of any kind.
 *
 * The scheduler puts a task on a CPU outside the domains (isolcpus=domain,
 * an isolated partition) only when the task's affinity asks for it. A
 * set-move must not bring a group's tasks and a stopper onto such a CPU by
 * its own choice. It takes one only when the masks allow no other CPU, so
 * that a group pinned there still works.
 */
static int conc_first_dest(const struct cpumask *mask, const struct cpumask *and)
{
	int cpu;

	scoped_guard(rcu) {
		for_each_cpu_and(cpu, mask, housekeeping_cpumask(HK_TYPE_DOMAIN)) {
			if (!and || cpumask_test_cpu(cpu, and))
				return cpu;
		}
	}
	return and ? cpumask_first_and(mask, and) : cpumask_first(mask);
}

/*
 * Return a destination CPU for the tasks of @top's subtree which still hold
 * a slot on @src, or -1. @scratch is a work mask of the caller.
 *
 * The CPU must be active and not going offline. It must be in @within when
 * given. Every such task must be allowed on it. @mask_task is checked
 * against @mask. Any other task is checked against the mask which its
 * cpuset entitles it to. The stopper applies that mask when it moves the
 * task. If no CPU fits the entitled masks, the current masks are used.
 *
 * The order of preference:
 *  - a CPU which @top already uses. The slots merge and the CPU count of
 *    @top drops. A join cannot do that for a task whose mask is behind its
 *    cpuset;
 *  - a CPU which the parent uses, so that the count of the parent is
 *    unchanged;
 *  - a new CPU. A housekeeping CPU is preferred, see conc_first_dest().
 */
static int conc_set_move_dest(struct task_group *top, int src, struct cpumask *scratch,
			    const struct cpumask *within,
			    struct task_struct *mask_task, const struct cpumask *mask)
{
	struct cgroup_subsys_state *css;
	cpumask_var_t tmp, alt;
	bool have_tmp;
	int cpu;

	have_tmp = alloc_cpumask_var(&tmp, GFP_KERNEL);
	if (have_tmp && !alloc_cpumask_var(&alt, GFP_KERNEL)) {
		free_cpumask_var(tmp);
		have_tmp = false;
	}

	cpumask_copy(scratch, within ?: cpu_active_mask);
	cpumask_and(scratch, scratch, cpu_active_mask);
	cpumask_andnot(scratch, scratch, &conc_offlining_cpus);
	cpumask_clear_cpu(src, scratch);
	/*
	 * @alt is narrowed by the current masks only. @scratch is narrowed
	 * by the entitled masks.
	 */
	if (have_tmp)
		cpumask_copy(alt, scratch);

	rcu_read_lock();
	css_for_each_descendant_pre(css, &top->css) {
		struct css_task_iter it;
		struct task_struct *t;

		if (!conc_tg_uses(css_tg(css), src))
			continue;
		css_task_iter_start(css, 0, &it);
		while ((t = css_task_iter_next(&it))) {
			/*
			 * Only the tasks which hold a slot on @src count. The
			 * stopper moves queued tasks only. A sleeper released
			 * its slot. It wakes up through conc_select_cpu(),
			 * which finds it a CPU of its own mask, or holds it.
			 */
			if (task_cpu(t) != src || !READ_ONCE(t->conc_accounted))
				continue;
			if (t == mask_task) {
				cpumask_and(scratch, scratch, mask);
				if (have_tmp)
					cpumask_and(alt, alt, mask);
			} else if (have_tmp && !conc_task_mask_fixed(t)) {
				conc_task_entitled(t, tmp);
				cpumask_and(scratch, scratch, tmp);
				cpumask_and(alt, alt, t->cpus_ptr);
			} else {
				cpumask_and(scratch, scratch, t->cpus_ptr);
				if (have_tmp)
					cpumask_and(alt, alt, t->cpus_ptr);
			}
		}
		css_task_iter_end(&it);
	}
	rcu_read_unlock();

	if (have_tmp) {
		if (cpumask_empty(scratch))
			cpumask_copy(scratch, alt);
		free_cpumask_var(tmp);
		free_cpumask_var(alt);
	}

	/*
	 * Try first a CPU of the own used set whose slot is not in a
	 * transient state.
	 */
	for_each_cpu_and(cpu, scratch, top->conc.used) {
		if (atomic_read(per_cpu_ptr(top->conc.present, cpu)) >= 0)
			return cpu;
	}
	cpumask_andnot(scratch, scratch, top->conc.used);
	if (top->conc.propagate) {
		cpu = conc_first_dest(scratch, top->parent->conc.used);
		if (cpu < nr_cpu_ids)
			return cpu;
	}
	cpu = conc_first_dest(scratch, NULL);
	return cpu < nr_cpu_ids ? cpu : -1;
}

/*
 * Empty @cpu of the tasks of @tg's subtree. Return 0 when the slot is gone.
 * Return -EBUSY and leave the slot in place when nothing works.
 *
 * The steps:
 *  - tasks join used CPUs of their own group;
 *  - with @set_move, the rest is set-moved to a new CPU, taken from @within
 *    when given;
 *  - with @hold, what is left is held, see conc_hold_cpu().
 *
 * @mask_task, when given, is checked against @mask in place of its own mask
 * (an affinity change is in progress).
 */
static int conc_evict_cpu(struct task_group *tg, int cpu, bool set_move, bool hold,
			struct task_struct *mask_task, const struct cpumask *mask,
			const struct cpumask *within)
{
	struct tg_concurrency *conc = &tg->conc;
	atomic_t *slot = per_cpu_ptr(conc->present, cpu);
	cpumask_var_t scratch;
	int ret = -EBUSY, v, round, remaining = 0, nr_marks;
	struct conc_evict_mark *marks;
	bool stuck = false;

	lockdep_assert_held(&conc_mutex);

	v = atomic_read(slot);
	if (v == CONC_SLOT_UNUSED || v == CONC_SLOT_DRAINING)
		return 0;
	/*
	 * Drain an idle slot at once. A task which wakes up claims a slot
	 * without the lock and may take this one first. Then evict that
	 * task, as from a busy slot. Do not give up: the group would stay
	 * over its limit with no task held and nothing to retry.
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
	/*
	 * The mark of @tg refuses a task which would bring a new slot to
	 * @cpu. A descendant which has a slot there still takes arrivals.
	 * Mark these slots too.
	 */
	marks = conc_evict_marks_alloc(tg, cpu, &nr_marks);
	conc_evict_marks_set(marks, nr_marks, cpu);

	/* Join: move each task to a CPU its group already uses. */
	for (round = 0; round < CONC_EVICT_ROUNDS; round++) {
		struct cgroup_subsys_state *css;

		remaining = 0;
		stuck = false;
		rcu_read_lock();
		css_for_each_descendant_pre(css, &tg->css) {
			struct css_task_iter it;
			struct task_struct *t;

			if (!conc_tg_uses(css_tg(css), cpu))
				continue;
			if (!css_tryget_online(css))
				continue;
			rcu_read_unlock();
			css_task_iter_start(css, 0, &it);
			while ((t = css_task_iter_next(&it))) {
				struct migration_arg arg = { .task = t };
				int dest;

				/*
				 * Move the present tasks only. A sleeper holds
				 * no slot.
				 */
				if (task_cpu(t) != cpu || !READ_ONCE(t->conc_accounted))
					continue;
				dest = conc_join_dest(t, cpu, t == mask_task ? mask : NULL, within);
				if (dest < 0) {
					remaining++;
					continue;
				}
				arg.dest_cpu = dest;
				stop_one_cpu(cpu, migration_cpu_stop, &arg);
				/* Its migration is disabled, or the move was refused. */
				if (task_cpu(t) == cpu && READ_ONCE(t->conc_accounted))
					stuck = true;
			}
			css_task_iter_end(&it);
			rcu_read_lock();
			css_put(css);
		}
		rcu_read_unlock();

		/*
		 * Everything joined. The slots left are idle and can be
		 * drained.
		 */
		if (!remaining) {
			conc_evict_marks_clear(marks, nr_marks, cpu);
			conc_unmark_evicting(tg, cpu);
			ret = conc_drain_cpu(tg, cpu);
			if (!ret)
				break;
			/*
			 * The drain failed. A task arrived late, or a child
			 * slot is not idle after all. Try again.
			 */
			if (!conc_mark_evicting(tg, cpu)) {
				conc_evict_marks_free(marks, nr_marks);
				return -EBUSY;
			}
			conc_evict_marks_set(marks, nr_marks, cpu);
			schedule_timeout_uninterruptible(1);
			continue;
		}
		break;
	}

	conc_evict_marks_clear(marks, nr_marks, cpu);
	conc_evict_marks_free(marks, nr_marks);
	/* The slot is gone, and its mark with it. */
	if (!ret)
		return 0;
	conc_unmark_evicting(tg, cpu);
	/*
	 * Go on with the set-move and the hold when tasks could not join, or
	 * when a task which had a CPU to join did not move. Otherwise the
	 * slot keeps an entity which this walk cannot see, such as an
	 * exiting task. Nothing here can move it.
	 */
	if (!remaining && !stuck)
		return ret;
	if (!alloc_cpumask_var(&scratch, GFP_KERNEL))
		return -ENOMEM;

	/*
	 * Consolidate. Set-move a child subtree whose tasks could not join
	 * within the child onto a CPU which this level already uses. That
	 * adds no CPU at this level. Then check the children again.
	 */
	for (round = 0; round < CONC_EVICT_ROUNDS; round++) {
		struct task_group *child, *pinned = NULL;

		rcu_read_lock();
		list_for_each_entry_rcu(child, &tg->children, siblings) {
			if (!conc_tg_uses(child, cpu) ||
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
	/*
	 * No common destination. Hold what is left if the caller asks for
	 * it.
	 */
	return hold ? conc_hold_cpu(tg, cpu) : -EBUSY;
}

/*
 * Evict CPUs of @tg until it is within its effective limit. Take the CPU
 * with the lowest slot count first. Tasks join, and the tasks which cannot
 * join are held. A set-move is not used: it would not reduce the count.
 * Leave shrink_pending set if @tg is still over its limit.
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

/*
 * Shrink every group of the subtree which is over its limit, innermost
 * first. Return true if a group freed a CPU.
 */
static bool conc_shrink_tree(struct task_group *top)
{
	struct cgroup_subsys_state *css;
	unsigned int before;
	bool shrunk = false;

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
		before = READ_ONCE(tg->conc.nr_used);
		conc_shrink(tg);
		/* A shrink which freed no CPU changed nothing. */
		if (READ_ONCE(tg->conc.nr_used) < before)
			shrunk = true;
		rcu_read_lock();
		css_put(css);
	}
	rcu_read_unlock();
	return shrunk;
}

/*
 * Re-evaluate the limits which cover @tg, after an event which can unblock
 * a stalled shrink. The caller holds conc_mutex and a reference on @tg.
 */
static void __conc_retry(struct task_group *tg)
{
	struct task_group *top = tg;
	struct cpumask *scratch;
	bool changed;

	lockdep_assert_held(&conc_mutex);

	/* The topmost active group: effective values flow down from there. */
	if (!top->conc.active)
		return;
	while (top->parent && top->parent->conc.active)
		top = top->parent;

	scratch = conc_alloc_scratch();
	if (!scratch)
		return;
	/* A walk which failed for lack of memory is tried again as a change. */
	changed = conc_update_tree(top, scratch) != 0;
	changed |= conc_shrink_tree(top);
	/*
	 * Applying the bounds visits every task. Skip it, and the
	 * notification of the effective files, when the retry changed
	 * nothing.
	 */
	if (changed) {
		conc_bounds_apply(top);
		conc_notify_tree(top);
	}
	kfree(scratch);
	conc_hold_kick();
}

static void conc_retry(struct task_group *tg)
{
	if (!conc_active(&tg->conc))
		return;
	mutex_lock(&conc_mutex);
	__conc_retry(tg);
	mutex_unlock(&conc_mutex);
}

/* Return true if @mask has an active CPU other than @cpu. */
static bool conc_other_active_cpu(const struct cpumask *mask, int cpu)
{
	return cpumask_any_and_but(mask, cpu_active_mask, cpu) < nr_cpu_ids;
}

/*
 * Widen the masks of the tasks of @tg's subtree which are queued on @cpu
 * and have no other active CPU in their mask. Give them now the mask which
 * select_fallback_rq() would give them when balance_push() pushes them.
 * They can then move with their group.
 *
 * A sleeping task keeps its mask, as it does without a limit.
 * select_fallback_rq() widens it only if it wakes up before the CPU comes
 * back. During suspend it does not wake up.
 */
static void conc_widen_pinned(struct task_group *tg, int cpu)
{
	struct cgroup_subsys_state *css;

	rcu_read_lock();
	css_for_each_descendant_pre(css, &tg->css) {
		struct css_task_iter it;
		struct task_struct *t;

		if (!conc_tg_uses(css_tg(css), cpu) || !css_tryget_online(css))
			continue;
		rcu_read_unlock();
		css_task_iter_start(css, 0, &it);
		while ((t = css_task_iter_next(&it))) {
			bool widened = false;

			if (task_cpu(t) != cpu || t->flags & PF_KTHREAD)
				continue;
			scoped_guard(raw_spinlock_irq, &t->pi_lock) {
				/*
				 * Skip a sleeping task. p->pi_lock keeps a
				 * wakeup from queueing it meanwhile. Skip a
				 * task with migration disabled: it is pinned
				 * for a short time only, and balance_push()
				 * leaves it alone too. @cpu is still active, so
				 * leave it out when looking for another active
				 * CPU in the mask.
				 */
				if (!READ_ONCE(t->on_rq) || is_migration_disabled(t) ||
				    conc_other_active_cpu(&t->cpus_mask, cpu))
					break;
				if (!cpuset_cpus_allowed_fallback(t) ||
				    !conc_other_active_cpu(t->cpus_ptr, cpu))
					set_cpus_allowed_force(t, task_cpu_fallback_mask(t));
				widened = true;
			}
			if (widened && t->mm && printk_ratelimit())
				printk_deferred("process %d (%s) no longer affine to cpu%d\n",
						task_pid_nr(t), t->comm, cpu);
		}
		css_task_iter_end(&it);
		rcu_read_lock();
		css_put(css);
	}
	rcu_read_unlock();
}

/*
 * Move the limited groups off @cpu before it goes offline.
 *
 * Called from sched_cpu_deactivate() while the CPU is still active, before
 * balance_push() pushes its tasks away. Evict the CPU from every topmost
 * counting group: tasks join, are set-moved or are held. A set-move covers
 * the subtree.
 *
 * The eviction can fail, for example for a kernel thread or a task with
 * migration disabled. Its result is not checked. balance_push() then moves
 * what is left, without a slot if the limit refuses the destination.
 *
 * New slots on the CPU are refused from here until conc_cpu_offline(). The
 * CPU is still active, so a task which wakes up with no other CPU lands on
 * it as a late arrival. The set-move takes it along. Otherwise it would run
 * elsewhere without a slot.
 */
void conc_cpu_deactivate(unsigned int cpu)
{
	struct task_group *tg;

	cpumask_set_cpu(cpu, &conc_offlining_cpus);
	if (!static_branch_unlikely(&sched_conc_enabled))
		return;
	/*
	 * Wait for the claims which started before the CPU was set in
	 * conc_offlining_cpus. Their slots are then visible in the used
	 * sets tested below.
	 */
	synchronize_rcu_expedited();

	mutex_lock(&conc_mutex);
	rcu_read_lock();
	list_for_each_entry_rcu(tg, &task_groups, list) {
		if (!conc_active(&tg->conc) || !cpumask_test_cpu(cpu, tg->conc.used))
			continue;
		if (tg->parent && conc_active(&tg->parent->conc))
			continue;
		if (!css_tryget_online(&tg->css))
			continue;
		rcu_read_unlock();
		/* The reference pins the list entry, see sched_release_group(). */
		conc_widen_pinned(tg, cpu);
		conc_evict_cpu(tg, cpu, true, true, NULL, NULL, NULL);
		rcu_read_lock();
		css_put(&tg->css);
	}
	rcu_read_unlock();
	mutex_unlock(&conc_mutex);
}

/*
 * @cpu is online again. Set the state of its SMT core in the unused_cores
 * mask of every group. The core is unused if the group uses no CPU of it,
 * and used otherwise.
 *
 * conc_used_add() and conc_used_del() update unused_cores with
 * cpu_smt_mask(). That mask did not hold @cpu for its siblings while @cpu
 * was offline.
 */
static void conc_core_online(int cpu)
{
	const struct cpumask *core = cpu_smt_mask(cpu);
	struct task_group *tg;

	rcu_read_lock();
	list_for_each_entry_rcu(tg, &task_groups, list) {
		struct tg_concurrency *conc = &tg->conc;
		unsigned long flags;

		/*
		 * The group never counted and has no masks. If it allocates
		 * them now, every core starts unused.
		 */
		if (!smp_load_acquire(&conc->present))
			continue;
		raw_spin_lock_irqsave(&conc->lock, flags);
		if (cpumask_intersects(core, conc->used))
			cpumask_andnot(conc->unused_cores, conc->unused_cores, core);
		else
			cpumask_or(conc->unused_cores, conc->unused_cores, core);
		raw_spin_unlock_irqrestore(&conc->lock, flags);
	}
	rcu_read_unlock();
}

/*
 * Called when @cpu comes online, or when its deactivation is rolled back.
 * Slots may be added on it again.
 */
void conc_cpu_activate(unsigned int cpu)
{
	cpumask_clear_cpu(cpu, &conc_offlining_cpus);
	/*
	 * Do this whether or not a group counts now. A used set outlives
	 * counting.
	 */
	conc_core_online(cpu);
	conc_hold_kick();
}

/* Is a limit of @top's subtree stalled? Under RCU. */
static bool conc_subtree_needs_retry(struct task_group *top)
{
	struct cgroup_subsys_state *css;

	css_for_each_descendant_pre(css, &top->css) {
		if (conc_needs_retry(css_tg(css)))
			return true;
	}
	return false;
}

/*
 * Finish the unplug of @cpu. Called from sched_cpu_wait_empty().
 *
 * The runqueue is empty, so the slots on the CPU are idle, except for the
 * cases listed below. Drop the CPU from every used set. Then retry the
 * groups which the unplug left over their limit.
 */
void conc_cpu_offline(unsigned int cpu)
{
	struct task_group *tg;

	if (!static_branch_unlikely(&sched_conc_enabled)) {
		cpumask_clear_cpu(cpu, &conc_offlining_cpus);
		return;
	}

	mutex_lock(&conc_mutex);
	rcu_read_lock();
	list_for_each_entry_rcu(tg, &task_groups, list) {
		if (!conc_active(&tg->conc) || !cpumask_test_cpu(cpu, tg->conc.used))
			continue;
		if (!css_tryget_online(&tg->css))
			continue;
		rcu_read_unlock();
		/*
		 * The reference pins the list entry, see
		 * sched_release_group().
		 *
		 * The drain can fail, because a slot can keep an entity:
		 *  - a throttled deadline task, which its replenishment
		 *    timer moves later, see dl_task_offline_migration();
		 *  - a reservation in flight, which conc_resv_finish()
		 *    settles.
		 * The slot then stays until its owner leaves it.
		 */
		conc_drain_cpu(tg, cpu);
		rcu_read_lock();
		css_put(&tg->css);
	}
	/*
	 * Retry the limits which the unplug left over the limit. A retry
	 * walks the whole subtree of a topmost counting group, so run it once
	 * per topmost group, after every slot was drained.
	 */
	list_for_each_entry_rcu(tg, &task_groups, list) {
		if (!conc_active(&tg->conc) ||
		    (tg->parent && conc_active(&tg->parent->conc)))
			continue;
		if (!conc_subtree_needs_retry(tg) || !css_tryget_online(&tg->css))
			continue;
		rcu_read_unlock();
		__conc_retry(tg);
		rcu_read_lock();
		css_put(&tg->css);
	}
	rcu_read_unlock();
	cpumask_clear_cpu(cpu, &conc_offlining_cpus);
	mutex_unlock(&conc_mutex);
	conc_hold_kick();
}

/* Return the group of @p with a reference, or NULL if it does not count. */
static struct task_group *conc_pin_task_group(struct task_struct *p)
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

/*
 * Prepare an affinity change of @p. Called from __set_cpus_allowed_ptr()
 * before the mask changes.
 *
 * The new mask may hold no used CPU of @p's group. @p would then need a new
 * CPU and the group would use one more. Vacate the current CPU of @p instead.
 * The other tasks there join used CPUs. The rest, with @p, is set-moved to a
 * CPU of the new mask. The migration which follows then finds @p in place and
 * the group uses no extra CPU.
 *
 * If this is not possible, @p migrates without a slot and is held at its next
 * return to user space (conc_task_move()). The rotation then gives it a slot.
 *
 * A cgroup migration of @p can run at the same time. What is prepared here
 * then serves the group which @p leaves, and @p is held in its new group.
 */
void conc_prepare_affinity(struct task_struct *p, struct affinity_context *ctx)
{
	const struct cpumask *new_mask = ctx->new_mask;
	struct task_group *tg;
	cpumask_var_t eff;
	bool have_eff = false;
	int src, c;

	ctx->conc.resv = NULL;
	ctx->conc.pending = false;
	ctx->conc.unchanged = false;
	tg = conc_pin_task_group(p);
	if (!tg)
		return;
	/*
	 * A change which does not come from sched_setaffinity(2), such as a
	 * cpuset change, is restricted to the requested mask when the two
	 * intersect. See __set_cpus_allowed_ptr(). Prepare for the mask which
	 * will be applied.
	 */
	if (!(ctx->flags & SCA_USER) && alloc_cpumask_var(&eff, GFP_KERNEL)) {
		bool same;

		have_eff = true;
		scoped_guard(raw_spinlock_irq, &p->pi_lock) {
			if (p->user_cpus_ptr && cpumask_and(eff, ctx->new_mask, p->user_cpus_ptr))
				new_mask = eff;
			same = cpumask_equal(new_mask, &p->cpus_mask);
		}
		/*
		 * The mask does not change. cpuset_fork() does this for every
		 * child, except in the top cpuset. The change will return
		 * early in __set_cpus_allowed_ptr_locked(). There is no CPU to
		 * vacate, so do not take conc_mutex.
		 */
		if (same) {
			free_cpumask_var(eff);
			css_put(&tg->css);
			return;
		}
	}
	src = task_cpu(p);
	mutex_lock(&conc_mutex);
	/* @p can join a used CPU. conc_pick_dest() or the wakeup takes it. */
	for_each_cpu_and(c, new_mask, tg->conc.used) {
		if (cpu_active(c) && atomic_read(per_cpu_ptr(tg->conc.present, c)) >= 0)
			goto pending;
	}
	if (!tg->conc.active)
		goto pending;
	/*
	 * Every CPU of the mask is offline or going offline, so no slot can
	 * be made. Do not try. The rounds below would only sleep with
	 * conc_mutex held. Every other change, the placement of the held
	 * tasks and the hotplug pass of cpuset wait for this lock.
	 *
	 * If the CPUs are offline, __set_cpus_allowed_ptr_locked() refuses
	 * the change. If they are going offline, the change is applied, and
	 * the unplug then widens the mask or holds the task.
	 */
	for_each_cpu(c, new_mask) {
		if (conc_cpu_usable(c))
			break;
	}
	if (c >= nr_cpu_ids)
		goto pending;
	/*
	 * Try once, and do not sleep with conc_mutex held. A mask which the
	 * limit cannot serve is no transient state: the other tasks of the
	 * group can be pinned elsewhere. @p then moves without a slot, is
	 * held at its return to user space, and the rotation serves it.
	 */
	if (!READ_ONCE(p->conc_accounted) || task_cpu(p) != src) {
		/*
		 * @p is not present here. It wakes up later and must then
		 * find a CPU of the mask.
		 */
		conc_make_reachable(tg, new_mask);
	} else if (conc_evict_cpu(tg, src, true, false, p, new_mask, NULL)) {
		/*
		 * The CPU could not be vacated: that would have set-moved
		 * the presence of the group, with @p. So @p alone needs a
		 * new CPU. Make headroom and reserve a slot. The migration
		 * which follows goes to that CPU (conc_pick_dest()) and
		 * fills the slot (conc_claim_slow()).
		 */
		ctx->conc.resv = conc_resv_make(tg, new_mask);
		if (ctx->conc.resv) {
			scoped_guard(raw_spinlock_irq, &p->pi_lock)
				p->conc_resv_cpu = conc_resv_cpu(ctx->conc.resv);
		}
	}
pending:
	/*
	 * Count the change as pending until it is applied. Set-moves wait for
	 * it (conc_wait_attach()), so what was prepared above stays valid.
	 * conc_task_affinity_changed() ends the pending state.
	 */
	atomic_inc(&conc_attach_pending);
	ctx->conc.pending = true;
	mutex_unlock(&conc_mutex);
	if (have_eff)
		free_cpumask_var(eff);
	css_put(&tg->css);
}

/*
 * Return true if a limit which covers @tg is exceeded and its shrink is
 * stalled. A retry can then change something.
 */
static bool conc_needs_retry(struct task_group *tg)
{
	struct task_group *lvl;

	for_each_conc_level_from(lvl, tg) {
		if (READ_ONCE(lvl->conc.shrink_pending))
			return true;
	}
	return false;
}

/*
 * The groups to retry at the end of a cpuset pass. Protected by conc_mutex.
 *
 * A cpuset pass (cpuset_update_tasks_cpumask()) changes the masks of all the
 * tasks of a cpuset, one after the other, under cpuset_mutex. It runs for a
 * write to cpuset.cpus, for its propagation and for CPU hotplug. A retry
 * after each task would walk the subtree once per task. So the topmost
 * groups to retry are listed, each once, and retried at the end of
 * the pass (conc_pass_end()).
 */
static LIST_HEAD(conc_pass_retries);

void conc_pass_begin(void)
{
	current->conc_in_pass = 1;
}

static void conc_pass_defer(struct task_group *tg)
{
	struct task_group *top = tg;

	while (top->parent && conc_active(&top->parent->conc))
		top = top->parent;
	guard(mutex)(&conc_mutex);
	if (list_empty(&top->conc.pass_node)) {
		css_get(&top->css);
		list_add_tail(&top->conc.pass_node, &conc_pass_retries);
	}
}

void conc_pass_end(void)
{
	struct task_group *tg, *tmp;

	current->conc_in_pass = 0;
	/* Only this pass adds to the list: cpuset_mutex orders the passes. */
	if (list_empty(&conc_pass_retries))
		return;
	guard(mutex)(&conc_mutex);
	list_for_each_entry_safe(tg, tmp, &conc_pass_retries, conc.pass_node) {
		bool retry;

		list_del_init(&tg->conc.pass_node);
		/* The limit to retry may be the limit of a descendant. */
		scoped_guard(rcu)
			retry = conc_subtree_needs_retry(tg);
		if (retry)
			__conc_retry(tg);
		css_put(&tg->css);
	}
}

/*
 * End an affinity change of @p. Called from __set_cpus_allowed_ptr() after
 * the change. Undo what conc_prepare_affinity() prepared. Then retry the
 * limits if a shrink is stalled: the change may let it go on. Nothing else
 * needs the walk.
 */
void conc_task_affinity_changed(struct task_struct *p, struct affinity_context *ctx)
{
	struct task_group *tg;

	if (ctx->conc.pending) {
		ctx->conc.pending = false;
		if (atomic_dec_and_test(&conc_attach_pending))
			wake_up(&conc_attach_wq);
	}
	if (ctx->conc.resv) {
		scoped_guard(raw_spinlock_irq, &p->pi_lock)
			p->conc_resv_cpu = -1;
		mutex_lock(&conc_mutex);
		conc_resv_finish(ctx->conc.resv);
		mutex_unlock(&conc_mutex);
		ctx->conc.resv = NULL;
	}
	/*
	 * @p is held. Its new mask may hold a used CPU, see
	 * conc_hold_work_fn().
	 */
	if (READ_ONCE(p->conc_held))
		conc_hold_kick();
	tg = conc_pin_task_group(p);
	if (!tg)
		return;
	/*
	 * Do not retry for a change which left the mask as it was.
	 * cpuset_fork() makes such a change for every child. A cpuset pass
	 * retries once, at its end (conc_pass_end()).
	 */
	if (conc_needs_retry(tg) && !ctx->conc.unchanged) {
		if (current->conc_in_pass)
			conc_pass_defer(tg);
		else
			conc_retry(tg);
	}
	css_put(&tg->css);
}

/*
 * cpuset_mutex orders the limit writes against the changes of the partition
 * roots and against the deadline admission. CONFIG_SCHED_MAX_CONCURRENCY
 * depends on CONFIG_CPUSETS for this.
 */
static inline void conc_cfg_lock_settled(void) { cpuset_lock_no_attach(); }

static inline void conc_cfg_unlock(void) { cpuset_unlock(); }

/**
 * conc_dl_admit - may @p become a SCHED_DEADLINE task?
 * @p: the task. The caller is __sched_setscheduler(), for a user request of
 *     SCHED_DEADLINE which the deadline admission sees (admission on, not
 *     SCHED_FLAG_SUGOV). It holds task_rq_lock() and cpuset_mutex.
 *
 * A deadline task is not allowed in a group which counts, see conc_dl_task().
 *
 * A limit which starts is serialized by cpuset_mutex. It finds the task with
 * or without the deadline policy (conc_subtree_dl_start()). A migration
 * checked after this call reads the new policy (__conc_attach_check()).
 *
 * Return: 0 if @p may become a deadline task. -EPERM if its group counts, as
 * for an affinity narrower than the root domain. -EAGAIN if a migration of @p
 * has not finished (p->conc_move_seq is odd, see __conc_attach_check()). It
 * was checked with the old policy, and the destination may count. The caller
 * must then drop its locks, call conc_dl_wait() and check again.
 */
int conc_dl_admit(struct task_struct *p)
{
	lockdep_assert_held(&p->pi_lock);
	if (!static_branch_unlikely(&sched_conc_enabled))
		return 0;
	if (READ_ONCE(task_group(p)->conc.active)) {
		trace_sched_conc_refuse(p, "sched_setattr", "SCHED_DEADLINE under a limit", -EPERM);
		return -EPERM;
	}
	if (p->conc_move_seq & 1)
		return -EAGAIN;
	return 0;
}

/*
 * Wait for the migration of @p which made conc_dl_admit() return -EAGAIN.
 * Call with no lock held.
 */
void conc_dl_wait(struct task_struct *p)
{
	unsigned int seq = READ_ONCE(p->conc_move_seq);

	if (seq & 1)
		wait_event(conc_attach_wq, READ_ONCE(p->conc_move_seq) != seq);
}

/*
 * Handle the exit of task @p. Called from the release callback of the cpu
 * controller.
 */
void conc_task_released(struct task_struct *p)
{
	struct task_group *tg;

	conc_debug_hold_end(p, "exited");
	tg = conc_pin_task_group(p);
	if (!tg)
		return;
	/* A shrink is stalled. The excess may go away with the task. */
	if (conc_needs_retry(tg))
		conc_retry(tg);
	css_put(&tg->css);
}

int conc_attach_record_srcs(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct task_struct *task;

	/*
	 * No group counts, so there is no source group to record.
	 * conc_attach_check() counted the migration as pending until
	 * conc_attach_finish().
	 */
	if (!static_branch_unlikely(&sched_conc_enabled))
		return 0;

	conc_attach_drop_idle_cids(tset);

	cgroup_taskset_for_each(task, css, tset) {
		struct task_group *tg = conc_pin_task_group(task);
		struct conc_attach_src *src;

		if (!tg)
			continue;
		list_for_each_entry(src, &conc_attach_srcs, node) {
			if (src->tg == tg)
				break;
		}
		if (&src->node != &conc_attach_srcs) {
			css_put(&tg->css);
			continue;
		}
		src = kmalloc(sizeof(*src), GFP_KERNEL);
		if (!src) {
			css_put(&tg->css);
			return -ENOMEM;
		}
		src->tg = tg;
		list_add(&src->node, &conc_attach_srcs);
	}
	return 0;
}

void conc_attach_finish(struct cgroup_taskset *tset, bool retry)
{
	struct cgroup_subsys_state *css, *last = NULL;
	struct conc_attach_src *src, *tmp;
	struct task_struct *task;
	bool checked = false;

	/*
	 * The tasks are in place. Make conc_move_seq even again. A change of
	 * policy which waits for it then checks the task where it is
	 * (conc_dl_admit()).
	 */
	cgroup_taskset_for_each(task, css, tset) {
		/*
		 * Only the check of this migration made the count odd. That
		 * check and this code both run under cgroup_mutex. So read
		 * the count without p->pi_lock. A migration then costs
		 * nothing more while no group has a limit.
		 */
		if (!(READ_ONCE(task->conc_move_seq) & 1))
			continue;
		scoped_guard(raw_spinlock_irq, &task->pi_lock)
			WRITE_ONCE(task->conc_move_seq, task->conc_move_seq + 1);
		checked = true;
	}

	/* The retries below may set-move again. */
	current->conc_attaching = 0;
	if (atomic_dec_and_test(&conc_attach_pending) || checked)
		wake_up(&conc_attach_wq);

	if (retry && static_branch_unlikely(&sched_conc_enabled)) {
		cgroup_taskset_for_each(task, css, tset) {
			if (css == last)
				continue;
			last = css;
			if (READ_ONCE(css_tg(css)->conc.active))
				conc_retry(css_tg(css));
		}
	}
	list_for_each_entry_safe(src, tmp, &conc_attach_srcs, node) {
		if (retry)
			conc_retry(src->tg);
		css_put(&src->tg->css);
		list_del(&src->node);
		kfree(src);
	}
	/*
	 * Apply the effect of the move on the per-mm CID bounds before the
	 * write returns. While no group counts there is none. A limit which
	 * starts waits for this migration and then applies the bounds itself.
	 * The update runs between configuration walks, as every bound update
	 * does (conc_bounds_lock()).
	 */
	if (retry && static_branch_unlikely(&sched_conc_enabled)) {
		guard(mutex)(&conc_mutex);
		conc_update_tset_mm_bounds(tset);
	}
	conc_attach_forget_mms();
}

/*
 * Handle a write to cpu.concurrency.max. @limit is the value written, 0 for
 * "max". Return 0 or a negative error code.
 *
 * The write does not fail because of the masks of the tasks. It can fail
 * because of the deadline tasks of the subtree, see conc_subtree_dl_start().
 *
 * A group with a ceiling has it since its creation (conc_tg_take_ceiling())
 * and counts already. A write to such a group starts nothing.
 *
 * The static key holds one reference per group with a limit or a ceiling.
 * Change it outside conc_mutex: a jump label update takes cpus_read_lock(),
 * and CPU hotplug takes conc_mutex under the hotplug lock.
 */
static int conc_set_request(struct task_group *tg, unsigned int limit)
{
	unsigned int ceiling = tg->conc.ceiling, ceil_eff = ceiling;
	struct cpumask *scratch;
	bool was, now, start;
	int ret = 0;

	scratch = conc_alloc_scratch();
	if (!scratch)
		return -ENOMEM;

	/*
	 * Take a provisional reference, so that the key is on during the
	 * checks. It is kept or dropped at the end of the function.
	 */
	conc_key_get();
	/*
	 * Turn the key on before the pending migrations are read. Pairs with
	 * the barrier of conc_attach_check().
	 */
	smp_mb();

	start = limit && !conc_active(&tg->conc);
again:
	/*
	 * Take cpuset_mutex before conc_mutex, as a cpuset write does. The
	 * deadline checks below need it. A change of a partition root, a
	 * change of policy and the start of a limit then exclude each other.
	 *
	 * Use cpuset_lock_no_attach(): conc_wait_attach() below waits for
	 * migrations with these locks held, as a set-move does.
	 */
	conc_cfg_lock_settled();
	mutex_lock(&conc_mutex);
	if (start && conc_scx_active) {
		ret = -EBUSY;
		goto out;
	}
	/*
	 * Wait for the migrations which were checked before. The deadline
	 * checks below then see their tasks in place.
	 */
	conc_wait_attach();
	if (limit && !tg->conc.active && !start) {
		/*
		 * The limits of its ancestors went away meanwhile. This write
		 * starts a limit after all.
		 */
		mutex_unlock(&conc_mutex);
		conc_cfg_unlock();
		start = true;
		goto again;
	}
	if (tg->parent->conc.ceiling_effective &&
	    (!ceil_eff || ceil_eff > tg->parent->conc.ceiling_effective))
		ceil_eff = tg->parent->conc.ceiling_effective;
	if (limit && ceil_eff && limit > ceil_eff) {
		ret = -EINVAL;
		goto out;
	}
	/*
	 * The subtree starts to count. Check its deadline tasks, see
	 * conc_subtree_dl_start().
	 */
	if (limit && !tg->conc.active) {
		ret = conc_subtree_dl_start(tg);
		if (ret)
			goto out;
	}
	if (limit || ceiling || tg->parent->conc.active) {
		ret = conc_subtree_state_alloc(tg);
		if (ret)
			goto out;
	}
	was = tg->conc.request || ceiling;
	now = limit || ceiling;
	swap(tg->conc.request, limit);
	ret = conc_update_tree(tg, scratch);
	if (ret < 0) {
		/* Nothing was changed. Keep the limit as it was. */
		tg->conc.request = limit;
		goto out;
	}
	ret = 0;
	conc_shrink_tree(tg);
	conc_bounds_apply(tg);
	conc_notify_tree(tg);
	conc_hold_kick();
out:
	mutex_unlock(&conc_mutex);
	conc_cfg_unlock();

	/*
	 * Keep one reference per group with a limit or a ceiling. If the
	 * group gained its first one, the provisional reference becomes the
	 * reference of the group. Otherwise drop it. A group which lost its
	 * limit and has no ceiling drops its own reference too.
	 */
	if (ret || now == was) {
		conc_key_put();
	} else if (!now) {
		conc_key_put();
		conc_key_put();
	}

	kfree(scratch);
	return ret;
}

/*
 * Give the new group @tg its ceiling, from the child_ceiling of @parent. The
 * group keeps it for its whole life, because processes may size per-ID data
 * by it. Return 0 or a negative error code.
 *
 * Call before sched_online_group(), which cannot be undone. If the group does
 * not come online after all, the caller undoes this function with
 * conc_tg_drop_ceiling().
 */
int conc_tg_take_ceiling(struct task_group *tg, struct task_group *parent)
{
	unsigned int ceiling = READ_ONCE(parent->conc.child_ceiling);
	int ret;

	if (!ceiling)
		return 0;
	/*
	 * A group with a ceiling holds one reference until conc_tg_offline().
	 */
	conc_key_get();
	scoped_guard(mutex, &conc_mutex)
		ret = conc_scx_active ? -EBUSY : conc_tg_state_alloc(tg);
	if (ret)
		conc_key_put();
	else
		tg->conc.ceiling = ceiling;
	return ret;
}

/*
 * Prepare the new group @tg before it comes online. Take its ceiling. Under a
 * counting parent the group counts from the start (conc_tg_online()). So
 * allocate its state here, where an allocation failure can still fail the
 * creation. The parent may start to count between here and conc_tg_online().
 * conc_tg_online() then allocates the state itself, under conc_mutex.
 */
int conc_tg_prepare(struct task_group *tg, struct task_group *parent)
{
	int ret;

	ret = conc_tg_take_ceiling(tg, parent);
	if (ret || !conc_active(&parent->conc))
		return ret;
	scoped_guard(mutex, &conc_mutex)
		ret = conc_tg_state_alloc(tg);
	if (ret)
		conc_tg_drop_ceiling(tg);
	return ret;
}

void conc_tg_drop_ceiling(struct task_group *tg)
{
	if (!tg->conc.ceiling)
		return;
	tg->conc.ceiling = 0;
	conc_key_put();
}

/*
 * Make the new group @tg count from the start if its parent counts, or if it
 * has a ceiling of its own. No task can join it before. Otherwise a group
 * created under a limit would count nothing. Its tasks would escape the limit
 * until the next write of a limit above it.
 *
 * This function cannot fail. It runs after sched_online_group() linked the
 * group, which cpu_cgroup_css_online() cannot undo. So the scratch mask is
 * static, protected by conc_mutex.
 */
void conc_tg_online(struct task_group *tg)
{
	static struct cpumask scratch;

	if (!static_branch_unlikely(&sched_conc_enabled) || !tg->parent)
		return;
	mutex_lock(&conc_mutex);
	if (tg->conc.ceiling || conc_active(&tg->parent->conc)) {
		/*
		 * The state was allocated in conc_tg_prepare(), unless the
		 * parent started to count after it.
		 */
		if (WARN_ONCE(conc_tg_state_alloc(tg),
			      "conc: no memory for css %d, which does not count\n", tg->css.id)) {
			mutex_unlock(&conc_mutex);
			return;
		}
		conc_update_tree(tg, &scratch);
		/*
		 * The group has no task yet, so there is no ID to wait for.
		 * The file shows the values at once.
		 */
		conc_bounds_apply(tg);
	}
	mutex_unlock(&conc_mutex);
}

/*
 * The slots which conc_tg_offline() gives back to the parent. Protected by
 * conc_mutex. The mask is static because a removal cannot fail on an
 * allocation.
 */
static struct cpumask conc_offline_held;

/* Handle the removal of group @tg. It has no task and no child. */
void conc_tg_offline(struct task_group *tg)
{
	struct tg_concurrency *conc = &tg->conc;
	struct cpumask *held = &conc_offline_held;
	unsigned long flags;
	bool had_request;
	int cpu;

	mutex_lock(&conc_mutex);
	had_request = conc->request || conc->ceiling;
	conc->request = 0;
	conc->ceiling = 0;
	raw_spin_lock_irqsave(&conc->lock, flags);
	if (conc->propagate)
		cpumask_copy(held, conc->used);
	else
		cpumask_clear(held);
	/* The group never counted. It has no slot to give back. */
	if (!conc->present) {
		raw_spin_unlock_irqrestore(&conc->lock, flags);
		goto done;
	}
	conc->propagate = false;
	WRITE_ONCE(conc->active, false);
	conc->effective = 0;
	conc->ceiling_effective = 0;
	conc->shrink_pending = false;
	for_each_cpu(cpu, conc->used)
		atomic_set(per_cpu_ptr(conc->present, cpu), CONC_SLOT_UNUSED);
	cpumask_clear(conc->used);
	cpumask_setall(conc->unused_cores);
	conc->nr_used = 0;
	raw_spin_unlock_irqrestore(&conc->lock, flags);
	/*
	 * Release the entities of the parent after conc->lock is dropped, see
	 * conc_claim_slow().
	 */
	for_each_cpu(cpu, held)
		conc_release(tg->parent, cpu);
done:
	mutex_unlock(&conc_mutex);

	irq_work_sync(&conc->irq_work);
	cancel_work_sync(&conc->work);
	if (had_request)
		conc_key_put();
}

/* Print "limit ceiling". A value of 0 is printed as "max". */
static void conc_show_pair(struct seq_file *sf, unsigned int limit, unsigned int ceiling)
{
	if (limit)
		seq_printf(sf, "%u ", limit);
	else
		seq_puts(sf, "max ");
	if (ceiling)
		seq_printf(sf, "%u\n", ceiling);
	else
		seq_puts(sf, "max\n");
}

static void conc_show_value(struct seq_file *sf, unsigned int val)
{
	if (val)
		seq_printf(sf, "%u\n", val);
	else
		seq_puts(sf, "max\n");
}

/* The limit requested: "max" or a positive integer. */
int cpu_concurrency_max_show(struct seq_file *sf, void *v)
{
	conc_show_value(sf, READ_ONCE(css_tg(seq_css(sf))->conc.request));
	return 0;
}

/*
 * Show the ceiling which the cgroup took at its creation: "max" or a positive
 * integer.
 */
int cpu_concurrency_ceiling_show(struct seq_file *sf, void *v)
{
	conc_show_value(sf, READ_ONCE(css_tg(seq_css(sf))->conc.ceiling));
	return 0;
}

/* One value: "max" or a positive integer. */
static int conc_parse_value(const char *tok, unsigned int *val)
{
	int ret;

	if (!strcmp(tok, "max")) {
		*val = 0;
		return 0;
	}
	ret = kstrtouint(tok, 10, val);
	if (ret)
		return ret;
	/*
	 * The value is at most the number of possible CPUs, because processes
	 * may size data by it.
	 */
	return *val && *val <= num_possible_cpus() ? 0 : -EINVAL;
}

ssize_t cpu_concurrency_max_write(struct kernfs_open_file *of,
				  char *buf, size_t nbytes, loff_t off)
{
	struct task_group *tg = css_tg(of_css(of));
	unsigned int limit;
	char *tok;
	int ret;

	buf = strstrip(buf);
	tok = strsep(&buf, " ");
	if (!tok || !*tok || (buf && *strstrip(buf)))
		return -EINVAL;
	ret = conc_parse_value(tok, &limit);
	if (ret)
		return ret;
	ret = conc_set_request(tg, limit);
	return ret ?: nbytes;
}

/*
 * Show the ceiling of the children created from now on: "max" or a positive
 * integer.
 */
int cpu_concurrency_child_ceiling_show(struct seq_file *sf, void *v)
{
	conc_show_value(sf, READ_ONCE(css_tg(seq_css(sf))->conc.child_ceiling));
	return 0;
}

ssize_t cpu_concurrency_child_ceiling_write(struct kernfs_open_file *of,
						    char *buf, size_t nbytes, loff_t off)
{
	unsigned int ceiling;
	int ret;

	ret = conc_parse_value(strstrip(buf), &ceiling);
	if (ret)
		return ret;
	/*
	 * conc_tg_take_ceiling() reads the value in one load. A child takes
	 * the old value or the new one.
	 */
	WRITE_ONCE(css_tg(of_css(of))->conc.child_ceiling, ceiling);
	return nbytes;
}

/*
 * cpu.concurrency.stat: the holds and the rotations of the subtree. See
 * conc_stat_hold_start() and conc_rotate_for().
 */
int cpu_concurrency_stat_show(struct seq_file *sf, void *v)
{
	struct tg_concurrency *conc = &css_tg(seq_css(sf))->conc;

	seq_printf(sf, "nr_held %d\n", atomic_read(&conc->st_nr_held));
	seq_printf(sf, "nr_holds %ld\n", atomic_long_read(&conc->st_nr_holds));
	seq_printf(sf, "held_usec %llu\n",
		   div_u64(atomic64_read(&conc->st_held_ns), NSEC_PER_USEC));
	seq_printf(sf, "nr_rotations %ld\n", atomic_long_read(&conc->st_nr_rotations));
	seq_printf(sf, "rotated_usec %llu\n",
		   div_u64(atomic64_read(&conc->st_rotated_ns), NSEC_PER_USEC));
	return 0;
}

int cpu_concurrency_events_show(struct seq_file *sf, void *v)
{
	struct tg_concurrency *conc = &css_tg(seq_css(sf))->conc;

	seq_printf(sf, "rotation %ld\n", atomic_long_read(&conc->st_nr_rotations));
	return 0;
}

int cpu_concurrency_effective_show(struct seq_file *sf, void *v)
{
	struct task_group *tg = css_tg(seq_css(sf));

	conc_show_pair(sf, conc_effective_shown(tg), READ_ONCE(tg->conc.ceiling_effective));
	return 0;
}

/*
 * Check a migration in the can_attach() of the cpu controller. Return 0 or
 * -EBUSY.
 *
 * Refuse a deadline task bound for a counting group. Only a change of group
 * can bring a deadline task under a limit.
 *
 * The policy is read under p->pi_lock. conc_move_seq of each checked task is
 * odd until the migration ends (conc_attach_finish()). A change of policy
 * waits for an even count (conc_dl_admit()).
 *
 * Count an accepted migration as pending, see conc_wait_attach().
 */
static int __conc_attach_check(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct task_struct *task, *refused = NULL;

	guard(mutex)(&conc_mutex);
	/*
	 * The static key went off since conc_attach_check() read it. No group
	 * counts any more. Only count the migration as pending. Do it under
	 * conc_mutex, where a limit which starts reads the count.
	 */
	if (!static_branch_unlikely(&sched_conc_enabled)) {
		conc_attach_begin();
		return 0;
	}
	cgroup_taskset_for_each(task, css, tset) {
		struct task_group *tg = css_tg(css);

		guard(raw_spinlock_irq)(&task->pi_lock);
		if (READ_ONCE(tg->conc.active) && conc_dl_task(task)) {
			trace_sched_conc_refuse(task, "cgroup migration", "a SCHED_DEADLINE task",
					      -EBUSY);
			refused = task;
			break;
		}
		WRITE_ONCE(task->conc_move_seq, task->conc_move_seq + 1);
	}
	if (!refused) {
		conc_attach_begin();
		return 0;
	}
	/*
	 * The migration is refused. cgroup core calls neither attach() nor
	 * cancel_attach() for it. Make conc_move_seq of the tasks checked
	 * before the refused one even again here.
	 */
	cgroup_taskset_for_each(task, css, tset) {
		if (task == refused)
			break;
		scoped_guard(raw_spinlock_irq, &task->pi_lock)
			WRITE_ONCE(task->conc_move_seq, task->conc_move_seq + 1);
	}
	wake_up(&conc_attach_wq);
	return -EBUSY;
}

/*
 * Check a migration in the can_attach() of the cpu controller. Return 0 if it
 * is accepted, or a negative error code.
 */
int conc_attach_check(struct cgroup_taskset *tset)
{
	int ret;

	/*
	 * While no group counts, only count the migration as pending, with no
	 * lock. This side increments the count, then reads the static key. A
	 * limit which starts turns the key on, then reads the count. Each
	 * side has a full barrier between its two steps. So the limit waits
	 * for this migration, or this migration sees the key on and is
	 * checked below.
	 */
	atomic_inc(&conc_attach_pending);
	/* Pairs with the smp_mb() of conc_set_request(). */
	smp_mb__after_atomic();
	if (!static_key_enabled(&sched_conc_enabled)) {
		current->conc_attaching = 1;
		return 0;
	}
	/*
	 * A group counts. Drop the count taken above. The check below runs
	 * under the locks and counts the migration again.
	 */
	if (atomic_dec_and_test(&conc_attach_pending))
		wake_up(&conc_attach_wq);
	ret = __conc_attach_check(tset);
	return ret;
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
 * Hand the CPU-owned IDs of the collected domains from src to dst. Called
 * from the set-move stopper under both runqueue locks.
 *
 * The domains were collected before the stopper, and a cgroup migration can
 * run in between. A domain may then have no user left in the moved subtree.
 * Nothing of it comes to dst. Drop its ID on src, unless its group uses src
 * itself.
 */
static void conc_sm_transfer_cids(struct conc_set_move_arg *arg)
{
	struct task_group *top = arg->groups[0].tg;
	int i;

	for (i = 0; i < arg->nr_doms; i++) {
		struct cid_domain *dom = arg->doms[i];
		struct task_group *g = conc_dom_group(dom);

		if (g && !conc_tg_is_ancestor(g, top) && !conc_tg_is_ancestor(top, g)) {
			if (!cpumask_test_cpu(arg->src, g->conc.used))
				cid_drop_on_cpu(dom, per_cpu_ptr(dom->pcpu, arg->src));
			continue;
		}
		conc_transfer_cid(dom, arg->src, arg->dst);
	}
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
