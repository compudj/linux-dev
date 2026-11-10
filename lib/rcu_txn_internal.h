/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Single-writer RCU pseudo-transactions: the steps of a commit
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */
#ifndef _LIB_RCU_TXN_INTERNAL_H
#define _LIB_RCU_TXN_INTERNAL_H

/*
 * The steps of a commit that installs proxies, and the resolve sections that
 * proxies are read in.  See __rcu_txn_commit_desc() in rcu_txn.c.  The KUnit
 * tests include this as well.  They stage a transaction between two steps,
 * and hold resolve sections open.
 */

#include <linux/rcu_txn.h>

/*
 * Resolve a proxy to its current target.
 *
 * @proxy must come from a load of its slot, within the same resolve section as
 * this call.  What orders the loads below is described under "Store-release /
 * load-acquire pairs" at the top of <linux/rcu_txn.h>.
 *
 * The selector indexes proxy->ptr[] directly, so resolution is a data
 * dependency rather than a conditional branch.
 */
static inline void *__rcu_txn_proxy_resolve(const struct rcu_txn_record *proxy)
{
	unsigned long sel;

	/*
	 * Load-acquire (A) pairs with the store-release (B) of
	 * __rcu_txn_group_commit().
	 */
	sel = smp_load_acquire(&READ_ONCE(proxy->group)->selector);
	return READ_ONCE(proxy->ptr[sel]);
}

/*
 * Resolve sections: see lib/rcu_txn.c.  The KUnit tests hold sections open
 * with these two.
 */

/*
 * Enter a resolve section.  Interrupts must be disabled.  Return its SRCU
 * index.
 */
int __rcu_txn_resolve_enter(void);

/* Leave the section of @idx, which __rcu_txn_resolve_enter() returned. */
void __rcu_txn_resolve_exit(int idx);

/*
 * Wait for the resolve sections in flight, without sleeping.  On return, no
 * reader can still use a proxy that was settled away before the call.  Call it
 * from any context except NMI and a resolve section.  It disables interrupts
 * while it waits.
 */
void __rcu_txn_synchronize_resolve(void);

/*
 * Local transactions: see lib/rcu_txn.c.  The number of descriptors in a CPU's
 * pool.  It covers the handles of every context of that CPU, and the
 * descriptors that wait for a grace period.
 */
#define RCU_TXN_LOCAL_POOL	16

/*
 * Give a local handle's descriptor back to its pool.  All its proxies must be
 * settled.  The descriptor is free again a grace period later, or earlier if a
 * steal takes it back.
 */
void __rcu_txn_local_retire(struct rcu_txn_desc *desc);

/* The number of times a pool took its descriptors back, on every CPU. */
unsigned long __rcu_txn_local_steals(void);

static inline void __rcu_txn_group_init(struct rcu_txn_group *group)
{
	group->selector = 0;
}

/*
 * Commit the flip group.  The flip is a single store-release of 1 to the
 * selector.  It switches every record of the transaction from old to new.
 * Call it once every record is installed: the store-release orders the
 * installs before the flip.
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
	 * The tag must be non-zero and within RCU_TXN_TAG_MASK.  Every value
	 * matches a zero tag: see __rcu_txn_holds_proxy().  Readers would
	 * then resolve plain values as proxies.
	 */
	RCU_TXN_WARN_ON(!r->tag || (r->tag & ~RCU_TXN_TAG_MASK));
	/*
	 * No bit of the tag may be set in the record's address.  The OR would
	 * leave such a bit as it is.  __rcu_txn_untag() would then subtract
	 * to a wrong address.
	 */
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
 * At the multi-record install, @r's slot must hold no proxy.  It must also
 * still hold the recorded old.
 *
 * The value check catches a lost update.  Another writer committed on the
 * slot after this transaction's record() and before its install.  It left a
 * plain value, so the no-proxy check passes.  Installing would make readers
 * see the recorded old value again.  The settle would then overwrite the
 * other writer's value.
 *
 * A correct single-writer program never trips it.  This transaction has not
 * written the slot yet.  The writer's earlier transactions settled before
 * they returned.  Chaining keeps the committed old in ptr[0].
 */
static inline void __rcu_txn_excl_slot_installable(struct rcu_txn_record *r)
{
	__rcu_txn_excl_slot_free(r->slot, r->tag, "install");
	__rcu_txn_excl_slot_unchanged(r, "install");
}

/*
 * Records must target pairwise-distinct slots: see rcu_txn_record().  They are
 * in record order, not sorted by slot, so check every pair.
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
