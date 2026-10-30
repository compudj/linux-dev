/* SPDX-License-Identifier: GPL-2.0 */

/*
 * The concurrency limit's hooks, inline for the scheduler's fast paths, and
 * the prototypes of concurrency.c; see concurrency.c.
 */

#ifndef _KERNEL_SCHED_CONCURRENCY_H
#define _KERNEL_SCHED_CONCURRENCY_H
/* Included by sched.h, after the runqueue and task group definitions. */

#ifdef CONFIG_SCHED_MAX_CONCURRENCY
/* Filled in by a later patch. */
static inline unsigned int conc_ns_clamp(struct cid_domain *dom, unsigned int bound)
{
	return bound;
}

/* Filled in by a later patch. */
static inline int conc_pin_check(struct cgroup_taskset *tset)
{
	return 0;
}

/* Filled in by a later patch. */
static inline int conc_pin_fork_check(struct task_struct *p, struct kernel_clone_args *kargs)
{
	return 0;
}

/* Filled in by a later patch. */
static inline bool conc_task_ns_allows(struct task_struct *t, struct task_group *tg)
{
	return false;
}

/*
 * Concurrency accounting primitives, see
 * Documentation/scheduler/sched-max-concurrency.rst.
 *
 * All of them are called with p->pi_lock or task_rq(p)->lock held, which
 * keeps task_group(p) and p->conc_accounted stable.
 */
DECLARE_STATIC_KEY_FALSE(sched_conc_enabled);

extern enum conc_claim_result conc_claim_slow(struct task_group *tg, int cpu, bool force,
					  struct task_struct *p);

/*
 * While a class selects a CPU for a wakeup, fork or exec, the CPUs the
 * concurrency limit lets the task use; NULL when unconstrained. Set by
 * conc_select_prepare() with interrupts disabled on the selecting CPU.
 */
DECLARE_PER_CPU(const struct cpumask *, conc_select_cpus);

static inline const struct cpumask *task_select_cpus(struct task_struct *p)
{
	const struct cpumask *cpus;

	if (!static_branch_unlikely(&sched_conc_enabled))
		return p->cpus_ptr;
	cpus = __this_cpu_read(conc_select_cpus);
	return cpus ?: p->cpus_ptr;
}

extern void __conc_select_prepare(struct task_struct *p);

static inline void conc_select_prepare(struct task_struct *p)
{
	if (static_branch_unlikely(&sched_conc_enabled))
		__conc_select_prepare(p);
}

/* Behind the key too: the pointers are reset when the key comes on (conc_key_get()). */
static inline void conc_select_done(void)
{
	if (static_branch_unlikely(&sched_conc_enabled))
		__this_cpu_write(conc_select_cpus, NULL);
}

/* True when @p's group takes part in concurrency accounting. */
static inline bool conc_task_active(struct task_struct *p)
{
	return static_branch_unlikely(&sched_conc_enabled) &&
	       READ_ONCE(task_group(p)->conc.active);
}

/*
 * Add an entity to the (@tg, @cpu) slot. The lockless fast path succeeds
 * iff the slot exists (its value is non-negative). @p is the task behind
 * the claim, NULL for a child group's entity; it fills a slot reserved
 * for its arrival. @force is for entities only: a task is never claimed
 * for beyond the limit, it is held instead, see conc_hold_request().
 */
static inline enum conc_claim_result conc_claim(struct task_group *tg, int cpu, bool force,
					    struct task_struct *p)
{
	if (atomic_inc_unless_negative(per_cpu_ptr(tg->conc.present, cpu)))
		return CONC_CLAIMED;
	return conc_claim_slow(tg, cpu, force, p);
}

extern atomic_t conc_nr_held;
extern void conc_hold_kick(void);
extern void conc_held_blocks(struct rq *rq, struct task_struct *p);
extern void conc_unhold(struct rq *rq, struct task_struct *p, bool rehold);

/*
 * Held off the CPUs, out of its class, see conc_hold(). Under
 * task_rq(p)->lock. Behind the key: no task stays held once the key is off,
 * the last reference waits for the held tasks to be placed (conc_key_put()).
 */
static inline bool conc_task_held(struct task_struct *p)
{
	return static_branch_unlikely(&sched_conc_enabled) && unlikely(p->conc_held);
}

/*
 * About to be held, see conc_hold_request(): it takes no concurrency ID. A
 * request the key went off on is cleared at the task's next return to
 * user space, and means nothing until then.
 */
