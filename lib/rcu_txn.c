// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Single-writer RCU pseudo-transactions: the out-of-line paths
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * The engine is inline in <linux/rcu_txn.h>.  What lives here is what allocates
 * a transaction's descriptor, or takes it from a pool, the commit that installs
 * proxies, and the resolve sections in which readers read them.
 */

#include <kunit/visibility.h>
#include <linux/container_of.h>
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/lockdep.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/rcu_txn.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/stddef.h>
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

/*
 * Local transactions
 * ==================
 *
 * A local handle, rcu_txn_init_local(), takes its descriptor from a pool of its
 * CPU's, of RCU_TXN_LOCAL_POOL descriptors, and keeps preemption disabled until
 * its commit or cancel, which give the descriptor back to that same pool.  Such
 * a transaction allocates nothing, and its commit cannot fail.
 *
 * A descriptor of a pool is in one of four places:
 *
 *   - free, ready for a handle;
 *   - held by a handle, between its init and its commit or cancel;
 *   - settled: the commit of its handle installed proxies, and settled them;
 *   - in a batch, whose call_rcu() callback is pending or ran.
 *
 * Only the pool's CPU moves a descriptor, with interrupts disabled, so these
 * four places always hold the whole pool.  The callback of a batch only marks
 * it done: it moves no descriptor, so none is ever out of sight of the CPU.
 *
 * Once a commit settled its proxies, it queues the settled descriptors in a
 * batch that is idle, or done, with call_rcu_hurry().  A handle takes a free
 * descriptor if there is one, else the descriptors of the batches whose
 * callback ran, a grace period after their settles.  If there are none, it
 * steals the settled descriptors and those of every batch back: once
 * __rcu_txn_synchronize_resolve() returns, no reader can still read them.  A
 * batch stolen from stays queued, its rcu_head being so, until its callback
 * runs, and is then found done and empty.
 *
 * Each context of a CPU holds one handle at a time, so a CPU holds at most a
 * few of them, and a steal finds the rest of its pool.  A handle that finds
 * nothing is past that bound: it warns, and its commit fails.
 *
 * A steal busy-waits for resolve sections with interrupts disabled, through a
 * scan of every CPU: a burst of more commits than the pool holds within a
 * grace period pays one scan per pool's worth.
 *
 * See Documentation/litmus-tests/rcu/RCU-txn+steal.litmus and
 * RCU-txn+recycle.litmus.
 */

/* A descriptor of a pool, with room for RCU_TXN_LOCAL_CAP records. */
struct rcu_txn_local_desc {
	TRAILING_OVERLAP(struct rcu_txn_desc, desc, records,
		struct rcu_txn_record __records[RCU_TXN_LOCAL_CAP];
	);
	struct rcu_txn_local_desc *next;	/* in a list of the pool's */
	unsigned int cpu;			/* the pool's */
};

static_assert(offsetof(struct rcu_txn_local_desc, __records) ==
	      offsetof(struct rcu_txn_local_desc, desc.records));
/* With no room for a filter: never armed, under RCU_TXN_BLOOM_MIN records. */
static_assert(RCU_TXN_LOCAL_CAP < RCU_TXN_BLOOM_MIN);

enum rcu_txn_batch_state {
	RCU_TXN_BATCH_IDLE,		/* may be queued */
	RCU_TXN_BATCH_QUEUED,		/* its callback is pending */
	RCU_TXN_BATCH_DONE,		/* its callback ran */
};

/* Settled descriptors that wait for one grace period. */
struct rcu_txn_local_batch {
	struct rcu_head rcu;
	struct rcu_txn_local_desc *descs;	/* its CPU's to change, alone */
	int state;			/* the callback's only store: DONE */
};

/* A CPU's pool. */
struct rcu_txn_local_cpu {
	struct rcu_txn_local_desc *free;
	struct rcu_txn_local_desc *settled;	/* in no batch yet */
	unsigned int next_batch;	/* where to look for an idle batch */
	bool ready;			/* the free list is built */
	unsigned long steals;
	struct rcu_txn_local_batch batch[RCU_TXN_LOCAL_POOL];
	struct rcu_txn_local_desc descs[RCU_TXN_LOCAL_POOL];
};

static DEFINE_PER_CPU(struct rcu_txn_local_cpu, rcu_txn_local_cpu);

/*
 * The lists of a pool change with interrupts disabled, on its CPU alone: an
 * interrupt that changed them in the middle of a change would lose a
 * descriptor, or give it twice.
 */
static void rcu_txn_local_push(struct rcu_txn_local_desc **list,
			       struct rcu_txn_local_desc *ld)
{
	lockdep_assert_irqs_disabled();
	ld->next = *list;
	*list = ld;
}

/* Move every descriptor of @from to @to. */
static void rcu_txn_local_splice(struct rcu_txn_local_desc **to,
				 struct rcu_txn_local_desc **from)
{
	lockdep_assert_irqs_disabled();
	while (*from) {
		struct rcu_txn_local_desc *ld = *from;

		*from = ld->next;
		rcu_txn_local_push(to, ld);
	}
}

