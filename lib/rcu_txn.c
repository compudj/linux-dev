// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Single-writer RCU pseudo-transactions: the out-of-line paths
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * The engine is inline in <linux/rcu_txn.h>.  What lives here is what allocates
 * a transaction's descriptor, the commit that installs proxies, and the resolve
 * sections in which readers read them.
 */

#include <kunit/visibility.h>
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/overflow.h>
#include <linux/rcu_txn.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/string.h>

#include "rcu_txn_internal.h"

/*
 * Resolve sections
 * ================
 *
 * See "Readers and reclaim" in <linux/rcu_txn.h>.  The sections have their own
 * atomic SRCU domain.  Its readers are NMI-safe.  Every section disables
 * interrupts, so the wait may run with interrupts disabled, as under a raw
 * spinlock.  The wait also disables interrupts itself: see
 * __rcu_txn_synchronize_resolve().
 */
DEFINE_STATIC_SRCU_ATOMIC(rcu_txn_resolve_srcu);

/* See rcu_txn_internal.h. */
int __rcu_txn_resolve_enter(void)
{
	RCU_TXN_WARN_ON(!irqs_disabled());
	return srcu_read_lock_atomic(&rcu_txn_resolve_srcu);
}
EXPORT_SYMBOL_IF_KUNIT(__rcu_txn_resolve_enter);

/* See rcu_txn_internal.h. */
void __rcu_txn_resolve_exit(int idx)
{
	RCU_TXN_WARN_ON(!irqs_disabled());
	srcu_read_unlock_atomic(&rcu_txn_resolve_srcu, idx);
}
EXPORT_SYMBOL_IF_KUNIT(__rcu_txn_resolve_exit);

/* See <linux/rcu_txn.h>. */
void *__rcu_txn_resolve_slow(void __rcu **slot, unsigned long tag)
{
	unsigned long flags;
	void *v;
	int idx;

	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_txn: resolve while RCU is not watching");
	idx = srcu_read_lock_atomic_irqsave(&rcu_txn_resolve_srcu, flags);
	/*
	 * The proxy the caller loaded before the section may belong to a
	 * descriptor that now serves another transaction.  Load the slot
	 * again, and resolve what it holds.
	 *
	 * If the slot is settled now, this load-acquire orders the caller's
	 * later loads after it.  A proxy gets the same from the load-acquire
	 * (A) of its selector.
	 */
	v = unrcu_pointer(smp_load_acquire(slot));
	if (rcu_txn_is_proxy(v, tag))
		v = __rcu_txn_proxy_resolve(__rcu_txn_untag(v, tag));
	srcu_read_unlock_atomic_irqrestore(&rcu_txn_resolve_srcu, idx, flags);
	return v;
}
EXPORT_SYMBOL_GPL(__rcu_txn_resolve_slow);

/* See rcu_txn_internal.h. */
void __rcu_txn_synchronize_resolve(void)
{
	unsigned long flags;

	/* An NMI would wait for the section it interrupted. */
	WARN_ON_ONCE(in_nmi());
	/*
	 * synchronize_srcu_atomic() keeps other waiters of the domain
	 * spinning until its grace period ends.  So an interrupt handler that
	 * waits on this CPU would deadlock: the wait it interrupted cannot
	 * end.
	 */
	local_irq_save(flags);
	synchronize_srcu_atomic(&rcu_txn_resolve_srcu);
	local_irq_restore(flags);
}
EXPORT_SYMBOL_IF_KUNIT(__rcu_txn_synchronize_resolve);

/*
 * Allocate a transaction descriptor for at least @cap records, with its flip
 * group initialized.  Its capacity is what the allocation holds, which may be
 * more.  Returns NULL if it cannot allocate.
 *
 * The records need RCU_TXN_RECORD_ALIGN.  kmalloc() aligns an object on the
 * largest power of two that divides its size.  The size of a descriptor is a
 * multiple of RCU_TXN_RECORD_ALIGN.  So is every size that
 * kmalloc_size_roundup() returns for it.
 */
static struct rcu_txn_desc *rcu_txn_desc_alloc(unsigned int cap, gfp_t gfp)
{
	struct rcu_txn_desc *desc;
	size_t size;

	size = kmalloc_size_roundup(struct_size(desc, records, cap));
	desc = kmalloc(size, gfp);
	if (!desc)
		return NULL;
	__rcu_txn_group_init(&desc->group);
	desc->cap = (size - sizeof(*desc)) / sizeof(desc->records[0]);
	return desc;
}

/*
 * Free a descriptor that installed no proxy.  It owes no grace period.
 * commit() and cancel() may run under a raw or a bit spinlock.  kfree() is
 * not allowed under a raw spinlock.  Once a per-CPU sheaf is full, SLUB takes
 * a spinlock_t, and PROVE_RAW_LOCK_NESTING reports it.  On PREEMPT_RT, this
 * also holds under a bit spinlock.  kfree_rcu() is allowed under both.  It
 * delays the free by a grace period.
 */
void __rcu_txn_free_desc(struct rcu_txn_desc *desc)
{
	kfree_rcu(desc, rcu_head);
}
EXPORT_SYMBOL_GPL(__rcu_txn_free_desc);

/* See <linux/rcu_txn.h>. */
bool __rcu_txn_resize(struct rcu_txn *t, unsigned int cap)
{
	struct rcu_txn_desc *desc;

	desc = rcu_txn_desc_alloc(cap, t->gfp);
	if (!desc)
		goto fail;
	if (t->nr)		/* @records is NULL until first allocated */
		memcpy(desc->records, t->records,
		       (size_t)t->nr * sizeof(*desc->records));
	if (t->desc)		/* NULL until first allocated */
		__rcu_txn_free_desc(t->desc);	/* record() may hold a lock */
	t->desc = desc;
	t->records = desc->records;
	t->cap = desc->cap;
	return true;

fail:
	t->state = RCU_TXN_OOM;	/* sticky */
	return false;
}
EXPORT_SYMBOL_GPL(__rcu_txn_resize);

/* See <linux/rcu_txn.h>. */
bool __rcu_txn_grow(struct rcu_txn *t)
{
	return __rcu_txn_resize(t, t->cap ? t->cap * 2 : RCU_TXN_CAP);
}
EXPORT_SYMBOL_GPL(__rcu_txn_grow);

/* See <linux/rcu_txn.h>. */
void __rcu_txn_commit_desc(struct rcu_txn_desc *desc, unsigned int nr)
{
	struct rcu_txn_record *records = desc->records;
	unsigned int i;

	__rcu_txn_assert_distinct_slots(records, nr);
	/* Install: readers still resolve to old, the selector being 0. */
	for (i = 0; i < nr; i++) {
		__rcu_txn_excl_slot_installable(&records[i]);
		__rcu_txn_record_install(&desc->group, &records[i]);
	}
	/* Commit: the flip.  Every proxy resolves to new. */
	__rcu_txn_group_commit(&desc->group);
	/* Settle. */
	for (i = 0; i < nr; i++) {
		__rcu_txn_excl_slot_ours(&records[i]);
		__rcu_txn_record_settle(&records[i]);
	}
	/* A reader may still hold a proxy, which lives in the descriptor. */
	kfree_rcu(desc, rcu_head);
}
EXPORT_SYMBOL_GPL(__rcu_txn_commit_desc);
