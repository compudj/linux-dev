// SPDX-License-Identifier: GPL-2.0
/*
 * The concurrency ID namespace. Its processes share one space of
 * concurrency IDs, which rseq(2) reports in ns_cid. The namespace says
 * which tasks share an ID space. The cgroup concurrency limit says how many
 * of them run at once.
 *
 * The namespace takes a maximum and a soft limit from the cgroups when it
 * is created, see conc_ns_home(). The IDs stay below both. The maximum
 * never changes. The soft limit follows the limit of the cgroup, see
 * conc_ns_feed().
 *
 * It is modelled on the time namespace. unshare(2) and setns(2) set the
 * namespace of the children. It is committed at fork(2) and execve(2).
 *
 * The membership of the namespace's domain is in cid.c. See
 * Documentation/scheduler/sched-max-concurrency.rst.
 */
#include <linux/cid_namespace.h>
#include <linux/user_namespace.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/proc_ns.h>
#include <linux/nstree.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/err.h>

#include "sched.h"

/*
 * The initial namespace has no domain. Its ID is the CPU number and its
 * maximum is nr_cpu_ids. Two tasks which run at the same time are on
 * different CPUs. So the IDs are unique and below the maximum, as in any
 * other namespace.
 */
struct cid_namespace init_cid_ns = {
	.ns		= NS_COMMON_INIT(init_cid_ns),
	.user_ns	= &init_user_ns,
};

static struct ucounts *inc_cid_namespaces(struct user_namespace *ns)
{
	return inc_ucount(ns, current_euid(), UCOUNT_CID_NAMESPACES);
}

static void dec_cid_namespaces(struct ucounts *ucounts)
{
	dec_ucount(ucounts, UCOUNT_CID_NAMESPACES);
}

/*
 * Create a namespace for the children of the creator. The caller gives it
 * its home, its maximum and its soft limit, see copy_cid_ns(). Until then
 * both numbers are the number of possible CPUs, which means no bound.
 */
static struct cid_namespace *clone_cid_ns(struct user_namespace *user_ns)
{
	struct cid_namespace *ns;
	struct ucounts *ucounts;
	int err;

	err = -ENOSPC;
	ucounts = inc_cid_namespaces(user_ns);
	if (!ucounts)
		goto fail;

	err = -ENOMEM;
	/*
	 * A process in a user namespace can create these. Charge the memory
	 * to its cgroup.
	 */
	ns = kzalloc_obj(*ns, GFP_KERNEL_ACCOUNT);
	if (!ns)
		goto fail_dec;

	err = ns_common_init(ns);
	if (err)
		goto fail_free;

	ns->max = num_possible_cpus();
	ns->soft = ns->max;
	ns->max_fixed = false;
	ns->home_fixed = false;
	ns->home = NULL;
	INIT_LIST_HEAD(&ns->home_node);
	err = cid_ns_domain_init(ns);
	if (err)
		goto fail_common;

	ns->ucounts = ucounts;
	ns->user_ns = get_user_ns(user_ns);
	ns_tree_add(ns);
	return ns;

fail_common:
	ns_common_free(ns);
fail_free:
	kfree(ns);
fail_dec:
	dec_cid_namespaces(ucounts);
fail:
	return ERR_PTR(err);
}

struct cid_namespace *copy_cid_ns(u64 flags, struct user_namespace *user_ns,
				  struct cid_namespace *old_ns,
				  struct task_struct *tsk)
{
	struct cid_namespace *ns;

	if (!(flags & CLONE_NEWCID))
		return get_cid_ns(old_ns);
	ns = clone_cid_ns(user_ns);
	/*
	 * unshare(2): @tsk is the caller. Its cgroup decides the home and the
	 * maximum now. For clone3(2), @tsk is the child, which has no cgroup
	 * yet: its join decides, later in the same call.
	 */
	if (!IS_ERR(ns) && tsk == current)
		conc_ns_home(ns, tsk);
	return ns;
}

/* Return the cgroup ID of the home of the namespace, or 0 if it has none. */
u64 cid_ns_home_id(struct ns_common *ns)
{
	return conc_ns_home_id(container_of(ns, struct cid_namespace, ns));
}

void free_cid_ns(struct cid_namespace *ns)
{
	ns_tree_remove(ns);
	conc_ns_unhome(ns);
	cid_ns_domain_exit(ns);
	dec_cid_namespaces(ns->ucounts);
	put_user_ns(ns->user_ns);
	ns_common_free(ns);
	kfree_rcu(ns, ns.ns_rcu);
}

/*
 * Commit the namespace of the children to @tsk. Called at a fork(2)
 * without CLONE_VM and at execve(2). The task takes the namespace which
 * its creator set for its children. sched_post_fork() and
 * switch_task_namespaces() then update the membership of the domain.
 */
void cidns_on_fork(struct nsproxy *nsproxy, struct task_struct *tsk)
{
	struct cid_namespace *ns = nsproxy->cid_ns_for_children;

	/* create_new_namespaces() already incremented the ref counter */
	if (nsproxy->cid_ns == ns)
		return;

	get_cid_ns(ns);
	put_cid_ns(nsproxy->cid_ns);
	nsproxy->cid_ns = ns;
}

static struct ns_common *cidns_get(struct task_struct *task)
{
	struct cid_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		ns = nsproxy->cid_ns;
		get_cid_ns(ns);
	}
	task_unlock(task);

	return ns ? &ns->ns : NULL;
}

static struct ns_common *cidns_for_children_get(struct task_struct *task)
{
	struct cid_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		ns = nsproxy->cid_ns_for_children;
		get_cid_ns(ns);
	}
	task_unlock(task);

	return ns ? &ns->ns : NULL;
}

static void cidns_put(struct ns_common *ns)
{
	put_cid_ns(to_cid_ns(ns));
}

/*
 * Install @new for setns(2). The caller joins the namespace, and its
 * children are born in it, as with a time namespace. The caller must be
 * single-threaded, so the threads of a process are never in different
 * namespaces.
 */
static int cidns_install(struct nsset *nsset, struct ns_common *new)
{
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct cid_namespace *ns = to_cid_ns(new);

	if (!current_is_single_threaded())
		return -EUSERS;

	if (!ns_capable(ns->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	/* Refuse a caller whose cgroup is outside the subtree of the home. */
	if (!conc_ns_may_join(ns))
		return -EBUSY;

	get_cid_ns(ns);
	put_cid_ns(nsproxy->cid_ns);
	nsproxy->cid_ns = ns;

	get_cid_ns(ns);
	put_cid_ns(nsproxy->cid_ns_for_children);
	nsproxy->cid_ns_for_children = ns;
	return 0;
}

static struct user_namespace *cidns_owner(struct ns_common *ns)
{
	return to_cid_ns(ns)->user_ns;
}

const struct proc_ns_operations cidns_operations = {
	.name		= "cid",
	.get		= cidns_get,
	.put		= cidns_put,
	.install	= cidns_install,
	.owner		= cidns_owner,
};

const struct proc_ns_operations cidns_for_children_operations = {
	.name		= "cid_for_children",
	.real_ns_name	= "cid",
	.get		= cidns_for_children_get,
	.put		= cidns_put,
	.install	= cidns_install,
	.owner		= cidns_owner,
};

void __init cid_ns_init(void)
{
	ns_tree_add(&init_cid_ns);
}
