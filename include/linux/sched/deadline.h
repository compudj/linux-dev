/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_DEADLINE_H
#define _LINUX_SCHED_DEADLINE_H

/*
 * SCHED_DEADLINE tasks has negative priorities, reflecting
 * the fact that any of them has higher prio than RT and
 * NORMAL/BATCH tasks.
 */

#include <linux/sched.h>

static inline bool dl_prio(int prio)
{
	return unlikely(prio < MAX_DL_PRIO);
}

/*
 * Returns true if a task has a priority that belongs to DL class. PI-boosted
 * tasks will return true. Use dl_policy() to ignore PI-boosted tasks.
 */
static inline bool dl_task(struct task_struct *p)
{
	return dl_prio(p->prio);
}

static inline bool dl_time_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

struct root_domain;
extern void dl_add_task_root_domain(struct task_struct *p);
extern void dl_clear_root_domain(struct root_domain *rd);
extern void dl_clear_root_domains(void);
extern void dl_rebuild_rd_accounting(void);
/*
 * Keep a rebuild of the root domains' accounting and an admission of
 * bandwidth from running at the same time. With cpusets, cpuset_mutex
 * does it, since every rebuild comes from the cpuset code. Without
 * cpusets, dl_accounting_lock() takes a mutex of its own.
 */
#ifdef CONFIG_CPUSETS
static inline void dl_accounting_lock(void) { }
static inline void dl_accounting_unlock(void) { }
static inline void lockdep_assert_dl_accounting_held(void) { }
#else
extern void dl_accounting_lock(void);
extern void dl_accounting_unlock(void);
extern void lockdep_assert_dl_accounting_held(void);
#endif
/*
 * Return the CPU that a move of DL task @p to the CPUs of @new_mask
 * records with its DL bandwidth. Return -1 if the bandwidth stays in the
 * root domain that counts it. Like the move, keep the CPUs within the
 * affinity set by the user. cpuset_can_attach() reserves the bandwidth in
 * the root domain of that CPU. cpuset_attach_task() then moves it there,
 * and set_cpus_allowed_dl() makes the same test.
 */
extern int dl_task_bw_move_cpu(struct task_struct *p,
			       const struct cpumask *new_mask);

static inline bool dl_server(struct sched_dl_entity *dl_se)
{
	return dl_se->dl_server;
}

static inline struct task_struct *dl_task_of(struct sched_dl_entity *dl_se)
{
	BUG_ON(dl_server(dl_se));
	return container_of(dl_se, struct task_struct, dl);
}

/*
 * Regarding the deadline, a task with implicit deadline has a relative
 * deadline == relative period. A task with constrained deadline has a
 * relative deadline <= relative period.
 *
 * We support constrained deadline tasks. However, there are some restrictions
 * applied only for tasks which do not have an implicit deadline. See
 * update_dl_entity() to know more about such restrictions.
 *
 * The dl_is_implicit() returns true if the task has an implicit deadline.
 */
static inline bool dl_is_implicit(struct sched_dl_entity *dl_se)
{
	return dl_se->dl_deadline == dl_se->dl_period;
}

#endif /* _LINUX_SCHED_DEADLINE_H */
