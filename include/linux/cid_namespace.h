/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CID_NAMESPACE_H
#define _LINUX_CID_NAMESPACE_H
/*
 * The concurrency ID namespace: the processes of one share a space of
 * concurrency IDs, reported by rseq(2) in ns_cid, below a maximum the
 * namespace is created with. See
 * Documentation/scheduler/sched-max-concurrency.rst.
 */
#include <linux/sched.h>
#include <linux/nsproxy.h>
#include <linux/ns_common.h>
#include <linux/rseq_types.h>
#include <linux/cpumask.h>
#include <linux/err.h>

struct file;
struct task_group;
struct user_namespace;
extern struct user_namespace init_user_ns;

/*
 * @home: the cgroup the namespace lives in: the nearest limited one at or
 *	 above the one its first member joined in, which keeps it on a list
 *	 (@home_node) while it exists; a member cannot leave its subtree,
 *	 see conc_ns_home(). NULL where no limit applied.
 * @max: the exclusive upper bound of the IDs, taken from the effective
 *	 ceiling of the nearest cgroup with one at or above the first
 *	 member's, or the number of possible CPUs, when that member joins;
 *	 never changes (@max_fixed). The initial namespace, whose ID is the
 *	 CPU number, reports nr_cpu_ids instead.
 * @soft: the bound the IDs are kept below now: the effective limit in
 *	 force at @home, fed from there (see conc_ns_feed()), capped by @max.
 * @dom: the engine's domain, unused by the initial namespace.
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
				  struct cid_namespace *old_ns);
void free_cid_ns(struct cid_namespace *ns);
void cidns_on_fork(struct nsproxy *nsproxy, struct task_struct *tsk);

static inline void put_cid_ns(struct cid_namespace *ns)
{
	if (ns_ref_put(ns))
		free_cid_ns(ns);
}

/* The exclusive upper bound of the IDs of @ns, for prctl(PR_GET_NS_CID_MAX). */
static inline unsigned int cid_ns_max(struct cid_namespace *ns)
{
	return ns == &init_cid_ns ? nr_cpu_ids : ns->max;
}

static inline int cid_ns_get_max_prctl(void)
{
	return cid_ns_max(current->nsproxy->cid_ns);
}

/*
 * Memory bound to a namespace at its creation (memfd_create(MFD_CID_NS)):
 * mapped by the namespace's processes only, so that a table sized by the
 * maximum is never indexed from another space. The binding is the inode's.
 */
int cid_ns_bind_file(struct file *file);
void cid_ns_unbind(struct cid_namespace **slot);
bool cid_ns_file_bound(struct file *file);
bool cid_ns_may_map(struct file *file);

/* The engine's side, kernel/sched/cid.c. */
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
						struct cid_namespace *old_ns)
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

static inline int cid_ns_bind_file(struct file *file)
{
	return -EINVAL;
}

static inline bool cid_ns_file_bound(struct file *file)
{
	return false;
}

static inline bool cid_ns_may_map(struct file *file)
{
	return true;
}
#endif /* !CONFIG_CID_NS */

DEFINE_FREE(cid_ns, struct cid_namespace *, if (_T) put_cid_ns(_T))

#endif /* _LINUX_CID_NAMESPACE_H */
