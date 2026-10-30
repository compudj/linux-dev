/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RSEQ_TYPES_H
#define _LINUX_RSEQ_TYPES_H

#include <linux/irq_work_types.h>
#include <linux/types.h>
#include <linux/workqueue_types.h>

#ifdef CONFIG_RSEQ
struct rseq;
struct task_group;
struct ipc_namespace;

/*
 * rseq_event::has_rseq contains the ABI version number so preserving it
 * in AND operations requires a mask.
 */
#define RSEQ_HAS_RSEQ_VERSION_MASK	0xff

/**
 * struct rseq_event - Storage for rseq related event management
 * @all:		Compound to initialize and clear the data efficiently
 * @events:		Compound to access events with a single load/store
 * @sched_switch:	True if the task was scheduled and needs update on
 *			exit to user
 * @ids_changed:	Indicator that IDs need to be updated
 * @user_irq:		True on interrupt entry from user mode
 * @has_rseq:		Greater than 0 if the task has a rseq pointer installed.
 *			Contains the RSEQ version number
 * @error:		Compound error code for the slow path to analyze
 * @fatal:		User space data corrupted or invalid
 * @slowpath:		Indicator that slow path processing via TIF_NOTIFY_RESUME
 *			is required
 *
 * @sched_switch and @ids_changed must be adjacent and the combo must be
 * 16bit aligned to allow a single store, when both are set at the same
 * time in the scheduler.
 */
struct rseq_event {
	union {
		u64				all;
		struct {
			union {
				u32		events;
				struct {
					u8	sched_switch;
					u8	ids_changed;
					u8	user_irq;
				};
			};

			u8			has_rseq;
			u8			__pad;
			union {
				u16		error;
				struct {
					u8	fatal;
					u8	slowpath;
				};
			};
		};
	};
};

/**
 * struct rseq_ids - Cache for ids, which need to be updated
 * @cpu_cid:	Compound of @cpu_id and @mm_cid to make the
 *		compiler emit a single compare on 64-bit
 * @cpu_id:	The CPU ID which was written last to user space
 * @mm_cid:	The MM CID which was written last to user space
 * @node_id:	The node ID which was written last to user space
 *
 * @cpu_id, @mm_cid and @node_id are updated when the data is written to user space.
 */
struct rseq_ids {
	union {
		u64		cpu_cid;
		struct {
			u32	cpu_id;
			u32	mm_cid;
		};
	};
	u32			node_id;
};

/**
 * union rseq_slice_state - Status information for rseq time slice extension
 * @state:	Compound to access the overall state
 * @enabled:	Time slice extension is enabled for the task
 * @granted:	Time slice extension was granted to the task
 */
union rseq_slice_state {
	u16			state;
	struct {
		u8		enabled;
		u8		granted;
	};
};

/**
 * struct rseq_slice - Status information for rseq time slice extension
 * @state:	Time slice extension state
 * @expires:	The time when a grant expires
 * @yielded:	Indicator for rseq_slice_yield()
 */
struct rseq_slice {
	union rseq_slice_state	state;
	u64			expires;
	u8			yielded;
};

/**
 * struct rseq_data - Storage for all rseq related data
 * @usrptr:	Pointer to the registered user space RSEQ memory
 * @len:	Length of the RSEQ region
 * @sig:	Signature of critical section abort IPs
 * @event:	Storage for event management
 * @ids:	Storage for cached CPU ID and MM CID
 * @slice:	Storage for time slice extension data
 */
struct rseq_data {
	struct rseq __user		*usrptr;
	u32				len;
	u32				sig;
	struct rseq_event		event;
	struct rseq_ids			ids;
#ifdef CONFIG_RSEQ_SLICE_EXTENSION
	struct rseq_slice		slice;
#endif
};

#else /* CONFIG_RSEQ */
struct rseq_data { };
#endif /* !CONFIG_RSEQ */

#ifdef CONFIG_SCHED_MM_CID

#define MM_CID_UNSET	BIT(31)
#define MM_CID_ONCPU	BIT(30)
#define MM_CID_TRANSIT	BIT(29)

/*
 * A cid domain hands out concurrency IDs to the tasks of its users that
 * run at the same time: the mm of a process (mm_cid) or an IPC namespace
 * (ns_cid). See Documentation/scheduler/sched-max-concurrency.rst.
 */
enum cid_domain_kind {
	CID_DOMAIN_MM,
	CID_DOMAIN_NS,
};
/**
 * struct cid_user - Storage for per task CID data, one per domain kind
 * @active:	The task is a user of the domain
 * @cid:	The CID associated to the task either permanently or
 *		borrowed from the CPU
 * @node:	Queued in the domain's user list
 */
