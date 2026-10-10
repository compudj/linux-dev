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
	/*
	 * Installing requires a non-zero tag within RCU_TXN_TAG_MASK, none of
	 * whose bits is set in the record's address.  Under a zero tag every
	 * plain value passes rcu_txn_is_proxy(), and readers would resolve live
	 * values as proxies.  With a tag bit already set in the address, the OR
	 * changes nothing and __rcu_txn_untag() subtracts to a wrong address.
	 */
	RCU_TXN_WARN_ON(!r->tag || (r->tag & ~RCU_TXN_TAG_MASK));
	RCU_TXN_WARN_ON((unsigned long)r & r->tag);
	r->group = group;
	/* Store-release of slot pairs with rcu_dereference(). */
	rcu_assign_pointer(*r->slot, (void *)((unsigned long)r | r->tag));
}

/*
 * The install and settle checks of CONFIG_DEBUG_RCU_TXN: see
 * <linux/rcu_txn.h>.
 */
#ifdef CONFIG_DEBUG_RCU_TXN

/* At settle, @r's slot must still hold the proxy this writer installed. */
static inline void __rcu_txn_excl_slot_ours(struct rcu_txn_record *r)
{
	void *want = (void *)((unsigned long)r | r->tag);
	void *v = rcu_access_pointer(*r->slot);

	WARN(v != want,
	     "rcu_txn: slot %p holds %p at settle, expected our installed proxy %p: a concurrent writer overwrote it\n",
	     r->slot, v, want);
}

/*
 * At the multi-record install, @r's slot must hold no proxy and must still
 * hold the recorded old.
 *
 * The value check catches a racing writer that ran a complete transaction on
 * the slot -- install, flip, settle -- between this transaction's record() and
 * its install.  That writer leaves a plain value behind, so the slot looks
 * free; installing over it would publish a stale old to readers, then settle
 * over the other writer's committed value.
 *
 * A correct single-writer program never trips it: this transaction has not
 * written the slot yet, the writer's earlier transactions settled before
 * returning, and chaining keeps the committed old in ptr[0].
 */
static inline void __rcu_txn_excl_slot_installable(struct rcu_txn_record *r)
{
	__rcu_txn_excl_slot_free(r->slot, r->tag, "install");
	__rcu_txn_excl_slot_unchanged(r, "install");
}

/*
 * Records must target pairwise-distinct slots: see rcu_txn_record().  They are
 * in record order, hence the pairwise scan.
 */
static inline void
__rcu_txn_assert_distinct_slots(const struct rcu_txn_record *records,
				unsigned int nr)
{
	unsigned int i, j;

	for (i = 1; i < nr; i++)
		for (j = 0; j < i; j++)
			WARN_ON(records[i].slot == records[j].slot);
}

#else	/* !CONFIG_DEBUG_RCU_TXN */

static inline void __rcu_txn_excl_slot_ours(struct rcu_txn_record *r)
{
}

static inline void __rcu_txn_excl_slot_installable(struct rcu_txn_record *r)
{
}

static inline void
__rcu_txn_assert_distinct_slots(const struct rcu_txn_record *records,
				unsigned int nr)
{
}

#endif	/* CONFIG_DEBUG_RCU_TXN */

#endif /* _LIB_RCU_TXN_INTERNAL_H */
