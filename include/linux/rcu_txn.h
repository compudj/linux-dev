/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Single-writer RCU pseudo-transactions
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */
#ifndef _LINUX_RCU_TXN_H
#define _LINUX_RCU_TXN_H

/*
 * Single-writer RCU pseudo-transactions switch a set of RCU-protected
 * pointers from their old to their new targets with a single store.  These
 * pointers are called slots.  Concurrent RCU readers see the switch as
 * atomic.  At any instant, every slot of the set resolves to its old target,
 * or every slot to its new one.
 *
 * The slots are the edges of the data structure that an update re-points.
 * The writer records each slot with its old and new targets.
 *
 * rcu_txn_commit() then publishes the records in three steps:
 *
 *   1. Install.  Store into each slot a tagged pointer to its record.  Such
 *      a record is called a proxy.  Readers recognise it by the tag.  All
 *      proxies of a transaction share one selector.  A proxy resolves to
 *      the slot's old target while the selector is 0, and to its new target
 *      once the selector is 1.  The selector is in a struct rcu_txn_group,
 *      called the flip group of the transaction.
 *   2. Commit.  Store 1 to the selector.  This single store is called the
 *      flip.  Every proxy of the transaction then resolves to its new
 *      target.
 *   3. Settle.  Store its new target into each slot, over the proxy.
 *
 * The records are freed after a grace period.
 *
 * The embedding data structure is responsible for:
 *   - excluding concurrent writers from the slots a transaction commits;
 *   - choosing the tag by which its readers recognise a proxy, such as a
 *     spare low bit or a reserved type code in an already-tagged pointer;
 *   - never storing a live value that carries this tag;
 *   - routing its readers' slot loads through rcu_txn_resolve(), within an
 *     RCU read-side critical section.
 *
 * Store-release / load-acquire pairs
 * ----------------------------------
 * Each step of the commit is a store-release.  Each one pairs with a load
 * of the reader:
 *
 *   - Install.  The store-release of the tagged proxy into the slot
 *     (rcu_assign_pointer()) pairs with the reader's rcu_dereference() of
 *     that slot.  It orders the writer's earlier stores before the reader's
 *     loads through the proxy.  These earlier stores are:
 *       - the record's {old, new, group};
 *       - the selector's initialization to 0;
 *       - the new data being published, which the embedder builds before
 *         the commit.
 *     A reader that finds the proxy thus sees the new data built, whatever
 *     the selector holds.
 *
 *   - Commit.  The store-release of the selector (0 -> 1) pairs with the
 *     load-acquire of the selector in __rcu_txn_proxy_resolve().  It orders
 *     every install of the transaction before the flip.  See "Ordering"
 *     below for what a reader gets from it.
 *
 *   - Settle.  The store-release of the new target into the slot pairs with
 *     the reader's rcu_dereference() of that slot.  This is the usual RCU
 *     publication guarantee, for a reader that finds the new target settled.
 *
 * A single-record commit installs no proxy: it is the settle store alone.
 *
 * Ordering
 * --------
 * The selector is one word.  It is written once (0 -> 1) and never back.
 * Take a reader that loads two slots of one transaction, with the second
 * load ordered after the first.  If the first load resolves to its new
 * target, the second finds that slot's proxy or its new target, never its
 * old one.  Over several slots, the reader sees old...old,new...new, never
 * new-then-old.  With a fixed read order, embedders can rely on this
 * instead of a per-traversal snapshot.
 *
 * The guarantee holds only when one of these orders the second load after
 * the first:
 *   - an address dependency: the address of the second slot comes from the
 *     value loaded from the first, through rcu_dereference();
 *   - an smp_rmb() between the two rcu_txn_dereference() calls;
 *   - a load-acquire of the first slot, with rcu_txn_dereference_acquire().
 * An smp_load_acquire() of some other word between the two loads is not
 * enough.  It orders the loads after it, not the load before it.
 *
 * Example with an address dependency.  A reader follows a forward edge of a
 * transaction to its new target.  From that node, it follows a back edge of
 * the same transaction.  It finds the back edge's new target.
 *
 * Example with none of the three.  A reader re-reads a pointer it cached
 * earlier, or walks prev-then-next between two slots it reached
 * independently.  On weakly-ordered hardware, its two loads may be
 * reordered.  It can find the new target in the slot it reads first, and
 * the old target in the slot it reads next.
 *
 * Readers and reclaim
 * -------------------
 * rcu_txn_commit() reclaims the transaction descriptor with kfree_rcu().  A
 * reader must therefore load a slot and resolve what it loaded within one RCU
 * read-side critical section.
 *
 * Allocation
 * ----------
 * rcu_txn_reserve() allocates the descriptor.  So does an rcu_txn_record()
 * that finds the handle full.  Both use the gfp_t given to rcu_txn_init().
 * A writer that records under a spinlock has two choices.  It initializes
 * the handle with a non-sleeping gfp_t, or it reserves its record bound
 * before it takes the lock.  Under a raw spinlock, only the reserve works:
 * the allocator cannot be called there.  On PREEMPT_RT, this also holds
 * under a bit spinlock.
 *
 * rcu_txn_commit() and rcu_txn_cancel() allocate nothing, do not sleep, and
 * may be called under a raw or a bit spinlock.
 */