/* Build this CPU's free list, on first use: the pool is static. */
static void rcu_txn_local_build(struct rcu_txn_local_cpu *lc)
{
	unsigned int i;

	for (i = 0; i < RCU_TXN_LOCAL_POOL; i++) {
		struct rcu_txn_local_desc *ld = &lc->descs[i];

		ld->desc.slots = RCU_TXN_LOCAL_CAP;
		ld->desc.cap = RCU_TXN_LOCAL_CAP;
		ld->cpu = smp_processor_id();
		rcu_txn_local_push(&lc->free, ld);
	}
	lc->ready = true;
}

static void rcu_txn_local_batch_done(struct rcu_head *rcu)
{
	struct rcu_txn_local_batch *b =
			container_of(rcu, struct rcu_txn_local_batch, rcu);

	/*
	 * Store-release (G), a grace period after the settles of the batch's
	 * descriptors, pairs with the load-acquire (H) of
	 * rcu_txn_local_reap().
	 */
	smp_store_release(&b->state, RCU_TXN_BATCH_DONE);
}

/*
 * If the callback of @b ran, put its descriptors in the free list, and make it
 * idle.  Returns true if @b is idle then.
 */
static bool rcu_txn_local_reap(struct rcu_txn_local_cpu *lc,
			       struct rcu_txn_local_batch *b)
{
	/* Load-acquire (H) pairs with (G) of rcu_txn_local_batch_done(). */
	switch (smp_load_acquire(&b->state)) {
	case RCU_TXN_BATCH_IDLE:
		return true;
	case RCU_TXN_BATCH_DONE:
		rcu_txn_local_splice(&lc->free, &b->descs);
		WRITE_ONCE(b->state, RCU_TXN_BATCH_IDLE);
		return true;
	}
	return false;
}

/* Queue the settled descriptors in a batch, if one is idle. */
static void rcu_txn_local_queue(struct rcu_txn_local_cpu *lc)
{
	unsigned int i, n = lc->next_batch;

	lockdep_assert_irqs_disabled();
	for (i = 0; i < RCU_TXN_LOCAL_POOL; i++) {
		struct rcu_txn_local_batch *b = &lc->batch[n];

		n = (n + 1) % RCU_TXN_LOCAL_POOL;
		if (!rcu_txn_local_reap(lc, b))
			continue;
		b->descs = lc->settled;
		lc->settled = NULL;
		WRITE_ONCE(b->state, RCU_TXN_BATCH_QUEUED);
		/* Not lazy: a pool waits for these. */
		call_rcu_hurry(&b->rcu, rcu_txn_local_batch_done);
		lc->next_batch = n;
		return;
	}
}

/*
 * Take back every descriptor that waits for a grace period, without waiting
 * for one.  Each was settled on this CPU before the wait.
 */
static void rcu_txn_local_steal(struct rcu_txn_local_cpu *lc)
{
	struct rcu_txn_local_desc *stolen = NULL;
	unsigned int i;

	rcu_txn_local_splice(&stolen, &lc->settled);
	for (i = 0; i < RCU_TXN_LOCAL_POOL; i++)
		rcu_txn_local_splice(&stolen, &lc->batch[i].descs);
	if (!stolen)
		return;
	__rcu_txn_synchronize_resolve();
	rcu_txn_local_splice(&lc->free, &stolen);
	WRITE_ONCE(lc->steals, lc->steals + 1);
}

/*
 * Take a descriptor from this CPU's pool, with interrupts disabled.  NULL if
 * handles hold the whole pool.
 */
static struct rcu_txn_local_desc *
rcu_txn_local_take(struct rcu_txn_local_cpu *lc)
{
	struct rcu_txn_local_desc *ld;
	unsigned int i;

	lockdep_assert_irqs_disabled();
	if (unlikely(!lc->ready))
		rcu_txn_local_build(lc);
	for (i = 0; !lc->free && i < RCU_TXN_LOCAL_POOL; i++)
		rcu_txn_local_reap(lc, &lc->batch[i]);
	if (!lc->free)
		rcu_txn_local_steal(lc);
	ld = lc->free;
	if (ld)
		lc->free = ld->next;
	return ld;
}

/**
 * rcu_txn_init_local() - initialize a transaction handle that cannot fail
 * @t: the handle
 *
 * For a transaction of at most RCU_TXN_LOCAL_CAP records that must not fail
 * for want of memory, such as a list operation on a teardown path.  Its
 * descriptor comes from a pool of this CPU's, and preemption stays disabled
 * until rcu_txn_commit() or rcu_txn_cancel(), which give it back there.
 * Nothing is allocated: rcu_txn_record() cannot fail, and rcu_txn_commit()
 * returns 0.
 *
 * The descriptor of a commit that installed proxies goes back to the pool a
 * grace period later, through call_rcu().  A pool that runs out of descriptors
 * takes those that still wait for a grace period back at once: it waits for
 * the readers that may still read them, which do so with interrupts disabled,
 * busy-waiting with interrupts disabled itself.
 *
 * Between init and commit or cancel, the writer must not sleep, nor take a
 * lock: it may already hold one of any kind.  Each context of a CPU, task,
 * softirq or hardirq, holds one local handle at a time.
 *
 * A record beyond RCU_TXN_LOCAL_CAP warns, and puts the handle in the OOM
 * state: its commit then publishes nothing, and returns -ENOMEM.
 *
 * Context: Any but NMI.  Disables preemption until commit or cancel.
 */
