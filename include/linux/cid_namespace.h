/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CID_NAMESPACE_H
#define _LINUX_CID_NAMESPACE_H
/*
 * The concurrency ID namespace. Its processes share one space of
 * concurrency IDs, which rseq(2) reports in ns_cid. The IDs are below a
 * maximum which is fixed when the namespace is created. See
 * Documentation/scheduler/sched-max-concurrency.rst.
 */
#include <linux/sched.h>
#include <linux/nsproxy.h>
#include <linux/ns_common.h>
#include <linux/rseq_types.h>
#include <linux/cpumask.h>
#include <linux/err.h>

struct task_group;
struct user_namespace;
extern struct user_namespace init_user_ns;

/*
 * @home: the cgroup the namespace lives in. It is the nearest cgroup with a
 *	 limit, at or above the cgroup the namespace is created in, see
 *	 conc_ns_home(). The home keeps the namespace on a list (@home_node).
 *	 A member cannot leave the subtree of the home. NULL when no limit
 *	 and no ceiling applied at the creation. It is decided once (@home_fixed) and never
 *	 changes. The home cannot be removed while the namespace exists.
 * @max: the exclusive upper bound of the IDs. It is taken at the creation,
 *	 from the effective ceiling of the nearest cgroup with a ceiling at
 *	 or above the cgroup the namespace is created in. It is the number of
 *	 possible CPUs when there is none. It never changes afterwards
 *	 (@max_fixed). The initial namespace reports nr_cpu_ids instead,
 *	 because its ID is the CPU number.
 * @soft: the bound the IDs are kept below now. It is the effective limit in
 *	 force at @home, capped by @max. conc_ns_feed() updates it.
 * @dom: the domain which allocates the IDs. The initial namespace does not
 *	 use it.
 */
struct cid_namespace {
	struct user_namespace	*user_ns;
	struct ucounts		*ucounts;
	struct ns_common	ns;
	struct cid_domain	dom;
	struct task_group	*home;
	struct list_head	home_node;
	unsigned int		max;
	unsigned int		soft;
	bool			max_fixed;
	bool			home_fixed;
} __randomize_layout;

extern struct cid_namespace init_cid_ns;

#ifdef CONFIG_CID_NS
static inline struct cid_namespace *to_cid_ns(struct ns_common *ns)
{
	return container_of(ns, struct cid_namespace, ns);
}

void __init cid_ns_init(void);

static inline struct cid_namespace *get_cid_ns(struct cid_namespace *ns)
{
	ns_ref_inc(ns);
	return ns;
}

struct cid_namespace *copy_cid_ns(u64 flags, struct user_namespace *user_ns,
				  struct cid_namespace *old_ns,
				  struct task_struct *tsk);
void free_cid_ns(struct cid_namespace *ns);
u64 cid_ns_home_id(struct ns_common *ns);
void cidns_on_fork(struct nsproxy *nsproxy, struct task_struct *tsk);

static inline void put_cid_ns(struct cid_namespace *ns)
{
	if (ns_ref_put(ns))
		free_cid_ns(ns);
}

/* Return the exclusive upper bound of the IDs of @ns. */
static inline unsigned int cid_ns_max(struct cid_namespace *ns)
{
	return ns == &init_cid_ns ? nr_cpu_ids : ns->max;
}

static inline int cid_ns_get_max_prctl(void)
{
	return cid_ns_max(current->nsproxy->cid_ns);
}

/* The membership and the domain, in kernel/sched/cid.c. */
int cid_ns_domain_init(struct cid_namespace *ns);
void cid_ns_domain_exit(struct cid_namespace *ns);
void sched_cid_switch_ns(struct task_struct *t, struct nsproxy *new);
#else /* !CONFIG_CID_NS */
static inline void __init cid_ns_init(void) { }

static inline struct cid_namespace *get_cid_ns(struct cid_namespace *ns)
{
	return NULL;
}

static inline void put_cid_ns(struct cid_namespace *ns) { }

static inline struct cid_namespace *copy_cid_ns(u64 flags, struct user_namespace *user_ns,
						struct cid_namespace *old_ns,
						struct task_struct *tsk)
{
	if (flags & CLONE_NEWCID)
		return ERR_PTR(-EINVAL);
	return old_ns;
}

static inline void cidns_on_fork(struct nsproxy *nsproxy, struct task_struct *tsk) { }

/* Without the namespace, ns_cid is the CPU number. */
static inline int cid_ns_get_max_prctl(void)
{
	return nr_cpu_ids;
}

static inline void sched_cid_switch_ns(struct task_struct *t, struct nsproxy *new) { }
#endif /* !CONFIG_CID_NS */

DEFINE_FREE(cid_ns, struct cid_namespace *, if (_T) put_cid_ns(_T))

#endif /* _LINUX_CID_NAMESPACE_H */