#include <linux/bug.h>
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/gfp_types.h>
#include <linux/rcupdate.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>

/*
 * First capacity of an unreserved handle.  With its header, such a descriptor
 * fits a 256-byte allocation.  A transaction that records more should
 * rcu_txn_reserve() its bound rather than grow to it.
 */
#define RCU_TXN_CAP	4

/*
 * RCU pseudo-transaction (struct rcu_txn)
 * =======================================
 *
 * A growable transaction over a set of slots.  The handle is a small on-stack
 * object the embedder declares and initializes with rcu_txn_init().  Nothing
 * is allocated before the first record() or reserve(), so init cannot fail.
 *
 *   init() --> PREPARE --commit() or cancel()--> DONE
 *                 |                               ^
 *                 | reserve() or record()         |
 *                 | cannot allocate               |
 *                 v                               |
 *                OOM ---commit() or cancel()------+
 *
 * record() appends a record {slot, old, new} to the transaction's descriptor.
 * It installs nothing, so the descriptor may still move as it grows.  The
 * record set is frozen once commit() installs the proxies.  Records must
 * target pairwise-distinct slots: see rcu_txn_record().
 *
 * A handle ends in commit() or in cancel().  commit() publishes the record
 * set, and cancel() drops it.  Either one consumes the handle.  The handle
 * must be initialized again before it is reused.
 *
 * OOM is sticky.  Once a reserve() or a record() has failed to allocate, later
 * ones do nothing and commit() publishes nothing and returns -ENOMEM.  An
 * embedder may therefore ignore their return values and test only commit()'s.
 *
 * Each record carries its own tag: see rcu_txn_record().
 */

enum rcu_txn_state {
	RCU_TXN_PREPARE = 0,
	RCU_TXN_OOM,			/* sticky: commit() returns -ENOMEM */
	/*
	 * Terminal: commit() or cancel() consumed the handle.  Using it again
	 * without initializing it warns.
	 */
	RCU_TXN_DONE,
};

/*
 * The selector shared by the records of one transaction.  It indexes
 * rcu_txn_record::ptr[]: 0 selects the old target, 1 the new one.  See
 * "Store-release / load-acquire pairs" above.
 */
struct rcu_txn_group {
	unsigned long selector;
};

/*
 * Records are aligned on RCU_TXN_RECORD_ALIGN bytes.  The low bits of a
 * record's address (RCU_TXN_TAG_MASK) are thus free for the tag that marks
 * an installed proxy.
 */
