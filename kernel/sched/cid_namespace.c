// SPDX-License-Identifier: GPL-2.0
/*
 * The concurrency ID namespace: the processes of one share a space of
 * concurrency IDs, reported by rseq(2) in ns_cid, below a maximum the
 * namespace is created with. The namespace says who shares an ID space;
 * the cgroup concurrency limit says how many of them may run at once, and
 * the namespace copies that number once, at its creation. See
 * Documentation/scheduler/sched-max-concurrency.rst. Modelled on the time
 * namespace: unshare(2) and setns(2) set what the children get, committed
 * at fork(2) and execve(2).
 *
 * The membership of the namespace's domain is in cid.c.
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
 * The initial namespace is degenerate: no domain, the ID is the CPU
 * number, the maximum nr_cpu_ids. Two tasks running at once are on
 * different CPUs, so the IDs are unique and below the maximum as in
 * any other namespace.
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
 * A new namespace, for the creator's children. It has no home, maximum
 * or soft limit until its first member joins (conc_ns_home()): with
 * clone3(CLONE_NEWCID | CLONE_INTO_CGROUP) that member is born in a
 * cgroup other than the creator's, the container's, whose ceiling and
 * limit are the ones meant. Until then the numbers are the number of
 * possible CPUs: no bound.
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
	/* A tenant with a user namespace makes these: charged to it. */
	ns = kzalloc_obj(*ns, GFP_KERNEL_ACCOUNT);
	if (!ns)
		goto fail_dec;

	err = ns_common_init(ns);
	if (err)
		goto fail_free;

	ns->max = num_possible_cpus();
	ns->soft = ns->max;
	ns->max_fixed = false;
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
				  struct cid_namespace *old_ns)
{
	if (!(flags & CLONE_NEWCID))
		return get_cid_ns(old_ns);
	return clone_cid_ns(user_ns);
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
 * A fork(2) without CLONE_VM, and execve(2): the task takes the namespace
 * its creator set for its children. The membership of the domain follows
 * in sched_post_fork() and switch_task_namespaces().
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
 * setns(2): a single-threaded process joins for itself and its children,
 * as it does a time namespace. Its ID space changes under it, which it
 * asked for; the threads of a process never belong to different spaces.
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
	/* From outside the cgroup subtree the namespace lives in: refused, as a migration out of it is. */
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