static __always_inline bool conc_task_unplaced(struct task_struct *p)
{
	return static_branch_unlikely(&sched_conc_enabled) && unlikely(READ_ONCE(p->conc_hold_pending));
}

/*
 * Remove an entity. Sticky: the slot stays in the used set at zero. A
 * slot that empties may let a held task in, see conc_hold_work_fn().
 */
static inline void conc_release(struct task_group *tg, int cpu)
{
	int v = atomic_dec_return(per_cpu_ptr(tg->conc.present, cpu));

#ifdef CONFIG_SCHED_MAX_CONCURRENCY_DEBUG
	/*
	 * Only an entity leaves: a slot that holds none, or none at all, was
	 * claimed and then overwritten, see conc_remove_reserved().
	 */
	WARN_ONCE(READ_ONCE(tg->conc.active) &&
		  (v + 1 == 0 || v + 1 == CONC_SLOT_UNUSED || v + 1 == CONC_SLOT_DRAINING ||
		   v + 1 == -CONC_SLOT_RESERVE_BIAS || v + 1 == -CONC_SLOT_EVICT_BIAS),
		  "conc: release on CPU %d from a slot without entity (%d)\n", cpu, v + 1);
#endif
	if (v == 0 && unlikely(atomic_read(&conc_nr_held)))
		conc_hold_kick();
}

/* Task level: claim a slot on @cpu for @p. Returns false only when refused. */
static inline bool conc_task_claim(struct task_struct *p, int cpu)
{
	enum conc_claim_result ret;

	if (!conc_task_active(p))
		return true;
	if (WARN_ON_ONCE(p->conc_accounted))
		return true;
	ret = conc_claim(task_group(p), cpu, false, p);
	if (ret == CONC_REFUSED)
		return false;
	if (ret == CONC_CLAIMED)
		p->conc_accounted = 1;
	return true;
}

/*
 * Release @p's slot on @cpu, if it holds one. Idempotent. Behind the key: a
 * task is accounted only while its group counts (conc_deactivate_tasks()),
 * so none is once the key is off.
 */
static inline void conc_task_release(struct task_struct *p, int cpu)
{
	if (!static_branch_unlikely(&sched_conc_enabled) || !p->conc_accounted)
		return;
	p->conc_accounted = 0;
	conc_release(task_group(p), cpu);
}

/* Why a task is to be held at its next return to user space. */
#define CONC_HOLD_UNPLACED	1	/* it runs without a slot */
#define CONC_HOLD_EVICT		2	/* its slot is being emptied */
extern bool conc_hold_request(struct task_struct *p, u8 why);
extern void conc_hold_task_work(struct callback_head *work);

/*
 * Move @p's slot from @src to @dst. Returns false when the limit refuses
 * @dst, in which case nothing changed and the caller must not migrate,
 * unless the task @must leave @src (an affinity change, hot-unplug): it
 * then goes without a slot, to be held at its next return to user space.
 */
static inline bool conc_task_move(struct task_struct *p, int src, int dst, bool must)
{
	enum conc_claim_result ret;

	if (!conc_task_active(p)) {
		/* Stale accounting from before the group went inactive. */
		conc_task_release(p, src);
		return true;
	}
	/* Held: it holds no slot and moves as it is. */
	if (conc_task_held(p))
		return true;
	ret = conc_claim(task_group(p), dst, false, p);
	if (ret == CONC_REFUSED) {
		if (!must)
			return false;
		conc_task_release(p, src);
		conc_hold_request(p, CONC_HOLD_UNPLACED);
		return true;
	}
	conc_task_release(p, src);
	if (ret == CONC_CLAIMED)
		p->conc_accounted = 1;
	return true;
}

extern void __init conc_init_root(void);
extern int conc_select_cpu(struct task_struct *p, int cpu);
extern void conc_task_change_group(struct task_struct *p, struct task_group *old_tg);
extern bool conc_can_move(struct task_struct *p, int cpu);
extern bool conc_constrain_mask(struct task_struct *p, struct cpumask *mask);
extern int conc_pick_dest(struct task_struct *p, const struct cpumask *valid,
			const struct cpumask *allowed, struct affinity_context *ctx);
extern void conc_task_affinity_changed(struct task_struct *p, struct affinity_context *ctx,
				     int ret);