#define RCU_TXN_RECORD_ALIGN	16
#define RCU_TXN_TAG_MASK	(RCU_TXN_RECORD_ALIGN - 1UL)

struct rcu_txn_record {
	void *ptr[2];			/* [0] old, [1] new; selector-indexed */
	struct rcu_txn_group *group;
	void __rcu **slot;		/* install / settle target */
	/*
	 * The slot's proxy tag: see rcu_txn_record().  Per record, so that
	 * slots tagged differently (a 0xF type code, a list's bit 0) can share
	 * one transaction.
	 */
	unsigned long tag;
} __aligned(RCU_TXN_RECORD_ALIGN);

/*
 * Transaction descriptor: the single allocation that carries a committed
 * transaction across its grace period.  It holds:
 *   - the flip group that every installed proxy reads;
 *   - the rcu_head that defers its reclaim;
 *   - the records, which are the proxies.
 * The first record() or reserve() allocates it.
 */
struct rcu_txn_desc {
	struct rcu_txn_group group;		/* read by installed proxies */
	struct rcu_head rcu_head;		/* deferred-free handle */
	unsigned int cap;			/* records it can hold */
	struct rcu_txn_record records[] __counted_by(cap);
};

/*
 * The records' offset and size are multiples of RCU_TXN_RECORD_ALIGN.
 * rcu_txn_desc_alloc() relies on this for their alignment.
 */
static_assert(!(offsetof(struct rcu_txn_desc, records) % RCU_TXN_RECORD_ALIGN));
static_assert(!(sizeof(struct rcu_txn_record) % RCU_TXN_RECORD_ALIGN));

/*
 * Transaction handle: a small on-stack object, private to one writer.
 */
struct rcu_txn {
	struct rcu_txn_record *records;	/* @desc's, NULL until allocated */
	struct rcu_txn_desc *desc;	/* NULL until first allocated */
	unsigned int nr;
	unsigned int cap;
	gfp_t gfp;			/* for the descriptor */
	enum rcu_txn_state state;
	bool disjoint;			/* declared slot-disjoint */
};

/*
 * Reader side
 * ===========
 *
 * A reader loads a transacted slot with rcu_dereference() and passes the value
 * to rcu_txn_resolve().  rcu_txn_dereference() does both.
 */

/**
 * rcu_txn_is_proxy() - is a slot's value an installed proxy?
 * @v: value loaded from a transacted slot
 * @tag: the slot's proxy tag
 *
 * A live value that an embedder stores in a transacted slot must not carry
 * all of @tag's bits.  See rcu_txn_record().
 *
 * Return: true if @v carries all of @tag's bits.
 */
static inline bool rcu_txn_is_proxy(const void *v, unsigned long tag)
{
	return ((unsigned long)v & tag) == tag;
}

/*
 * Recover the record's address from a slot value that holds an installed proxy.
 *
 * rcu_txn_is_proxy() has shown that every tag bit is set in @v.  Subtracting
 * the tag is then the same as masking it off.  The compiler can fold a
 * constant @tag into the displacement of the loads that follow.  A mask would
 * add one operation to the reader's chain of dependent loads.
 */
static inline struct rcu_txn_record *__rcu_txn_untag(void *v, unsigned long tag)
{
	return (struct rcu_txn_record *)((unsigned long)v - tag);
}

/*
 * Resolve a proxy to its current target.
 *
 * @proxy must come from an rcu_dereference() of the slot that holds it.  What
 * orders the loads below is described under "Store-release / load-acquire
 * pairs" at the top of this file.
 *
 * The selector indexes proxy->ptr[] directly, so resolution is a data
 * dependency rather than a conditional branch.
 */
static inline void *__rcu_txn_proxy_resolve(const struct rcu_txn_record *proxy)
{
	/*
	 * Load-acquire (A) pairs with the store-release (B) of
	 * __rcu_txn_group_commit().
	 */
	return proxy->ptr[smp_load_acquire(&proxy->group->selector)];
}

