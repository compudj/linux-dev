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
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/percpu.h>
#include <linux/rcu_txn.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "rcu_txn_internal.h"

/*
 * Resolve sections
 * ================
 *
 * A reader dereferences a proxy only within a resolve section, and
 * __rcu_txn_synchronize_resolve() waits for the sections in flight.  Together
 * they make a small RCU flavour, whose read-side critical sections are the
 * resolve sections alone, and whose writer waits without sleeping: a section
 * runs with interrupts disabled, so it is short.  A descriptor whose proxies
 * are all settled can then be reused once the wait returns, without a grace
 * period.
 *
 * A CPU counts its sections with a sequence count per nesting level, odd while
 * a section of that level is in flight.  Interrupts being disabled, only an
 * NMI, or an exception that acts as one, enters a section within another, and
 * it takes the next level.  A count therefore serves one section at a time, and
 * only its CPU writes it.
 *
 * A section, __rcu_txn_resolve_slow():
 *
 *	count odd;  smp_mb() (C);  load the slot again and resolve it;
 *	count even, with smp_store_release() (E)
 *
 * A wait, __rcu_txn_synchronize_resolve():
 *
 *	smp_mb() (D);  for each CPU and level, load the count with
 *	smp_load_acquire() (F) and, if it is odd, again (F) until it changes
 *
 * (C) and (D) are the full barriers of a store-buffering pattern, between a
 * section that loads a slot and a writer that settled the slot, then waits:
 * either the section loads the settled value, or the wait loads the count that
 * the section made odd, or a later one.
 *
 * (E) pairs with (F): what a section read through a proxy is ordered before
 * what the writer does once its wait returns, such as reusing the descriptor.
 *
 * A changed count means that the section found in flight is over: the next
 * section of that level starts only after it.  The wait thus waits for at most
 * one section per CPU and level, however many a CPU runs back to back.  A
 * count that wraps around while the wait samples it only makes the wait
 * longer.
 *
 * See Documentation/litmus-tests/rcu/RCU-txn+steal.litmus.
 */

/* A CPU's resolve sections. */
struct rcu_txn_resolve_cpu {
	unsigned int depth;		/* sections in flight */
	/* Per level: odd while a section is in flight. */
	unsigned long seq[RCU_TXN_RESOLVE_NEST];
};

static DEFINE_PER_CPU(struct rcu_txn_resolve_cpu, rcu_txn_resolve_cpu);

/* See rcu_txn_internal.h. */
unsigned int __rcu_txn_resolve_enter(void)
{
	struct rcu_txn_resolve_cpu *rc = this_cpu_ptr(&rcu_txn_resolve_cpu);
	unsigned int level = READ_ONCE(rc->depth);

	RCU_TXN_WARN_ON(!irqs_disabled());
	/*
	 * Take the level before its count, so that an NMI that enters a section
	 * in between takes the next one.
	 */
	WRITE_ONCE(rc->depth, level + 1);
	barrier();
	/* Deeper than NMIs nest: no count to make odd. */
	if (WARN_ON_ONCE(level >= RCU_TXN_RESOLVE_NEST))
		return RCU_TXN_RESOLVE_NEST;
	WRITE_ONCE(rc->seq[level], rc->seq[level] + 1);
	/*
	 * Full barrier (C) pairs with (D) of __rcu_txn_synchronize_resolve():
	 * the count is odd before the section loads the slot.
	 */
	smp_mb();
	return level;
}
EXPORT_SYMBOL_IF_KUNIT(__rcu_txn_resolve_enter);

/* See rcu_txn_internal.h. */
void __rcu_txn_resolve_exit(unsigned int level)
{
	struct rcu_txn_resolve_cpu *rc = this_cpu_ptr(&rcu_txn_resolve_cpu);

	RCU_TXN_WARN_ON(!irqs_disabled());
	if (likely(level < RCU_TXN_RESOLVE_NEST)) {
		/*
		 * Store-release (E) pairs with (F) of
		 * __rcu_txn_synchronize_resolve().
		 */
		smp_store_release(&rc->seq[level], rc->seq[level] + 1);
	}
	barrier();
	WRITE_ONCE(rc->depth, READ_ONCE(rc->depth) - 1);
}
EXPORT_SYMBOL_IF_KUNIT(__rcu_txn_resolve_exit);

/* See <linux/rcu_txn.h>. */
void *__rcu_txn_resolve_slow(void __rcu **slot, unsigned long tag)
{
	unsigned long flags;
	unsigned int level;
	void *v;

	/*
	 * raw_: a section takes no lock, and NMIs and tracers read slots
	 * as well.
	 */
	raw_local_irq_save(flags);
	level = __rcu_txn_resolve_enter();
	/*
	 * The proxy that the caller loaded before the section may belong to a
	 * descriptor that serves another transaction by now: load the slot
	 * again, and resolve what it holds.
	 */
	v = rcu_dereference_raw(*slot);
	if (rcu_txn_is_proxy(v, tag))
		v = __rcu_txn_proxy_resolve(__rcu_txn_untag(v, tag));
	__rcu_txn_resolve_exit(level);
	raw_local_irq_restore(flags);
	return v;
}
EXPORT_SYMBOL_GPL(__rcu_txn_resolve_slow);

/* See rcu_txn_internal.h. */
void __rcu_txn_synchronize_resolve(void)
{
	unsigned int level;
	int cpu;

	/* An NMI would wait for the section it interrupted. */
	WARN_ON_ONCE(in_nmi());
	/*
	 * Full barrier (D) pairs with (C) of __rcu_txn_resolve_enter(): the
	 * caller's settles are ordered before the counts it loads.
	 */
	smp_mb();
	for_each_possible_cpu(cpu) {
		struct rcu_txn_resolve_cpu *rc =
				per_cpu_ptr(&rcu_txn_resolve_cpu, cpu);

		for (level = 0; level < RCU_TXN_RESOLVE_NEST; level++) {
			/*
			 * Load-acquire (F) pairs with (E) of
			 * __rcu_txn_resolve_exit().
			 */
			unsigned long seq = smp_load_acquire(&rc->seq[level]);

			if (seq & 1)	/* in flight: wait until it changes */
				smp_cond_load_acquire(&rc->seq[level],
						      seq != VAL);
		}
	}
}
EXPORT_SYMBOL_IF_KUNIT(__rcu_txn_synchronize_resolve);

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

	__rcu_txn_assert_distinct_slots(records, nr);
	/* Install: readers still resolve to old, the selector being 0. */
	for (i = 0; i < nr; i++) {
		__rcu_txn_excl_slot_installable(&records[i]);
		__rcu_txn_record_install(&desc->group, &records[i]);
	}
	/* Flip: every proxy resolves to new. */
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