extern void conc_prepare_affinity(struct task_struct *p, struct affinity_context *ctx);
extern void conc_task_mask_changed(struct task_struct *p, struct affinity_context *ctx);
extern void conc_task_fork(struct task_struct *p);
struct kernel_clone_args;
extern int conc_fork_check(struct task_struct *p, struct kernel_clone_args *kargs);
extern void conc_fork_done(struct task_struct *p);
extern int conc_check_affinity(struct task_struct *p, const struct cpumask *mask);
extern bool conc_affinity_lock(struct task_struct *p, struct affinity_context *ctx);
extern void conc_affinity_unlock(struct task_struct *p, bool locked);

/*
 * A migration of @p judged its mask after @ctx's change was judged, see
 * conc_affinity_lock(): the change must be judged again. Under p->pi_lock,
 * or once it was true, as the count only grows.
 */
extern unsigned int conc_limit_gen;

static inline bool conc_judged_stale(struct task_struct *p, struct affinity_context *ctx)
{
	return (ctx->flags & SCA_CC_JUDGED) &&
	       (READ_ONCE(p->conc_move_seq) != ctx->conc.move_seq || READ_ONCE(conc_limit_gen) != ctx->conc.gen);
}

extern int conc_tg_prepare(struct task_group *tg, struct task_group *parent);
extern int conc_scx_enable(void);
extern void conc_scx_disable(void);
extern void conc_cpu_deactivate(unsigned int cpu);
extern void conc_cpu_activate(unsigned int cpu);
extern void conc_cpu_offline(unsigned int cpu);

/* Called from outside concurrency.c. */
int alloc_conc_sched_group(struct task_group *tg);

void free_conc_sched_group(struct task_group *tg);

void conc_task_released(struct task_struct *p);

void conc_attach_record_srcs(struct cgroup_taskset *tset);

void conc_attach_finish(struct cgroup_taskset *tset, bool retry);

int conc_tg_take_ceiling(struct task_group *tg, struct task_group *parent);

void conc_tg_drop_ceiling(struct task_group *tg);

void conc_tg_online(struct task_group *tg);

void conc_tg_offline(struct task_group *tg);

int cpu_concurrency_max_show(struct seq_file *sf, void *v);
int cpu_concurrency_ceiling_show(struct seq_file *sf, void *v);

ssize_t cpu_concurrency_max_write(struct kernfs_open_file *of,
					 char *buf, size_t nbytes, loff_t off);

int cpu_concurrency_child_ceiling_show(struct seq_file *sf, void *v);

ssize_t cpu_concurrency_child_ceiling_write(struct kernfs_open_file *of,
						    char *buf, size_t nbytes, loff_t off);

int cpu_concurrency_effective_show(struct seq_file *sf, void *v);

int conc_attach_check(struct cgroup_taskset *tset);

#ifdef CONFIG_SCHED_MM_CID
unsigned int conc_dom_bound(struct cid_domain *dom);

void conc_dom_grow_tops(struct cid_domain *dom);

void conc_dom_init(struct cid_domain *dom, struct task_struct *p);

void conc_dom_fork(struct cid_domain *dom, struct task_struct *t);

void conc_dom_user_exited(struct cid_domain *dom, struct task_struct *t);

void conc_mm_task_moved(struct task_struct *p);

void conc_dom_drop_stale_cids(struct cid_domain *dom);

void conc_bounds_lock(void);

void conc_bounds_unlock(void);

/* conc_mutex, for the namespace admissions of cid.c. */
DEFINE_LOCK_GUARD_0(conc_bounds, conc_bounds_lock(), conc_bounds_unlock())
#endif /* CONFIG_SCHED_MM_CID */
#endif /* CONFIG_SCHED_MAX_CONCURRENCY */

#ifndef CONFIG_SCHED_MAX_CONCURRENCY
static inline const struct cpumask *task_select_cpus(struct task_struct *p)
{
	return p->cpus_ptr;
}

static inline void conc_select_prepare(struct task_struct *p) { }

static inline void conc_select_done(void) { }

static inline void conc_init_root(void) { }

static inline bool conc_task_active(struct task_struct *p) { return false; }

static inline bool conc_task_claim(struct task_struct *p, int cpu) { return true; }

static inline void conc_task_release(struct task_struct *p, int cpu) { }

static inline bool conc_task_move(struct task_struct *p, int src, int dst, bool must) { return true; }

static inline bool conc_task_held(struct task_struct *p) { return false; }

static inline void conc_held_blocks(struct rq *rq, struct task_struct *p) { }