/**
 * rcu_txn_resolve() - resolve a value loaded from a transacted slot
 * @v: value returned by rcu_dereference() on the slot
 * @tag: the slot's proxy tag
 *
 * Return a plain value as is.  Resolve an installed proxy through its flip
 * group's selector.  It resolves to the slot's old target until the
 * transaction commits, and to its new target from then on.
 *
 * Context: RCU read-side critical section, the one @v was loaded in.
 * Return: the slot's target.
 */
static inline void *rcu_txn_resolve(void *v, unsigned long tag)
{
	if (likely(!rcu_txn_is_proxy(v, tag)))
		return v;
	return __rcu_txn_proxy_resolve(__rcu_txn_untag(v, tag));
}

/**
 * rcu_txn_dereference_check() - load and resolve a transacted pointer
 * @p: the RCU-protected pointer, a transacted slot
 * @tag: the slot's proxy tag
 * @c: condition under which the load is also legal, as for
 *     rcu_dereference_check()
 *
 * Return: the slot's target, with the type @p points to.
 */
#define rcu_txn_dereference_check(p, tag, c)				\
({									\
	void *__rcu_txn_v = rcu_dereference_check(p, c);		\
									\
	(typeof(*(p)) __force __kernel *)rcu_txn_resolve(__rcu_txn_v, tag); \
})

/**
 * rcu_txn_dereference() - load and resolve a transacted pointer
 * @p: the RCU-protected pointer, a transacted slot
 * @tag: the slot's proxy tag
 *
 * Load @p with rcu_dereference(), then resolve it with rcu_txn_resolve().
 *
 * A writer does not need it.  It excludes the slot's other writers, so the
 * slot holds no proxy outside its own rcu_txn_commit().  It loads the slot
 * with rcu_dereference_protected() and does not resolve.
 *
 * Context: RCU read-side critical section.
 * Return: the slot's target, with the type @p points to.
 */
#define rcu_txn_dereference(p, tag)	rcu_txn_dereference_check(p, tag, 0)

/**
 * rcu_txn_dereference_acquire() - load a transacted pointer with a load-acquire
 * @p: the RCU-protected pointer, a transacted slot
 * @tag: the slot's proxy tag
 *
 * Like rcu_txn_dereference(), but load @p with smp_load_acquire().  The
 * reader's later loads are then ordered after this one.
 *
 * Use it when the reader loads several slots, and does not reach each one
 * through the value of the one before.  The two links of one node are an
 * example.  The reader then uses it for every such load but the last.  See
 * "Ordering" at the top of this file.
 *
 * Context: RCU read-side critical section.
 * Return: the slot's target, with the type @p points to.
 */
#define rcu_txn_dereference_acquire(p, tag)				\
({									\
	void __rcu **__rcu_txn_s = rcu_txn_slot(&(p));			\
	void *__rcu_txn_v;						\
									\
	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),				\
			 "suspicious rcu_txn_dereference_acquire() usage"); \
	/* Order the reader's later loads after this one. */		\
	__rcu_txn_v = unrcu_pointer(smp_load_acquire(__rcu_txn_s));	\
	__rcu_txn_v = rcu_txn_resolve(__rcu_txn_v, tag);		\
	(typeof(*(p)) __force __kernel *)__rcu_txn_v;			\
})

/**
 * rcu_txn_slot() - convert the address of an RCU-protected pointer to a slot
 * @pp: address of an __rcu pointer, of any type
 *
 * This API takes every slot as a ``void __rcu **``.  Convert the address
 * of an embedder's typed pointer to that type.  Sparse first checks that the
 * pointer is __rcu.
 *
 * Return: @pp as a ``void __rcu **``.
 */
#define rcu_txn_slot(pp)						\
({									\
	typeof(pp) __rcu_txn_pp = (pp);					\
									\
	rcu_check_sparse(*__rcu_txn_pp, __rcu);				\
	(void __rcu **)__rcu_txn_pp;					\
})

