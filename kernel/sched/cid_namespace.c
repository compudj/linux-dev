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
#include <linux/shmem_fs.h>
#include <linux/hugetlb.h>
#include <linux/mm.h>
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
 * Memory bound to a namespace: a memfd created with MFD_CID_NS, huge
 * pages or not, or a System V segment created with SHM_CID_NS, belongs to
 * its creator's namespace, and only the namespace's processes map it, so
 * that a table sized by the maximum is never indexed from another space.
 * The binding is the inode's, set before any descriptor exists and cleared
 * at its eviction, and a mapping never outlives the membership: a mapping
 * is never inherited by a fork(2) child, which maps the object for itself
 * (VMA_DONTCOPY, which madvise(MADV_DOFORK) cannot clear), setns(2) is
 * refused while one exists, and execve(2) starts from a fresh mm. Kernel-side
 * access to a process's memory (ptrace(2), process_vm_writev(2)) is not a
 * mapping and is left alone: the hazard is a user instruction indexing
 * with an ID of another space.
 */
static struct cid_namespace **cid_ns_file_slot(struct file *file)
{
	/* The underlying file of a System V segment too: same mapping, same inode. */
	if (shmem_file(file))
		return &SHMEM_I(file_inode(file))->cid_ns;
#ifdef CONFIG_HUGETLBFS
	if (is_file_hugepages(file))
		return &HUGETLBFS_I(file_inode(file))->cid_ns;
#endif
	return NULL;
}

/* At the creation of @file: nothing else refers to it, nothing to lock. */
int cid_ns_bind_file(struct file *file)
{
	struct cid_namespace **slot = cid_ns_file_slot(file);

	if (!slot)
		return -EINVAL;
	*slot = get_cid_ns(current->nsproxy->cid_ns);
	return 0;
}

/* At the eviction of the inode. */
void cid_ns_unbind(struct cid_namespace **slot)
{
	if (*slot) {
		put_cid_ns(*slot);
		*slot = NULL;
	}
}

/* The namespace the memory of @file is bound to, NULL when none. */
static struct cid_namespace *cid_ns_of_file(struct file *file)
{
	struct cid_namespace **slot = cid_ns_file_slot(file);

	return slot ? *slot : NULL;
}

bool cid_ns_file_bound(struct file *file)
{
	return cid_ns_of_file(file) != NULL;
}

/* From the mmap hooks: unbound, or bound to the caller's namespace. */
bool cid_ns_may_map(struct file *file)
{
	struct cid_namespace *ns = cid_ns_of_file(file);

	return !ns || ns == current->nsproxy->cid_ns;
}

/* The first mapping of @mm from @start of memory bound to a namespace other than @ns. */
static struct vm_area_struct *cid_ns_find_other(struct mm_struct *mm, unsigned long start,
						struct cid_namespace *ns)
{
	VMA_ITERATOR(vmi, mm, start);
	struct vm_area_struct *vma;

	for_each_vma(vmi, vma) {
		struct cid_namespace *bound;

		if (!vma->vm_file)
			continue;
		bound = cid_ns_of_file(vma->vm_file);
		if (bound && bound != ns)
			return vma;
	}
	return NULL;
}

/* Does @mm map memory bound to a namespace other than @ns? */
static bool cid_ns_mm_maps_other(struct mm_struct *mm, struct cid_namespace *ns)
{
	bool maps;

	if (!mm)
		return false;
	mmap_read_lock(mm);
	maps = cid_ns_find_other(mm, 0, ns) != NULL;
	mmap_read_unlock(mm);
	return maps;
}

/*
 * A fork(2) without CLONE_VM, and execve(2): the task takes the namespace
 * its creator set for its children. The membership of the domain follows
 * in sched_post_fork() and switch_task_namespaces(). No mapping of memory
 * bound to a namespace follows: a fork(2) child inherits none (VMA_DONTCOPY),
 * an execve(2) has a fresh mm already.
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
	/* Its mappings of memory bound to the namespace it leaves: unmapped first, by the caller. */
	if (cid_ns_mm_maps_other(current->mm, ns))
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
