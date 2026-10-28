// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Single-writer RCU pseudo-transactions: the out-of-line paths
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * The engine is inline in <linux/rcu_txn.h>.  This file holds the code that
 * allocates a transaction's descriptor or takes it from a pool.  It also holds
 * the commit that installs proxies, and the resolve sections in which readers
 * read them.
 */

#include <kunit/visibility.h>
#include <linux/container_of.h>
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/overflow.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/rcu_txn.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/srcu.h>
#include <linux/stddef.h>
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
 *
 * See Documentation/litmus-tests/rcu/RCU-txn+steal.litmus.
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
 * Local transactions
 * ==================
 *
 * A local handle, initialized by rcu_txn_init_local(), takes its descriptor
 * from its CPU's pool of RCU_TXN_LOCAL_POOL descriptors.  It keeps preemption
 * disabled until its commit or cancel.  The commit or cancel gives the
 * descriptor back to that same pool.  Such a transaction allocates nothing,
 * so its commit cannot fail for want of memory.
 *
 * A descriptor of a pool is free, held by a handle, settled, or in a batch.
 * A batch is a set of settled descriptors that wait for one grace period.
 * These functions move a descriptor:
 *
 *   free -> held       rcu_txn_init_local().
 *   held -> free       __rcu_txn_local_put(), for a cancel or a commit that
 *                      installed no proxy.
 *   held -> settled    __rcu_txn_local_retire(), for a commit that installed
 *                      proxies, once it settled them.
 *   settled -> batch   rcu_txn_local_queue(), if a batch is idle or done.  It
 *                      queues the batch with call_rcu_hurry().
 *   batch -> free      rcu_txn_local_reap(), once the callback marked the
 *                      batch done.  This is a grace period after the settles.
 *   settled -> free,   rcu_txn_local_steal(), when a handle finds no free
 *   batch -> free      descriptor and no done batch holds one.  It waits in
 *                      __rcu_txn_synchronize_resolve(), which spins with
 *                      interrupts disabled until the resolve sections in
 *                      flight end.  Then no reader can still read these
 *                      descriptors.
 *
 * Only the pool's CPU moves a descriptor, with interrupts disabled.  So these
 * four states always hold the whole pool.  The callback of a batch moves no
 * descriptor: it only marks the batch done.  It may run on another CPU, as
 * with rcu_nocbs, and a hardirq may interrupt it.  Neither leaves a descriptor
 * in transit.
 *
 * A batch stolen from stays queued until its callback runs, because its
 * rcu_head is still queued.  The batch is then found done and empty.
 *
 * Each context of a CPU holds one handle at a time.  So a CPU holds at most a
 * few handles, and a steal finds the rest of its pool.  A handle that finds
 * nothing is past that bound: it warns, and its commit fails.
 *
 * A burst of more commits than the pool holds within a grace period pays one
 * steal per pool's worth.
 *
 * See Documentation/litmus-tests/rcu/RCU-txn+steal.litmus and
 * RCU-txn+recycle.litmus.
 */

/* A descriptor of a pool, with room for RCU_TXN_LOCAL_CAP records. */
struct rcu_txn_local_desc {
	TRAILING_OVERLAP(struct rcu_txn_desc, desc, records,
		struct rcu_txn_record __records[RCU_TXN_LOCAL_CAP];
	);
	struct rcu_txn_local_desc *next;	/* in one of the pool's lists */
	unsigned int cpu;			/* the CPU of the pool */
};

static_assert(offsetof(struct rcu_txn_local_desc, __records) ==
	      offsetof(struct rcu_txn_local_desc, desc.records));

enum rcu_txn_batch_state {
	RCU_TXN_BATCH_IDLE,		/* may be queued */
	RCU_TXN_BATCH_QUEUED,		/* its callback is pending */
	RCU_TXN_BATCH_DONE,		/* its callback ran */
};

/* Settled descriptors that wait for one grace period. */
struct rcu_txn_local_batch {
	struct rcu_head rcu;
	struct rcu_txn_local_desc *descs;	/* changed by its CPU only */
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

static void rcu_txn_local_push(struct rcu_txn_local_desc **list,
			       struct rcu_txn_local_desc *ld)
{
	ld->next = *list;
	*list = ld;
}

/* Move every descriptor of @from to @to. */
static void rcu_txn_local_splice(struct rcu_txn_local_desc **to,
				 struct rcu_txn_local_desc **from)
{
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
	 * This runs a grace period after the settles of the batch's
	 * descriptors.  Store-release (D) pairs with the load-acquire (C) of
	 * rcu_txn_local_reap().
	 */
	smp_store_release(&b->state, RCU_TXN_BATCH_DONE);
}

/*
 * If the callback of @b ran, move its descriptors to the free list and make @b
 * idle.  Return true if @b is idle.
 */
static bool rcu_txn_local_reap(struct rcu_txn_local_cpu *lc,
			       struct rcu_txn_local_batch *b)
{
	/* Load-acquire (C) pairs with (D) of rcu_txn_local_batch_done(). */
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
 * for one.  Wait for the resolve sections in flight instead.  Each descriptor
 * was settled on this CPU before that wait.
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
 * Take a descriptor from this CPU's pool.  The caller disables interrupts.
 * Return NULL if handles hold the whole pool.
 */
static struct rcu_txn_local_desc *
rcu_txn_local_take(struct rcu_txn_local_cpu *lc)
{
	struct rcu_txn_local_desc *ld;
	unsigned int i;

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
 * Use it for a transaction that must not fail for want of memory, such as a
 * list operation on a teardown path.  The transaction holds at most
 * RCU_TXN_LOCAL_CAP records.  Its descriptor comes from this CPU's pool.
 * Preemption stays disabled until rcu_txn_commit() or rcu_txn_cancel(), which
 * give the descriptor back to the pool.  Nothing is allocated, so
 * rcu_txn_record() cannot fail and rcu_txn_commit() returns 0.
 *
 * A pool that runs out of descriptors may busy-wait with interrupts disabled:
 * see "Local transactions" above.
 *
 * Between init and commit or cancel, preemption is disabled, so the writer
 * must not sleep.  It may take or hold a lock that never sleeps, such as a raw
 * spinlock or a bit spinlock.  A CPU's task, softirq and hardirq contexts may
 * each hold one local handle at a time.
 *
 * A record beyond RCU_TXN_LOCAL_CAP warns and puts the handle in the OOM
 * state.  Its commit then publishes nothing, and returns -ENOMEM.
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
	/* An NMI may have interrupted a change to this CPU's pool. */
	if (WARN_ON_ONCE(in_nmi())) {
		t->state = RCU_TXN_OOM;
		return;
	}
	local_irq_save(flags);
	ld = rcu_txn_local_take(this_cpu_ptr(&rcu_txn_local_cpu));
	local_irq_restore(flags);
	/* This CPU's handles hold its whole pool. */
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

	/* Its handle kept preemption disabled since it took the descriptor. */
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

	/* A local handle has RCU_TXN_LOCAL_CAP records, and no more. */
	if (WARN_ON_ONCE(t->local))
		goto fail;
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
	/* Commit: the flip.  Every proxy resolves to new. */
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