struct cid_user {
	unsigned int		active;
	unsigned int		cid;
	struct hlist_node	node;
};

/**
 * struct cid_pcpu - Storage for per CPU CID data
 * @cid:	The CID associated to the CPU either permanently or
 *		while a task with a CID is running
 */
struct cid_domain;
/*
 * @cid: the CID the CPU owns (MM_CID_ONCPU), caches or hands over
 * @cpu, @dom: the CPU and the domain this record belongs to
 * @node: on the CPU's list of the domains whose CID it owns, see
 *	  cid_pcpu_write(); written under the CPU's runqueue lock
 */
struct cid_pcpu {
	unsigned int		cid;
	int			cpu;
	struct cid_domain	*dom;
	struct list_head	node;
}____cacheline_aligned_in_smp;

/**
 * struct cid_domain - Storage for per domain CID data
 * @pcpu:		Per CPU storage for CIDs associated to a CPU
 * @mode:		Indicates per CPU and transition mode
 * @max_cids:		The exclusive maximum CID value for allocation and convergence
 * @cidmask:		The allocation bitmap, num_possible_cpus() bits
 * @cpus_allowed:	Union of the users' allowed CPUs, growth only
 * @kind:		Which task field holds the users' state (enum cid_domain_kind)
 * @irq_work:		irq_work to handle the affinity mode change case
 * @work:		Regular work to handle the affinity mode change case
 * @lock:		Spinlock to protect against affinity setting which can't take @mutex
 * @mutex:		Mutex to serialize joins and leaves related to this domain
 * @user_list:		List of the users of the domain
 * @nr_cpus_allowed:	The number of CPUs in @cpus_allowed
 * @users:		The number of tasks using the domain. For an mm, separate
 *			from mm::mm_users as that is modified by mmget()/mm_put()
 *			by other entities which do not actually share the MM.
 * @pcpu_thrs:		Threshold for switching back from per CPU mode
 * @update_deferred:	A deferred switch back to per task mode is pending.
 * @bound:		Concurrency bound from cpu.concurrency.max, 0 if none.
 *			See Documentation/scheduler/sched-max-concurrency.rst
 * @bound_update:	A deferred bound update is pending
 * @lca:		Lowest common ancestor task group of the users
 * @tops:		Buffer for the topmost limited groups of the users, when
 *			they span several; grown in process context
 * @nr_tops:		Its capacity
 * @tops_wanted:	The capacity the last scan needed
 */
struct cid_domain {
	/* Hotpath read mostly members */
	struct cid_pcpu		__percpu *pcpu;
	unsigned int		mode;
	unsigned int		max_cids;
	unsigned long		*cidmask;
	struct cpumask		*cpus_allowed;
	unsigned int		kind;

	/* Rarely used. Moves @lock and @mutex into the second cacheline */
	struct irq_work		irq_work;
	struct work_struct	work;

	raw_spinlock_t		lock;
	struct mutex		mutex;
	struct hlist_head	user_list;

	/* Low frequency modified */
	unsigned int		nr_cpus_allowed;
	unsigned int		users;
	unsigned int		pcpu_thrs;
	unsigned int		update_deferred;
	unsigned int		bound;
	unsigned int		bound_update;
#ifdef CONFIG_SCHED_MAX_CONCURRENCY
	struct task_group	*lca;
	struct task_group	**tops;
	unsigned int		nr_tops;
	unsigned int		tops_wanted;
#endif /* CONFIG_SCHED_MAX_CONCURRENCY */
} ____cacheline_aligned;
/**
 * struct ns_cid - The cid domain of an IPC namespace created with CLONE_IPC_CID
 * @dom:	The domain
 * @ns:		The namespace, referenced by each member while it is one
 * @ceiling_css:	The cpu css of the innermost cgroup with a ceiling of the first
 *		member, whose subtree no member may leave; NULL when there
 *		was none
 * @ceiling:	The ceiling, frozen at the first admission: the effective
 *		ceiling of @ceiling_css's cgroup then, or the number of
 *		possible CPUs
 * @frozen:	The first admission happened
 *
 * See Documentation/scheduler/sched-max-concurrency.rst
 */
struct cgroup_subsys_state;
#else /* CONFIG_SCHED_MM_CID */
struct cid_domain { };
struct cid_user { };
#endif /* !CONFIG_SCHED_MM_CID */

#endif