/*
 * Record primitives
 * =================
 *
 * What the inline paths need: record(), and the commit of a single record.  A
 * record is installed and a group committed in lib/rcu_txn.c: see
 * __rcu_txn_commit_desc().
 */

/*
 * Set a record's {old, new, slot, tag}.  Install sets its group.
 */
static inline void __rcu_txn_record_set(struct rcu_txn_record *r,
					void __rcu **slot, void *old_ptr,
					void *new_ptr, unsigned long tag)
{
	r->ptr[0] = old_ptr;
	r->ptr[1] = new_ptr;
	r->slot = slot;
	r->tag = tag;
}

/*
 * Settle record @r's slot: store its new target there, over the proxy.
 */
static inline void __rcu_txn_record_settle(struct rcu_txn_record *r)
{
	/* Store-release of slot pairs with rcu_dereference(). */
	rcu_assign_pointer(*r->slot, r->ptr[1]);
}

/*
 * Writer side
 * ===========
 *
 * The handle's contract is stated under "RCU pseudo-transaction" above.
 */

/**
 * rcu_txn_init() - initialize a transaction handle
 * @t: the handle
 * @gfp: allocation flags for the transaction's descriptor
 *
 * Allocates nothing, so it cannot fail.  @gfp is used by rcu_txn_reserve()
 * and by an rcu_txn_record() that finds the handle full.
 */
static inline void rcu_txn_init(struct rcu_txn *t, gfp_t gfp)
{
	memset(t, 0, sizeof(*t));
	t->state = RCU_TXN_PREPARE;
	t->gfp = gfp;
}

/*
 * Move the handle's records to a new descriptor holding at least @cap of them.
 * PREPARE only.  Return false if it cannot allocate.  The handle is then in
 * the RCU_TXN_OOM state.
 */
bool __rcu_txn_resize(struct rcu_txn *t, unsigned int cap);

/**
 * rcu_txn_reserve() - give the handle a descriptor for a number of records
 * @t: the handle
 * @cap: number of records the descriptor must hold
 *
 * The reserve is optional: record() grows the descriptor on demand.  An
 * embedder whose record count is bounded can reserve that bound before it
 * takes its locks.  It may sleep there, and a failure is easy to handle.
 * Every later record() then appends without allocating and cannot fail.
 *
 * Reserving fewer records than the transaction records later is correct.
 * It only costs the grow that the reserve is meant to avoid.
 *
 * Call after init and before commit.  A handle that already buffers records
 * grows to fit @cap.
 *
 * Context: May sleep if the handle's gfp_t allows it.
 * Return: true, or false if it cannot allocate, which is sticky.
 */
static inline bool rcu_txn_reserve(struct rcu_txn *t, unsigned int cap)
{
	if (unlikely(t->state != RCU_TXN_PREPARE)) {
		/* OOM is sticky.  DONE: commit() or cancel() consumed it. */
		WARN_ON_ONCE(t->state != RCU_TXN_OOM);
		return false;
	}
	if (t->records && cap <= t->cap)
		return true;		/* already large enough */
	return __rcu_txn_resize(t, cap);
}

/* Free a descriptor that installed no proxy: see lib/rcu_txn.c. */
void __rcu_txn_free_desc(struct rcu_txn_desc *desc);

/*
 * Release the handle's descriptor, if it has one.  None of its proxies was
 * installed, so no grace period is owed.  The free is still deferred: see
 * __rcu_txn_free_desc().
 */
static inline void __rcu_txn_release_desc(struct rcu_txn *t)
{
	if (t->desc)
		__rcu_txn_free_desc(t->desc);
}

/*
 * Mark the handle consumed by commit() or by cancel().  It no longer owns a
 * descriptor.  It must be initialized again before it is reused.
 */
