/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Single-writer RCU pseudo-transactions: the steps of a commit
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */
#ifndef _LIB_RCU_TXN_INTERNAL_H
#define _LIB_RCU_TXN_INTERNAL_H

/*
 * The steps of a commit that installs proxies: see __rcu_txn_commit_desc() in
 * rcu_txn.c.  The KUnit tests include this as well, to stage a transaction
 * between two steps.
 */

#include <linux/rcu_txn.h>

static inline void __rcu_txn_group_init(struct rcu_txn_group *group)
{
	group->selector = 0;
}

/*
 * Commit the flip group: switch every record in the transaction from old to new
 * with a single store-release.  Must be called once every record is installed:
 * the store-release orders the installs before the flip.
 */
static inline void __rcu_txn_group_commit(struct rcu_txn_group *group)
{
	/*
	 * Store-release (B) pairs with the load-acquire (A) of
	 * __rcu_txn_proxy_resolve().
	 */
	smp_store_release(&group->selector, 1);
}

/*
 * Install record @r's tagged proxy into its slot, bound to flip group @group.
 * Readers resolve it to old until the group commits.  The installed value is
 * @r's address OR'd with the record's tag.
 */
static inline void __rcu_txn_record_install(struct rcu_txn_group *group,
					    struct rcu_txn_record *r)
{
	r->group = group;
	/* Store-release of slot pairs with rcu_dereference(). */
	rcu_assign_pointer(*r->slot, (void *)((unsigned long)r | r->tag));
}

#endif /* _LIB_RCU_TXN_INTERNAL_H */