void rcu_txn_init_local(struct rcu_txn *t)
{
	struct rcu_txn_local_desc *ld;
	unsigned long flags;

	rcu_txn_init(t, GFP_NOWAIT);	/* unused: the handle never grows */
	t->local = true;
	preempt_disable();		/* commit or cancel enables it again */
	/* An NMI may have interrupted this CPU in the middle of a change. */
	if (WARN_ON_ONCE(in_nmi())) {
		t->state = RCU_TXN_OOM;
		return;
	}
	local_irq_save(flags);
	ld = rcu_txn_local_take(this_cpu_ptr(&rcu_txn_local_cpu));
	local_irq_restore(flags);
	/* More handles of this CPU's than its pool holds. */
	if (WARN_ON_ONCE(!ld)) {
		t->state = RCU_TXN_OOM;
		return;
	}
	__rcu_txn_group_init(&ld->desc.group);
	t->desc = &ld->desc;
	t->records = ld->desc.records;
	t->cap = RCU_TXN_LOCAL_CAP;
}
EXPORT_SYMBOL_GPL(rcu_txn_init_local);

static struct rcu_txn_local_desc *rcu_txn_local_of(struct rcu_txn_desc *desc)
{
	struct rcu_txn_local_desc *ld =
			container_of(desc, struct rcu_txn_local_desc, desc);

	/* Its handle kept preemption disabled since it took it. */
	RCU_TXN_WARN_ON(ld->cpu != smp_processor_id());
	return ld;
}

/* See <linux/rcu_txn.h>. */
void __rcu_txn_local_put(struct rcu_txn_desc *desc)
{
	struct rcu_txn_local_desc *ld = rcu_txn_local_of(desc);
	unsigned long flags;

	local_irq_save(flags);
	rcu_txn_local_push(&this_cpu_ptr(&rcu_txn_local_cpu)->free, ld);
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(__rcu_txn_local_put);

/* See rcu_txn_internal.h. */
void __rcu_txn_local_retire(struct rcu_txn_desc *desc)
{
	struct rcu_txn_local_desc *ld = rcu_txn_local_of(desc);
	struct rcu_txn_local_cpu *lc;
	unsigned long flags;

	local_irq_save(flags);
	lc = this_cpu_ptr(&rcu_txn_local_cpu);
	rcu_txn_local_push(&lc->settled, ld);
	rcu_txn_local_queue(lc);
	local_irq_restore(flags);
}
EXPORT_SYMBOL_IF_KUNIT(__rcu_txn_local_retire);

/* The length of @ld's list, or more than a pool holds if it is longer. */
static unsigned int rcu_txn_local_len(struct rcu_txn_local_desc *ld)
{
	unsigned int n = 0;

	for (; ld && n <= RCU_TXN_LOCAL_POOL; ld = ld->next)
		n++;
	return n;
}

/* See rcu_txn_internal.h. */
unsigned int __rcu_txn_local_count(void)
{
	struct rcu_txn_local_cpu *lc;
	unsigned long flags;
	unsigned int n, i;

	local_irq_save(flags);
	lc = this_cpu_ptr(&rcu_txn_local_cpu);
	if (unlikely(!lc->ready))
		rcu_txn_local_build(lc);
	n = rcu_txn_local_len(lc->free) + rcu_txn_local_len(lc->settled);
	for (i = 0; i < RCU_TXN_LOCAL_POOL; i++)
		n += rcu_txn_local_len(lc->batch[i].descs);
	local_irq_restore(flags);
	return n;
}
EXPORT_SYMBOL_IF_KUNIT(__rcu_txn_local_count);

/* The steals of every pool so far. */
unsigned long __rcu_txn_local_steals(void)
{
	unsigned long steals = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		steals += READ_ONCE(per_cpu_ptr(&rcu_txn_local_cpu, cpu)->steals);
	return steals;
}
EXPORT_SYMBOL_IF_KUNIT(__rcu_txn_local_steals);

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

	/* A local handle has RCU_TXN_LOCAL_CAP records, and no more. */
	if (WARN_ON_ONCE(t->local))
		goto fail;
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
void __rcu_txn_commit_desc(struct rcu_txn_desc *desc, unsigned int nr,
			   bool local)
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
	if (local)
		__rcu_txn_local_retire(desc);
	else
		kfree_rcu(desc, rcu_head);
}
EXPORT_SYMBOL_GPL(__rcu_txn_commit_desc);