static inline void __rcu_txn_done(struct rcu_txn *t)
{
	t->desc = NULL;
	t->records = NULL;
	t->nr = 0;
	t->state = RCU_TXN_DONE;
}

/*
 * Return this transaction's record for @slot, or NULL.  The scan is linear:
 * O(nr) per call and O(nr^2) to build a write set.  The next record() may
 * grow the descriptor and move the records.  Use the returned pointer before
 * recording again.
 */
static inline struct rcu_txn_record *__rcu_txn_scan(const struct rcu_txn *t,
						    void __rcu **slot)
{
	unsigned int i;

	for (i = 0; i < t->nr; i++)
		if (t->records[i].slot == slot)
			return &t->records[i];
	return NULL;
}

/*
 * Grow a full handle: double its capacity, or give an unreserved handle its
 * first RCU_TXN_CAP records.  Return false if it cannot allocate.
 *
 * This is out of line.  With this path in its body, the compiler would not
 * inline rcu_txn_record().  Every recorded edge would then pay a call for an
 * append of five stores.
 */
bool __rcu_txn_grow(struct rcu_txn *t);

/**
 * rcu_txn_record() - record one edge of a transaction
 * @t: the handle
 * @slot: the slot to switch
 * @old_ptr: the slot's current target
 * @new_ptr: the target the transaction gives it
 * @tag: the slot's proxy tag
 *
 * Record that the transaction switches @slot from @old_ptr to @new_ptr.
 * @tag is OR'd into the proxy value installed in @slot.  The slot's readers
 * recognise the proxy by it.  It is for instance a reserved type code, or a
 * list's bit 0.  It must be within RCU_TXN_TAG_MASK.
 *
 * Neither @old_ptr nor @new_ptr may carry all of @tag's bits.  After settle,
 * the slot holds @new_ptr directly.  If @new_ptr carried them, every later
 * reader would resolve it as a proxy, and dereference a wrong address.
 *
 * Call this in PREPARE only, before commit().
 *
 * Records must target pairwise-distinct slots.  record() does not look for
 * an earlier record of the same slot.  A slot recorded twice gets two
 * proxies, and both are settled in record order.  The earlier edit is then
 * lost.  Use rcu_txn_record_chain() when the same slot may be written twice.
 *
 * Context: The caller excludes other writers of @slot.  May sleep if the
 * handle is full and its gfp_t allows it.
 * Return: true, or false if it cannot allocate, which is sticky.
 */
static inline bool rcu_txn_record(struct rcu_txn *t, void __rcu **slot,
				  void *old_ptr, void *new_ptr,
				  unsigned long tag)
{
	struct rcu_txn_record *r;

	if (unlikely(t->state != RCU_TXN_PREPARE)) {
		/* OOM is sticky.  DONE: commit() or cancel() consumed it. */
		WARN_ON_ONCE(t->state != RCU_TXN_OOM);
		return false;
	}
	if (unlikely(t->nr == t->cap) && !__rcu_txn_grow(t))
		return false;			/* OOM, now sticky */
	r = &t->records[t->nr++];
	__rcu_txn_record_set(r, slot, old_ptr, new_ptr, tag);
	return true;
}

/*
 * Read-your-own-writes pair (rcu_txn_load() / rcu_txn_record_chain())
 * ====================================================================
 *
 * rcu_txn_record() requires pairwise-distinct slots.  This suits an embedder
 * whose slots are distinct by construction, such as a list operation that
 * records two edges.  It pays no scan per store.
 *
 * Other embedders may write the same slot twice in one transaction.  For
 * example, deleting two adjacent nodes of a list in one transaction writes
 * the predecessor's next slot twice.  The first store skips one node, the
 * second skips the other.  The second store must update the pending record,
 * not add a second one.  This pair does that:
 *
 *     old = rcu_txn_load(t, slot, TAG);
 *     rcu_txn_record_chain(t, slot, old, f(old), TAG);
 *
 * The transaction then keeps at most one record per slot.  There is no read
 * set: a load records nothing and is not validated, since a single writer has
 * nothing to validate against.
 *
 * Scope.  One record per slot is not enough for every embedder.  A list
 * computes a new next pointer from a neighbour's pointer.  If it loads that
 * neighbour's slot directly, it misses a write pending on it.  Such an
 * embedder must also traverse with rcu_txn_load().
 *
 * After an OOM, the pair no longer reads its own writes.  The failed record()
 * appended nothing, so a later load() of that slot returns the committed
 * value.  commit() then publishes nothing.  But an embedder may change state
 * outside the transaction as it records.  It must not derive that state from
 * such a load.
 */

