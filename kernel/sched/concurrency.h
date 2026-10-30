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
static inline void conc_bounds_apply(struct task_group *top)
{
}

/* Filled in by a later patch. */
static inline void conc_classes_drop(struct tg_concurrency *conc)
{
}

/* Filled in by a later patch. */
static inline void conc_hold_kick(void)
{
}

/* Filled in by a later patch. */
static inline bool conc_hold_request(struct task_struct *p, u8 why)
{
	return false;
}

/* Filled in by a later patch. */
static inline void conc_notify_tree(struct task_group *top)
{
}

/* Filled in by a later patch. */
static inline void conc_report_drop(struct tg_concurrency *conc, unsigned int from)
{
}

/* Filled in by a later patch. */
static inline void conc_slot_removed(struct task_group *tg, int cpu)
{
}

/* Filled in by a later patch. */
static inline bool conc_slot_sole_hitter(struct tg_concurrency *conc, int cpu)
{
	return false;
}

/* Filled in by a later patch. */
static inline bool conc_task_held(struct task_struct *p)
{
	return false;
}

/* Filled in by a later patch. */
static inline unsigned int conc_unhit_hits(struct tg_concurrency *conc, int cpu)
{
	return 0;
}

/* Filled in by a later patch. */
static inline void conc_unhit_slot_added(struct tg_concurrency *conc, int cpu)
{
}

/* Filled in by a later patch. */
static inline void conc_unhit_slot_removed(struct tg_concurrency *conc, int cpu)
{
}

/* Filled in by a later patch. */
static inline void conc_want_headroom(struct tg_concurrency *conc)
{
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
struct kernel_clone_args;

/* Called from outside concurrency.c. */
int alloc_conc_sched_group(struct task_group *tg);

void free_conc_sched_group(struct task_group *tg);
#else /* !CONFIG_SCHED_MAX_CONCURRENCY */
static inline void conc_init_root(void) { }

static inline bool conc_task_active(struct task_struct *p) { return false; }

static inline bool conc_task_claim(struct task_struct *p, int cpu) { return true; }

static inline void conc_task_release(struct task_struct *p, int cpu) { }

static inline bool conc_task_move(struct task_struct *p, int src, int dst, bool must) { return true; }

struct kernel_clone_args;

static inline int alloc_conc_sched_group(struct task_group *tg) { return 1; }

static inline void free_conc_sched_group(struct task_group *tg) { }
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