static inline void conc_unhold(struct rq *rq, struct task_struct *p, bool rehold) { }

static __always_inline bool conc_task_unplaced(struct task_struct *p) { return false; }

static inline int conc_select_cpu(struct task_struct *p, int cpu) { return cpu; }

static inline void conc_task_change_group(struct task_struct *p, struct task_group *old_tg) { }

static inline bool conc_can_move(struct task_struct *p, int cpu) { return true; }

static inline bool conc_constrain_mask(struct task_struct *p, struct cpumask *mask) { return true; }

static inline int conc_pick_dest(struct task_struct *p, const struct cpumask *valid,
			       const struct cpumask *allowed, struct affinity_context *ctx)
{
	return cpumask_any_and_distribute(valid, allowed);
}

static inline void conc_task_affinity_changed(struct task_struct *p, struct affinity_context *ctx,
					    int ret) { }

static inline void conc_prepare_affinity(struct task_struct *p, struct affinity_context *ctx) { }

static inline void conc_task_mask_changed(struct task_struct *p, struct affinity_context *ctx) { }

static inline void conc_task_fork(struct task_struct *p) { }

struct kernel_clone_args;

static inline int conc_fork_check(struct task_struct *p, struct kernel_clone_args *kargs)
{
	return 0;
}

static inline void conc_fork_done(struct task_struct *p) { }

static inline int conc_check_affinity(struct task_struct *p, const struct cpumask *mask)
{
	return 0;
}

static inline bool conc_affinity_lock(struct task_struct *p, struct affinity_context *ctx)
{
	return false;
}

static inline void conc_affinity_unlock(struct task_struct *p, bool locked) { }

static inline bool conc_judged_stale(struct task_struct *p, struct affinity_context *ctx)
{
	return false;
}

static inline int conc_tg_prepare(struct task_group *tg, struct task_group *parent) { return 0; }

static inline int conc_scx_enable(void) { return 0; }

static inline void conc_scx_disable(void) { }

static inline void conc_cpu_deactivate(unsigned int cpu) { }

static inline void conc_cpu_activate(unsigned int cpu) { }

static inline void conc_cpu_offline(unsigned int cpu) { }

static inline int alloc_conc_sched_group(struct task_group *tg) { return 1; }

static inline void free_conc_sched_group(struct task_group *tg) { }

static inline int conc_tg_take_ceiling(struct task_group *tg, struct task_group *parent) { return 0; }

static inline void conc_tg_drop_ceiling(struct task_group *tg) { }

static inline void conc_tg_online(struct task_group *tg) { }

static inline void conc_tg_offline(struct task_group *tg) { }

static inline void conc_mm_task_moved(struct task_struct *p) { }

static inline void conc_attach_record_srcs(struct cgroup_taskset *tset) { }

static inline void conc_attach_finish(struct cgroup_taskset *tset, bool retry) { }

static inline int conc_attach_check(struct cgroup_taskset *tset) { return 0; }

#ifdef CONFIG_SCHED_MM_CID
static inline void conc_bounds_lock(void) { }

static inline void conc_bounds_unlock(void) { }

static inline unsigned int conc_dom_bound(struct cid_domain *dom) { return 0; }

static inline void conc_dom_grow_tops(struct cid_domain *dom) { }

static inline void conc_dom_init(struct cid_domain *dom, struct task_struct *p) { }

static inline void conc_dom_fork(struct cid_domain *dom, struct task_struct *t) { }

static inline void conc_dom_user_exited(struct cid_domain *dom, struct task_struct *t) { }

static inline void conc_dom_drop_stale_cids(struct cid_domain *dom) { }
#endif /* CONFIG_SCHED_MM_CID */
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY */

#ifndef CONFIG_SCHED_MAX_CONCURRENCY_DEBUG
static inline void conc_debug_switch(struct rq *rq, struct task_struct *prev,
				   struct task_struct *next) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY_DEBUG */

/* core.c's, for concurrency.c. */
struct rq *__migrate_task(struct rq *rq, struct rq_flags *rf,
				 struct task_struct *p, int dest_cpu, bool must)
	__must_hold(__rq_lockp(rq));

int migration_cpu_stop(void *data);

void
do_set_cpus_allowed(struct task_struct *p, struct affinity_context *ctx);

int select_fallback_rq(int cpu, struct task_struct *p);

int select_task_rq(struct task_struct *p, int cpu, int *wake_flags);
#endif /* !_KERNEL_SCHED_CONCURRENCY_H */