/**
 * rcu_txn_declare_disjoint() - declare the write set slot-disjoint
 * @t: the handle
 *
 * Declare that no slot is recorded twice.  rcu_txn_load() and
 * rcu_txn_record_chain() then skip their scan of the records so far.  This
 * saves a little from the second record on.
 *
 * Distinctness is about slot addresses, not about the nodes edited.  Two
 * operations on distinct nodes may write the same slot.  The deletion of two
 * adjacent nodes of a list does.
 *
 * Recording a slot twice on such a handle breaks the rule of rcu_txn_record(),
 * with the consequence stated there.
 *
 * Call after init and before the first record().  On a handle whose reserve()
 * failed it does nothing: the failure is sticky, and commit() reports it.
 */
static inline void rcu_txn_declare_disjoint(struct rcu_txn *t)
{
	if (t->state == RCU_TXN_OOM)
		return;
	/* Later, the records so far would no longer be looked up. */
	if (WARN_ON_ONCE(t->state != RCU_TXN_PREPARE || t->nr))
		return;
	t->disjoint = true;
}

/**
 * rcu_txn_load() - load a slot as the transaction will leave it
 * @t: the handle
 * @slot: the slot
 * @tag: the slot's proxy tag
 *
 * Return the pending new value of the transaction's record for @slot, if it
 * has one.  Otherwise return the slot's current value.  A handle declared
 * disjoint always returns the current value.
 *
 * Context: The caller excludes other writers of @slot.
 * Return: the pending or the current value of @slot, as described above.
 */
static inline void *rcu_txn_load(struct rcu_txn *t, void __rcu **slot,
				 unsigned long tag)
{
	struct rcu_txn_record *r;
	void *v;

	/* A consumed handle has no record left: it reads committed values. */
	WARN_ON_ONCE(t->state == RCU_TXN_DONE);
	if (!t->disjoint) {
		r = __rcu_txn_scan(t, slot);
		if (r)
			return r->ptr[1];	/* our own pending write */
	}
	v = rcu_dereference_protected(*slot, 1);
	return v;
}

/*
 * The read-your-own-writes half of rcu_txn_record_chain(): update this
 * transaction's record for @slot, or append one if it has none.
 */
static inline bool __rcu_txn_chain(struct rcu_txn *t, void __rcu **slot,
				   void *old_ptr, void *new_ptr,
				   unsigned long tag)
{
	struct rcu_txn_record *r;

	r = __rcu_txn_scan(t, slot);
	if (!r)
		return rcu_txn_record(t, slot, old_ptr, new_ptr, tag);
	r->ptr[1] = new_ptr;		/* committed old preserved */
	return true;
}

/**
 * rcu_txn_record_chain() - record an edge on a slot that may repeat
 * @t: the handle
 * @slot: the slot to switch
 * @old_ptr: what rcu_txn_load() returned for @slot
 * @new_ptr: the target the transaction gives it
 * @tag: the slot's proxy tag
 *
 * Record that the transaction switches @slot from @old_ptr to @new_ptr.  If
 * the transaction already has a record for @slot, update it: that record
 * keeps its old value and takes @new_ptr.  Otherwise, append a record as
 * rcu_txn_record() does.  See the pair's contract above.
 *
 * On the chain path, @old_ptr should be the record's pending value.  A
 * different @old_ptr means that the caller computed @new_ptr from a stale
 * read.
 *
 * Context: As rcu_txn_record().
 * Return: As rcu_txn_record().
 */
