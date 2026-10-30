/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Inline hooks of the concurrency limit for the fast paths of the
 * scheduler, and the prototypes of concurrency.c. See concurrency.c.
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
static inline bool conc_task_held(struct task_struct *p)
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
 * Return true if the group of @conc counts. This is an acquire. The state
 * of a group is set up before @active is set with a release. So a path
 * that sees @active set without the group's lock can use that state, the
 * per-CPU slots first.
 */
static inline bool conc_active(struct tg_concurrency *conc)
{
	/* Pairs with the release that sets @active. */
	return smp_load_acquire(&conc->active);
}

/* True when @p's group takes part in concurrency accounting. */
static inline bool conc_task_active(struct task_struct *p)
{
	return static_branch_unlikely(&sched_conc_enabled) && conc_active(&task_group(p)->conc);
}

/*
 * Add an entity to the (@tg, @cpu) slot. The lockless fast path succeeds
 * iff the slot exists (its value is non-negative). @p is the task behind
 * the claim. It is NULL for a child group's entity. @p may fill a slot
 * reserved for it. @force adds the entity even where the limit refuses
 * it. Only a child group's entity is forced. A task is never added beyond
 * the limit: it is held instead, see conc_hold_request().
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
 * Remove an entity. The slot stays in the used set, at zero. A slot that
 * empties may let a held task in.
 */
static inline void conc_release(struct task_group *tg, int cpu)
{
	int v = atomic_dec_return(per_cpu_ptr(tg->conc.present, cpu));

#ifdef CONFIG_SCHED_MAX_CONCURRENCY_DEBUG
	/*
	 * A release needs an entity in the slot. Warn when the slot was empty
	 * or did not exist. The slot was then overwritten after the claim.
	 */
	WARN_ONCE(conc_active(&tg->conc) &&
		  (v + 1 == 0 || v + 1 == CONC_SLOT_UNUSED || v + 1 == CONC_SLOT_DRAINING ||
		   v + 1 == -CONC_SLOT_RESERVE_BIAS || v + 1 == -CONC_SLOT_EVICT_BIAS),
		  "conc: release on CPU %d from a slot without entity (%d)\n", cpu, v + 1);
#endif
	if (v == 0 && unlikely(atomic_read(&conc_nr_held)))
		conc_hold_kick();
}

/* Claim a slot on @cpu for @p. Return false only when the limit refuses. */
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
 * Release the slot of @p on @cpu, if it holds one. A second call does
 * nothing. The static key test is safe: a task is accounted only while its
 * group counts, so no task is accounted once the key is off.
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
 * Move the slot of @p from @src to @dst. Return false when the limit
 * refuses @dst. Nothing changed then, and the caller must not migrate @p.
 * With @must set, @p has to leave @src (an affinity change, a CPU going
 * offline). It then moves without a slot and is held at its next return
 * to user space.
 */
static inline bool conc_task_move(struct task_struct *p, int src, int dst, bool must)
{
	enum conc_claim_result ret;

	if (!conc_task_active(p)) {
		/* Stale accounting from before the group went inactive. */
		conc_task_release(p, src);
		return true;
	}
	/* A held task holds no slot. It moves as it is. */
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

/*
 * The three steps of a move of the slot of @p which can be undone, for a
 * swap of two tasks, see conc_swap_slots().
 *
 * conc_task_claim_dst() claims a slot on @dst and keeps the slot of the
 * source. Return a negative value if the limit refuses, 1 if a slot was
 * claimed, 0 if @p needs none. conc_task_unclaim_dst() gives that slot
 * back. conc_task_commit_move() releases the slot of @src.
 */
static inline int conc_task_claim_dst(struct task_struct *p, int dst)
{
	if (!conc_task_active(p))
		return 0;
	switch (conc_claim(task_group(p), dst, false, p)) {
	case CONC_REFUSED:
		return -EBUSY;
	case CONC_CLAIMED:
		return 1;
	default:
		return 0;
	}
}

static inline void conc_task_unclaim_dst(struct task_struct *p, int dst, int claimed)
{
	if (claimed > 0)
		conc_release(task_group(p), dst);
}

static inline void conc_task_commit_move(struct task_struct *p, int src, int claimed)
{
	conc_task_release(p, src);
	if (claimed > 0)
		p->conc_accounted = 1;
}

extern void __init conc_init_root(void);

/* Called from outside concurrency.c. */
int alloc_conc_sched_group(struct task_group *tg);

void free_conc_sched_group(struct task_group *tg);
#else /* !CONFIG_SCHED_MAX_CONCURRENCY */

static inline void conc_init_root(void) { }

static inline bool conc_task_active(struct task_struct *p) { return false; }

static inline bool conc_task_claim(struct task_struct *p, int cpu) { return true; }

static inline void conc_task_release(struct task_struct *p, int cpu) { }

static inline bool conc_task_move(struct task_struct *p, int src, int dst, bool must)
{
	return true;
}

static inline int conc_task_claim_dst(struct task_struct *p, int dst) { return 0; }

static inline void conc_task_unclaim_dst(struct task_struct *p, int dst, int claimed) { }

static inline void conc_task_commit_move(struct task_struct *p, int src, int claimed) { }

static inline int alloc_conc_sched_group(struct task_group *tg) { return 1; }

static inline void free_conc_sched_group(struct task_group *tg) { }

#endif /* !CONFIG_SCHED_MAX_CONCURRENCY */

#ifndef CONFIG_SCHED_MAX_CONCURRENCY_DEBUG
static inline void conc_debug_switch(struct rq *rq, struct task_struct *prev,
				   struct task_struct *next) { }
#endif /* !CONFIG_SCHED_MAX_CONCURRENCY_DEBUG */

/* Functions of core.c that concurrency.c uses. */
struct rq *__migrate_task(struct rq *rq, struct rq_flags *rf,
				 struct task_struct *p, int dest_cpu, bool must)
	__must_hold(__rq_lockp(rq));

int migration_cpu_stop(void *data);

void
do_set_cpus_allowed(struct task_struct *p, struct affinity_context *ctx);

int select_fallback_rq(int cpu, struct task_struct *p);

int select_task_rq(struct task_struct *p, int cpu, int *wake_flags);
#endif /* !_KERNEL_SCHED_CONCURRENCY_H */
