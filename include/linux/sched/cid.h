/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_CID_H
#define _LINUX_SCHED_CID_H
/*
 * The concurrency ID domain of an IPC namespace created with CLONE_IPC_CID,
 * see Documentation/scheduler/sched-max-concurrency.rst.
 */
#include <linux/err.h>
#include <linux/sched.h>
struct ipc_namespace;
struct ns_cid;
struct nsproxy;
#ifdef CONFIG_SCHED_MM_CID
struct ns_cid *ns_cid_create(struct ipc_namespace *ns);
void ns_cid_destroy(struct ns_cid *nc);
int sched_cid_setns(struct task_struct *t, struct ipc_namespace *ns);
void sched_cid_setns_done(struct task_struct *t);
void sched_cid_switch_ns(struct task_struct *t, struct nsproxy *new);
int sched_cid_fork_ns(struct task_struct *p);
unsigned int ns_cid_max(struct ipc_namespace *ns);
unsigned int ns_cid_cur(struct ipc_namespace *ns);
#else /* !CONFIG_SCHED_MM_CID */
static inline struct ns_cid *ns_cid_create(struct ipc_namespace *ns) { return ERR_PTR(-EINVAL); }
static inline void ns_cid_destroy(struct ns_cid *nc) { }
static inline int sched_cid_setns(struct task_struct *t, struct ipc_namespace *ns) { return 0; }
static inline void sched_cid_setns_done(struct task_struct *t) { }
static inline void sched_cid_switch_ns(struct task_struct *t, struct nsproxy *new) { }
static inline int sched_cid_fork_ns(struct task_struct *p) { return 0; }
static inline unsigned int ns_cid_max(struct ipc_namespace *ns) { return 0; }
static inline unsigned int ns_cid_cur(struct ipc_namespace *ns) { return 0; }
#endif /* !CONFIG_SCHED_MM_CID */
#endif /* !_LINUX_SCHED_CID_H */