static inline bool rcu_txn_record_chain(struct rcu_txn *t, void __rcu **slot,
					void *old_ptr, void *new_ptr,
					unsigned long tag)
{
	if (unlikely(t->state != RCU_TXN_PREPARE)) {
		/* OOM is sticky.  DONE: commit() or cancel() consumed it. */
		WARN_ON_ONCE(t->state != RCU_TXN_OOM);
		return false;
	}
	if (t->disjoint) {		/* no slot repeats: skip the lookup */
		return rcu_txn_record(t, slot, old_ptr, new_ptr, tag);
	}
	return __rcu_txn_chain(t, slot, old_ptr, new_ptr, tag);
}

/**
 * rcu_txn_cancel() - drop a transaction instead of committing it
 * @t: the handle
 *
 * Drop the records.  Nothing is published.  The descriptor is freed as
 * described at __rcu_txn_free_desc(), in lib/rcu_txn.c.  cancel() consumes
 * the handle.
 *
 * A handle must end in commit() or in cancel().  A handle that is merely
 * dropped leaks its descriptor.  A handle in the OOM state may be cancelled
 * as well.
 *
 * Only the records are dropped: what an embedder changed outside the
 * transaction as it recorded is not rolled back.
 *
 * Context: Does not sleep, and may be called under a raw or a bit spinlock.
 * Any context but NMI: it may call kfree_rcu().
 */
static inline void rcu_txn_cancel(struct rcu_txn *t)
{
	/* A consumed handle must be initialized again before reuse. */
	if (WARN_ON_ONCE(t->state == RCU_TXN_DONE))
		return;
	__rcu_txn_release_desc(t);
	__rcu_txn_done(t);
}

/*
 * Install, commit, settle and retire the first @nr records of @desc.  This is
 * the commit of a transaction of two records or more, which installs
 * proxies.
 */
void __rcu_txn_commit_desc(struct rcu_txn_desc *desc, unsigned int nr);

/**
 * rcu_txn_commit() - commit a transaction
 * @t: the handle
 *
 * Publish the record set, in the three steps described at the top of this
 * file.  With one record, install no proxy: one store-release to the slot is
 * the whole commit.  An empty transaction publishes nothing.  commit()
 * consumes the handle.
 *
 * With two or more records, the descriptor's reclaim is deferred with
 * kfree_rcu(): a reader may still hold a proxy.
 *
 * Context: The caller excludes other writers of the recorded slots.  Does not
 * sleep, and may be called under a raw or a bit spinlock.  Any context but
 * NMI: it may call kfree_rcu().
 * Return: 0 on success.  -ENOMEM if a reserve() or a record() of this
 * transaction failed: nothing is published then.  -EINVAL, with a warning,
 * if the handle was already consumed.
 */
static inline int rcu_txn_commit(struct rcu_txn *t)
{
	const unsigned int nr = t->nr;

	/* A consumed handle must be initialized again before reuse. */
	if (WARN_ON_ONCE(t->state == RCU_TXN_DONE))
		return -EINVAL;
	if (unlikely(t->state == RCU_TXN_OOM)) {
		rcu_txn_cancel(t);
		return -ENOMEM;
	}
	if (nr <= 1) {
		if (nr == 1) {
			struct rcu_txn_record *r = &t->records[0];

			/* One record: its settle store is the whole commit. */
			__rcu_txn_record_settle(r);
		}
		__rcu_txn_release_desc(t);
	} else {
		/*
		 * The proxies live in the descriptor, which is reclaimed after
		 * a grace period.
		 */
		__rcu_txn_commit_desc(t->desc, nr);
	}
	__rcu_txn_done(t);
	return 0;
}

#endif /* _LINUX_RCU_TXN_H */
