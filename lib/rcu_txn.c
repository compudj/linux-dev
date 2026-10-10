// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Single-writer RCU pseudo-transactions: the out-of-line paths
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * The engine is inline in <linux/rcu_txn.h>.  What lives here is what allocates
 * a transaction's descriptor, and the commit that installs proxies.
 */

#include <linux/export.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/rcu_txn.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "rcu_txn_internal.h"

/* Record-sized slots that the filter takes, past the records. */
#define RCU_TXN_BLOOM_SLOTS						\
	DIV_ROUND_UP(RCU_TXN_BLOOM_SIZE, sizeof(struct rcu_txn_record))

/*
 * The records that a descriptor of @slots record-sized slots holds.  Once they
 * are RCU_TXN_BLOOM_MIN or more, the filter takes the slots past them: a
 * descriptor with slots for that many records but not for the filter as well
 * holds RCU_TXN_BLOOM_MIN - 1 records.
 */
static unsigned int rcu_txn_desc_cap(unsigned int slots)
{
	if (slots < RCU_TXN_BLOOM_MIN)
		return slots;
	return max_t(unsigned int, slots - RCU_TXN_BLOOM_SLOTS,
		     RCU_TXN_BLOOM_MIN - 1);
}

/*
 * Allocate a transaction descriptor for at least @cap records, with its flip
 * group initialized.  Its capacity is what the allocation holds, which may be
 * more.  Returns NULL if it cannot allocate.
 *
 * The records need RCU_TXN_RECORD_ALIGN.  kmalloc() aligns an object on the
 * largest power of two that divides its size, and the size of a descriptor is a
 * multiple of that alignment, as is every size kmalloc_size_roundup() returns
 * for it.
 */
static struct rcu_txn_desc *rcu_txn_desc_alloc(unsigned int cap, gfp_t gfp)
{
	struct rcu_txn_desc *desc;
	size_t size;

	size = struct_size(desc, records, cap);
	if (cap >= RCU_TXN_BLOOM_MIN)
		size = size_add(size, RCU_TXN_BLOOM_SLOTS *
				      sizeof(struct rcu_txn_record));
	size = kmalloc_size_roundup(size);
	desc = kmalloc(size, gfp);
	if (!desc)
		return NULL;
	__rcu_txn_group_init(&desc->group);
	desc->slots = (size - sizeof(*desc)) / sizeof(desc->records[0]);
	desc->cap = rcu_txn_desc_cap(desc->slots);
	return desc;
}

/*
 * Free a descriptor that installed no proxy, which owes no grace period.
 * commit() and cancel() may run under a raw or a bit spinlock, where
 * PREEMPT_RT does not allow kfree().  It does allow kfree_rcu().
 */
void __rcu_txn_free_desc(struct rcu_txn_desc *desc)
{
	if (IS_ENABLED(CONFIG_PREEMPT_RT))
		kfree_rcu(desc, rcu_head);
	else
		kfree(desc);
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
	/*
	 * An armed filter moves too: both descriptors hold RCU_TXN_BLOOM_MIN
	 * records or more, so both have room for it.
	 */
	if (t->bloom_live)
		memcpy(&desc->records[desc->cap], __rcu_txn_bloom(t),
		       RCU_TXN_BLOOM_SIZE);
	kfree(t->desc);		/* NULL if it had none */
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

	/* Install: readers still resolve to old, the selector being 0. */
	for (i = 0; i < nr; i++)
		__rcu_txn_record_install(&desc->group, &records[i]);
	/* Flip: every proxy resolves to new. */
	__rcu_txn_group_commit(&desc->group);
	/* Settle. */
	for (i = 0; i < nr; i++)
		__rcu_txn_record_settle(&records[i]);
	/* A reader may still hold a proxy, which lives in the descriptor. */
	kfree_rcu(desc, rcu_head);
}
EXPORT_SYMBOL_GPL(__rcu_txn_commit_desc);
