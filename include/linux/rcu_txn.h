/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Single-writer RCU pseudo-transactions
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */
#ifndef _LINUX_RCU_TXN_H
#define _LINUX_RCU_TXN_H

/*
 * Single-writer RCU pseudo-transactions switch a set of RCU-protected pointers,
 * the slots, from their old to their new targets with a single store, as
 * observed by concurrent RCU readers: at any instant, every slot of the set
 * resolves to its old target, or every slot to its new one.
 * Documentation/RCU/rcu_txn.rst says when to use them, and describes the data
 * structures built on them.
 *
 * The slots are the edges of the data structure that an update re-points; the
 * writer records each edge as its slot with the slot's old and new targets.
 *
 * Each slot is made to hold a tagged pointer to a record of the transaction, a
 * proxy, which resolves to the slot's old target while the selector of the
 * transaction's flip group is 0, and to its new target once it is 1.
 * rcu_txn_commit() installs the proxies, stores 1 to the selector, then settles
 * each slot to its new target and frees the records after a grace period.
 *
 * The embedding data structure is responsible for:
 *   - excluding concurrent writers from the slots a transaction commits;
 *   - choosing the tag that marks a proxy pointer so its readers recognise
 *     it (a spare low bit, or a reserved type code in an already-tagged
 *     pointer), and never storing a live value that carries it;
 *   - routing its readers' slot loads through rcu_txn_resolve(), within an
 *     RCU read-side critical section.
 *
 * Store-release / load-acquire pairs
 * ----------------------------------
 * What each of the writer's store-release orders, and against which load:
 *
 *   - Install.  The store-release of the tagged proxy into the slot
 *     (rcu_assign_pointer()) pairs with the reader's rcu_dereference() of
 *     that slot.  It orders the writer's earlier stores before the reader's
 *     loads through the proxy: the record's {old, new, group}, the selector's
 *     initialization to 0, and the new data being published, which the
 *     embedder builds before the commit.  A reader that finds the proxy thus
 *     sees the new data built, whatever the selector holds.
 *
 *   - Commit.  The store-release of the selector (0 -> 1) pairs with the
 *     load-acquire of the selector in __rcu_txn_proxy_resolve().  It orders
 *     every install of the transaction before the flip, so that a reader that
 *     resolves one proxy to its new target, then loads another slot of the
 *     transaction, ordered after that resolution, finds that slot's proxy or
 *     its new target, never its old one.
 *
 *   - Settle.  The store-release of the new target into the slot pairs with
 *     the reader's rcu_dereference() of that slot: the usual RCU publication
 *     guarantee, for a reader that finds the new target settled.
 *
 * A single-record commit installs no proxy: it is the settle store alone.
 *
 * Ordering / monotonicity
 * ------------------------
 * The selector is written exactly once (0 -> 1) and never back, so a
 * reader resolving several proxy records over time observes a monotone
 * old...old,new...new sequence -- never new-then-old.  Embedders can rely
 * on this together with a fixed read order to avoid a per-traversal
 * snapshot: a reader that follows a forward edge of a transaction to its new
 * target, then a back edge of the same transaction from that node, finds the
 * back edge's new target, never its old one.
 *
 * Scope of "never new-then-old":  Monotonicity of the selector is
 * unconditional.  It is one word, stored once.  What a reader's sequence of
 * resolutions inherits from it depends on how the reader got from one slot to
 * the next.  A dependency-chained traversal (each hop's address derived from
 * the value the previous hop loaded) carries the order with rcu_dereference().
 * A reader that hops without that chain -- re-reading a pointer it cached
 * earlier, or walking prev-then-next between two independently-reached slots --
 * has no such edge: on weakly-ordered hardware its two loads may be reordered,
 * and it can observe the new selector's effect on the later slot and the old on
 * the earlier one.  Such a reader owes itself an explicit acquire (or a
 * dependency).
 *
 * Readers and reclaim
 * -------------------
 * A reader dereferences a proxy only within a resolve section, which
 * rcu_txn_resolve() enters when the value it is given is one.  With interrupts
 * disabled, the section makes a sequence count of its CPU odd, issues a full
 * barrier, loads the slot again and resolves what the slot holds then, and
 * makes the count even again with a store-release.  A proxy loaded before the
 * section is thus never dereferenced: by then, its descriptor may serve another
 * transaction.
 *
 * Once a commit has settled every slot it installed a proxy in, its descriptor
 * can be reclaimed in either of two ways:
 *
 *   - After a grace period, as rcu_txn_commit() does with kfree_rcu(), or with
 *     call_rcu() back into its pool for the descriptor of a local handle.  A
 *     resolve section, interrupts disabled, is an RCU read-side critical
 *     section.
 *
 *   - Once __rcu_txn_synchronize_resolve() returns, without a grace period.
 *     It waits for the resolve sections in flight, busy-waiting since they are
 *     short, so that its caller need not sleep.  A pool that runs out of
 *     descriptors takes back those still waiting for a grace period this way.
 *     See lib/rcu_txn.c.
 *
 * A reader still loads a slot and uses the target it resolves within one RCU
 * read-side critical section: the embedder reclaims its nodes after a grace
 * period.
 *
 * Allocation
 * ----------
 * The descriptor is allocated with the gfp_t given to rcu_txn_init(), by
 * rcu_txn_reserve() or by an rcu_txn_record() that finds the handle full.  A
 * writer that records under a spinlock either initializes the handle with a
 * non-sleeping gfp_t, or reserves its record bound before it takes the lock.
 * Under a raw or a bit spinlock only the reserve will do: on PREEMPT_RT the
 * allocator cannot be called there.
 *
 * A transaction of at most RCU_TXN_LOCAL_CAP records that must not fail is
 * initialized with rcu_txn_init_local() instead.  Its descriptor comes from a
 * pool of its CPU's, and preemption stays disabled until its commit or cancel:
 * see lib/rcu_txn.c.
 *
 * rcu_txn_commit() and rcu_txn_cancel() allocate nothing, do not sleep, and
 * may be called under a raw or a bit spinlock.
 */

#include <linux/bug.h>
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/gfp_types.h>
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/rcu_txn_bloom.h>	/* read-your-own-writes filter */
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>

#ifdef CONFIG_DEBUG_RCU_TXN
#include <asm/current.h>
#endif

/*
 * Contract checks that cost more than a test of the handle's state are built
 * with CONFIG_DEBUG_RCU_TXN only.  They report every occurrence: a once-only
 * report would hide the violations of every embedder but the first.
 */
#ifdef CONFIG_DEBUG_RCU_TXN
# define RCU_TXN_WARN_ON(cond)	WARN_ON(cond)
#else
# define RCU_TXN_WARN_ON(cond)	BUILD_BUG_ON_INVALID(cond)
#endif

/*
 * First capacity of an unreserved handle: with its header, such a descriptor
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
 * record() appends a record {slot, old, new} to the transaction's descriptor
 * and installs nothing, so the descriptor may still move as it grows.  The
 * record set is frozen once commit() installs the proxies.  Records must target
 * pairwise-distinct slots: see rcu_txn_record().
 *
 * A handle ends in commit(), which publishes the record set, or in cancel(),
 * which drops it.  Either consumes the handle, which must be initialized again
 * before it is reused.
 *
 * OOM is sticky.  Once a reserve() or a record() has failed to allocate, later
 * ones do nothing and commit() publishes nothing and returns -ENOMEM.  An
 * embedder may therefore ignore their return values and test only commit()'s.
 *
 * Each record carries its own tag: the bits OR'd into the proxy value installed
 * in its slot, by which the slot's readers recognise a proxy (a reserved type
 * code, for instance).  It must be within RCU_TXN_TAG_MASK.
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
 * Shared selector for a flip group.  selector == 0 -> records resolve to
 * old_ptr; selector == 1 -> new_ptr.  Written once (0 -> 1) with a
 * store-release by __rcu_txn_group_commit(); read with a load-acquire by
 * __rcu_txn_proxy_resolve().
 */
struct rcu_txn_group {
	unsigned long selector;
};

/*
 * Records are aligned on RCU_TXN_RECORD_ALIGN bytes, which leaves the low bits
 * of a record's address, RCU_TXN_TAG_MASK, free for the tag that marks an
 * installed proxy.
 */
#define RCU_TXN_RECORD_ALIGN	16
#define RCU_TXN_TAG_MASK	(RCU_TXN_RECORD_ALIGN - 1UL)

struct rcu_txn_record {
	void *ptr[2];			/* [0] old, [1] new; selector-indexed */
	struct rcu_txn_group *group;
	void __rcu **slot;		/* install / settle target */
	/*
	 * Tag bits OR'd into the proxy value installed in @slot.  Per record,
	 * so that slots tagged differently (a 0xF type code, a list's bit 0)
	 * can share one transaction.
	 */
	unsigned long tag;
} __aligned(RCU_TXN_RECORD_ALIGN);

/*
 * Transaction descriptor: the single allocation that carries a committed
 * transaction across its grace period.  It holds the flip group every installed
 * proxy reads, the rcu_head that defers its reclaim, and the records, which are
 * the proxies.  The first record() or reserve() allocates it, and it may move
 * as it grows in PREPARE, no proxy being installed yet.
 */
struct rcu_txn_desc {
	struct rcu_txn_group group;		/* read by installed proxies */
	struct rcu_head rcu_head;		/* deferred-free handle */
	unsigned int cap;			/* records it can hold */
	/*
	 * Record-sized slots of the allocation: the @cap records and, in a
	 * descriptor of RCU_TXN_BLOOM_MIN records or more, past them, the
	 * read-your-own-writes filter (__rcu_txn_bloom()).
	 */
	unsigned int slots;
	struct rcu_txn_record records[] __counted_by(slots);
};

/*
 * A descriptor's size is then a multiple of a record's alignment, which is
 * what makes kmalloc() return it aligned that much: see rcu_txn_desc_alloc().
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
	bool bloom_live;		/* the descriptor's filter is armed */
	bool local;			/* see rcu_txn_init_local() */
#ifdef CONFIG_DEBUG_RCU_TXN
	/* Task that initialized or adopted the handle, and drives it. */
	struct task_struct *owner;
#endif
};

/*
 * Reader side
 * ===========
 *
 * A reader loads a transacted slot with rcu_dereference() and passes the slot
 * and the value to rcu_txn_resolve().  rcu_txn_dereference() does both.
 */

/**
 * rcu_txn_is_proxy() - is a slot's value an installed proxy?
 * @v: value loaded from a transacted slot
 * @tag: the slot's proxy tag
 *
 * No live value an embedder stores in a transacted slot may carry all of
 * @tag's bits: see rcu_txn_record().
 *
 * Return: true if @v carries all of @tag's bits.
 */
static inline bool rcu_txn_is_proxy(const void *v, unsigned long tag)
{
	return ((unsigned long)v & tag) == tag;
}

/*
 * Does @v, loaded from a slot transacted under @tag, hold an installed proxy?
 * Unlike rcu_txn_is_proxy(), this answers false under a zero tag, which every
 * value matches and which marks no proxy.  A zero tag is legal for a
 * transaction that installs none.
 */
static inline bool __rcu_txn_holds_proxy(const void *v, unsigned long tag)
{
	return tag && rcu_txn_is_proxy(v, tag);
}

/*
 * Recover the record's address from a slot value that holds an installed proxy.
 *
 * Subtracting the tag is equivalent to masking it off, since
 * rcu_txn_is_proxy() has shown every tag bit set in @v.  It lets the compiler
 * fold a constant @tag into the displacement of the loads that follow, where a
 * mask would be one more operation in the reader's chain of dependent loads.
 */
static inline struct rcu_txn_record *__rcu_txn_untag(void *v, unsigned long tag)
{
	RCU_TXN_WARN_ON(!rcu_txn_is_proxy(v, tag));
	return (struct rcu_txn_record *)((unsigned long)v - tag);
}

/*
 * Resolve the value @slot holds, within a resolve section: see "Readers and
 * reclaim" above, and lib/rcu_txn.c.  Out of line: a reader only gets here
 * while a commit is in flight on the slot.
 */
void *__rcu_txn_resolve_slow(void __rcu **slot, unsigned long tag);

/**
 * rcu_txn_resolve() - resolve a value loaded from a transacted slot
 * @slot: the slot
 * @v: value returned by rcu_dereference() on @slot
 * @tag: the slot's proxy tag
 *
 * A plain value is returned as is.  For an installed proxy, @slot is loaded
 * again within a resolve section, and what it holds then is resolved.  A proxy
 * resolves through its flip group's selector, to the slot's old target until
 * the transaction commits and to its new target from then on, and a target
 * settled in the meantime is returned as is.
 *
 * Context: RCU read-side critical section, the one @v was loaded in.  Any
 * context, NMI included.
 * Return: the slot's target.
 */
static inline void *rcu_txn_resolve(void __rcu **slot, void *v,
				    unsigned long tag)
{
	if (likely(!rcu_txn_is_proxy(v, tag)))
		return v;
	return __rcu_txn_resolve_slow(slot, tag);
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
	void __rcu **__rcu_txn_s = rcu_txn_slot(&(p));			\
	void *__rcu_txn_v = rcu_dereference_check(*__rcu_txn_s, c);	\
									\
	__rcu_txn_v = rcu_txn_resolve(__rcu_txn_s, __rcu_txn_v, tag);	\
	(typeof(*(p)) __force __kernel *)__rcu_txn_v;			\
})

/**
 * rcu_txn_dereference() - load and resolve a transacted pointer
 * @p: the RCU-protected pointer, a transacted slot
 * @tag: the slot's proxy tag
 *
 * rcu_dereference() followed by rcu_txn_resolve().  A writer that excludes
 * the slot's other writers never finds a proxy there outside of its own
 * rcu_txn_commit(), and may use rcu_dereference_protected() as is.
 *
 * Context: RCU read-side critical section.
 * Return: the slot's target, with the type @p points to.
 */
#define rcu_txn_dereference(p, tag)	rcu_txn_dereference_check(p, tag, 0)

/*
 * The optional lockdep condition of an iterator, as for
 * list_for_each_entry_rcu(): the condition if one was given, else 0.
 */
#define __rcu_txn_cond(cond...)			___rcu_txn_cond(0, ## cond, 0)
#define ___rcu_txn_cond(zero, cond, extra...)	((zero) || (cond))

/**
 * rcu_txn_slot() - the engine's view of an RCU-protected pointer's address
 * @pp: address of an __rcu pointer, of any type
 *
 * The engine takes every slot as a ``void __rcu **``.  This converts the
 * address of an embedder's typed pointer, after sparse has checked that the
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
 * Set a record's {old, new, slot, tag}; its group is bound at install.
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
 * Settle record @r's slot: store its new target there, over the proxy.  For a
 * transaction of one record, which installs no proxy, this store is the whole
 * commit.
 */
static inline void __rcu_txn_record_settle(struct rcu_txn_record *r)
{
	/* Store-release of slot pairs with rcu_dereference(). */
	rcu_assign_pointer(*r->slot, r->ptr[1]);
}

/*
 * CONFIG_DEBUG_RCU_TXN: runtime validation of the single-writer contract.
 *
 * This engine requires writer mutual exclusion -- it has no install-time CAS,
 * no conflict detection and no abort, so two writers racing on one slot simply
 * corrupt it, and a default build does not detect it.  With
 * CONFIG_DEBUG_RCU_TXN a violation warns, with a report identifying the
 * violated handle or slot.  Without it there is no check, and the handle does
 * not carry the owner field.
 *
 * There is no per-structure object to claim an owner on, so the
 * validator claims the two things that do exist:
 *
 *   - the handle.  Its owner is the task that initialized it, or the one
 *     that adopted it before anything was recorded, and reserve / record /
 *     load / commit / cancel must all run in that task.  Catches a
 *     transaction handed between tasks mid-flight.
 *
 *   - the slot, which is what two racing writers actually share, and so is
 *     where the real violation is visible.  In a correct single-writer
 *     program a slot being recorded or installed to cannot already hold an
 *     installed proxy.  A proxy sitting there therefore means another
 *     writer is mid-transaction on that very slot right now.
 *     Symmetrically, at settle each slot must still hold this writer's
 *     proxy; anything else means a concurrent writer overwrote it.
 */

#ifdef CONFIG_DEBUG_RCU_TXN

static inline void __rcu_txn_excl_claim(struct rcu_txn *t)
{
	t->owner = current;
}

static inline void __rcu_txn_excl_owner(const struct rcu_txn *t,
					const char *what)
{
	WARN(t->owner != current,
	     "rcu_txn %p: %s in a task other than the one that initialized or adopted the handle -- one transaction is driven end to end by one task\n",
	     t, what);
}

/* @slot must hold no proxy: it would be another writer's. */
static inline void __rcu_txn_excl_slot_free(void __rcu **slot,
					    unsigned long tag, const char *what)
{
	void *v = rcu_access_pointer(*slot);

	WARN(__rcu_txn_holds_proxy(v, tag),
	     "rcu_txn: slot %p already holds an installed proxy (%p) at %s: another writer is mid-transaction on it\n",
	     slot, v, what);
}

/*
 * @r's slot must still hold the recorded old.  This is the only witness of a
 * single-record commit, which installs no proxy.
 */
static inline void __rcu_txn_excl_slot_unchanged(struct rcu_txn_record *r,
						 const char *what)
{
	void *v = rcu_access_pointer(*r->slot);

	WARN(v != r->ptr[0],
	     "rcu_txn: slot %p holds %p at %s, but %p was recorded as its old: a concurrent writer changed it\n",
	     r->slot, v, what, r->ptr[0]);
}

#else	/* !CONFIG_DEBUG_RCU_TXN */

static inline void __rcu_txn_excl_claim(struct rcu_txn *t)
{
}

static inline void __rcu_txn_excl_owner(const struct rcu_txn *t,
					const char *what)
{
}

static inline void __rcu_txn_excl_slot_free(void __rcu **slot,
					    unsigned long tag, const char *what)
{
}

static inline void __rcu_txn_excl_slot_unchanged(struct rcu_txn_record *r,
						 const char *what)
{
}

#endif	/* CONFIG_DEBUG_RCU_TXN */

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
	__rcu_txn_excl_claim(t);
}

/*
 * The records of a local transaction: enough for the largest operation of
 * <linux/rcu_txn_list.h>, a move.
 */
#define RCU_TXN_LOCAL_CAP	6

/* See lib/rcu_txn.c. */
void rcu_txn_init_local(struct rcu_txn *t);

/*
 * Move the handle's records to a new descriptor holding at least @cap of them.
 * PREPARE only: no proxy is installed yet, so the records may move.  Returns
 * false if it cannot allocate, leaving the handle in the RCU_TXN_OOM state.
 */
bool __rcu_txn_resize(struct rcu_txn *t, unsigned int cap);

/**
 * rcu_txn_reserve() - give the handle a descriptor for a number of records
 * @t: the handle
 * @cap: number of records the descriptor must hold
 *
 * Optional: record() grows the descriptor on demand.  An embedder whose record
 * count is bounded can reserve that bound up front, where a failure is easy to
 * handle and where it may sleep; every later record() then appends without
 * allocating and cannot fail.
 *
 * RCU_TXN_CAP is not a floor: it is only where an unreserved handle starts.
 * Reserving less than the transaction goes on to record is correct, and costs
 * the grow that a reserve exists to avoid.
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
		/* OOM is sticky.  DONE: commit() consumed the handle. */
		WARN_ON_ONCE(t->state != RCU_TXN_OOM);
		return false;
	}
	__rcu_txn_excl_owner(t, "reserve()");
	if (t->records && cap <= t->cap)
		return true;		/* already large enough */
	return __rcu_txn_resize(t, cap);
}

/**
 * rcu_txn_adopt() - take over a handle that another task got ready
 * @t: the handle, initialized and holding no record yet
 *
 * One task drives a transaction end to end.  An embedder may still get a
 * handle ready in one task, where it can sleep and fail, with rcu_txn_init()
 * and rcu_txn_reserve(), and have another record and commit, or cancel.  The
 * task that takes over calls this first.  The embedder makes sure that the
 * task which got the handle ready is done with it.
 */
static inline void rcu_txn_adopt(struct rcu_txn *t)
{
	/* A handle that changes hands with records in it is mid-flight. */
	RCU_TXN_WARN_ON(t->nr);
	/* A local handle belongs to the CPU it disabled preemption on. */
	RCU_TXN_WARN_ON(t->local);
	__rcu_txn_excl_claim(t);
}

/* Free a descriptor that installed no proxy: see lib/rcu_txn.c. */
void __rcu_txn_free_desc(struct rcu_txn_desc *desc);

/* Put a local descriptor that installed no proxy back in its pool. */
void __rcu_txn_local_put(struct rcu_txn_desc *desc);

/*
 * Release the handle's descriptor, if it has one.  No proxy of it was
 * installed, so no grace period is owed.
 */
static inline void __rcu_txn_release_desc(struct rcu_txn *t)
{
	if (!t->desc)
		return;
	if (t->local)
		__rcu_txn_local_put(t->desc);
	else
		__rcu_txn_free_desc(t->desc);
}

/*
 * Mark the handle consumed, by commit() or by cancel(): it no longer owns a
 * descriptor, and must be initialized again before it is reused.  A local
 * handle enables preemption again.
 */
static inline void __rcu_txn_done(struct rcu_txn *t)
{
	t->desc = NULL;
	t->records = NULL;
	t->nr = 0;
	t->state = RCU_TXN_DONE;
	if (t->local)
		preempt_enable();
}

/*
 * Scan this transaction's records for @slot, or NULL.  Linear, so O(nr) per
 * call and O(nr^2) to build a write set -- __rcu_txn_find_ryw() below is what
 * keeps that off the hot path.  The returned pointer is invalidated by the
 * next record() (a grow may move the records), so use it before recording
 * again.
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
 * RCU_TXN_BLOOM_MIN: the record count from which the read-your-own-writes
 * filter (<linux/rcu_txn_bloom.h>) is used.  Below it the filter is never
 * armed, nor even zeroed.
 *
 * rcu_txn_load() is only called on a slot about to be recorded, so loads
 * track records one for one, and most transactions are tiny: one edge for an
 * hlist operation, two for a list operation.  Scanning that many records is
 * cheaper than hashing.
 *
 * The filter is built and armed by the first __rcu_txn_find_ryw() that faces
 * at least this many records, then kept current by every record().  Arming on
 * the record that crosses the threshold instead would make a transaction of
 * exactly RCU_TXN_BLOOM_MIN records build a filter it never tests.
 *
 * The filter lives in the descriptor, past the records, so that the handle on
 * the stack does not carry it.  A descriptor of this many records or more has
 * room for it (rcu_txn_desc_alloc()).
 */
#define RCU_TXN_BLOOM_MIN	8

static_assert(RCU_TXN_BLOOM_MIN > 1);

/* The filter of the handle's descriptor: see RCU_TXN_BLOOM_MIN. */
static inline u64 *__rcu_txn_bloom(const struct rcu_txn *t)
{
	return (u64 *)&t->desc->records[t->desc->cap];
}

/*
 * Build the filter from the records so far and arm it.  See
 * RCU_TXN_BLOOM_MIN.
 */
static inline void __rcu_txn_bloom_arm(struct rcu_txn *t)
{
	u64 *bloom = __rcu_txn_bloom(t);
	unsigned int i;

	memset(bloom, 0, RCU_TXN_BLOOM_SIZE);
	for (i = 0; i < t->nr; i++)
		rcu_txn_bloom_set(bloom, t->records[i].slot);
	t->bloom_live = true;
}

/*
 * __rcu_txn_scan() behind the filter: a clear bit means the slot is not
 * recorded, and the scan is skipped.  All k bits set leaves the answer to the
 * scan, which rules out a false positive.  The filter never changes the answer.
 */
static inline struct rcu_txn_record *__rcu_txn_find_ryw(struct rcu_txn *t,
							void __rcu **slot)
{
	if (unlikely(!t->bloom_live)) {
		/* Armed on first use: see RCU_TXN_BLOOM_MIN. */
		if (t->nr < RCU_TXN_BLOOM_MIN)
			return __rcu_txn_scan(t, slot);
		__rcu_txn_bloom_arm(t);
	}
	if (!rcu_txn_bloom_test(__rcu_txn_bloom(t), slot))
		return NULL;			/* definitely absent */
	return __rcu_txn_scan(t, slot);
}

/*
 * Grow a full handle: double its capacity, or give an unreserved handle its
 * first RCU_TXN_CAP records.  Returns false if it cannot allocate.
 *
 * Out of line: with this path in its body, the compiler does not inline
 * rcu_txn_record(), and every recorded edge pays a call for an append that is
 * five stores.
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
 * Record that the transaction switches @slot from @old_ptr to @new_ptr.  @tag
 * is OR'd into the proxy value installed in @slot, so that the slot's readers
 * recognise the proxy and route its resolution (a reserved type code, or a
 * list's bit 0).  Neither @old_ptr nor @new_ptr may carry all of @tag's bits:
 * after settle the slot holds @new_ptr directly, and a reader would resolve it
 * as a proxy.
 *
 * PREPARE only: the record set is frozen once proxies are installed, so this
 * must run before commit().  Nothing is installed here.
 *
 * Records must target pairwise-distinct slots.  record() appends without
 * looking for an earlier record of the same slot; recording a slot twice
 * installs two proxies on it and settles both in record order, so the earlier
 * edit is lost.  CONFIG_DEBUG_RCU_TXN warns about it at commit.  Use
 * rcu_txn_record_chain() when the same slot may be written twice.
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
		/* OOM is sticky.  DONE: commit() consumed the handle. */
		WARN_ON_ONCE(t->state != RCU_TXN_OOM);
		return false;
	}
	/*
	 * Neither value may look like a proxy under @tag.  After settle the
	 * slot holds @new_ptr directly: were it tagged, every later reader
	 * would resolve it as a proxy and dereference garbage.
	 */
	RCU_TXN_WARN_ON(__rcu_txn_holds_proxy(old_ptr, tag));
	RCU_TXN_WARN_ON(__rcu_txn_holds_proxy(new_ptr, tag));
	__rcu_txn_excl_owner(t, "record()");
	__rcu_txn_excl_slot_free(slot, tag, "record()");
	if (unlikely(t->nr == t->cap) && !__rcu_txn_grow(t))
		return false;			/* OOM, now sticky */
	r = &t->records[t->nr++];
	__rcu_txn_record_set(r, slot, old_ptr, new_ptr, tag);
	if (unlikely(t->bloom_live))	/* armed: keep it current */
		rcu_txn_bloom_set(__rcu_txn_bloom(t), slot);
	return true;
}

/*
 * Read-your-own-writes pair (rcu_txn_load() / rcu_txn_record_chain())
 * ====================================================================
 *
 * rcu_txn_record() requires pairwise-distinct slots.  This suits an embedder
 * whose slots are distinct by construction, such as a list operation recording
 * two edges: it pays no scan per store.
 *
 * An embedder whose write sites can coincide needs the opposite.  Deleting two
 * adjacent nodes of a list in one transaction writes the predecessor's next
 * slot twice, first to skip the one node and then the other, and the second
 * store must fuse with the pending one rather than duplicate it.  This pair
 * does that:
 *
 *     old = rcu_txn_load(t, slot, TAG);
 *     rcu_txn_record_chain(t, slot, old, f(old), TAG);
 *
 * The transaction then keeps at most one record per slot.  There is no read
 * set: a load records nothing and is not validated, since a single writer has
 * nothing to validate against.
 *
 * Scope.  The pair fuses same-slot stores.  It does not make every composition
 * sound: an embedder whose new value is a neighbour's pointer (a list) can
 * still build its write set from stale reads of distinct slots, unless its
 * traversal also reads through rcu_txn_load().
 *
 * After an OOM the pair no longer reads its own writes: the failed record()
 * appended nothing, so a later load() of that slot returns the committed value.
 * commit() then publishes nothing, but an embedder that changes state outside
 * the transaction as it records must not derive it from such a load.
 */

/**
 * rcu_txn_declare_disjoint() - declare the write set slot-disjoint
 * @t: the handle
 *
 * Declare that no slot is recorded twice.  rcu_txn_load() then returns the
 * committed value without looking for a pending one, and
 * rcu_txn_record_chain() appends without looking for an earlier record.
 *
 * Distinctness is a property of the slot address, not of the nodes edited: two
 * operations on distinct nodes, such as the deletion of two adjacent nodes of
 * a list, may write the same slot.
 *
 * This skips the lookup among the records so far on every load and record, a
 * scan or, past RCU_TXN_BLOOM_MIN records, a Bloom filter test: a small saving
 * from the second record on.
 *
 * Recording a slot twice on such a handle breaks the rule of rcu_txn_record(),
 * with the consequence stated there.  CONFIG_DEBUG_RCU_TXN warns about it at
 * the offending rcu_txn_load() or rcu_txn_record_chain().
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

/*
 * On a handle declared disjoint, warn when a load or a record targets a slot
 * that the transaction has already recorded.  The load would return the
 * committed value where its caller expects the pending one, and the record
 * would install a second proxy on the slot.
 */
#ifdef CONFIG_DEBUG_RCU_TXN
static inline void __rcu_txn_disjoint_check(const struct rcu_txn *t,
					    void __rcu **slot, const char *what)
{
	WARN(t->disjoint && __rcu_txn_scan(t, slot),
	     "rcu_txn %p: slot %p is already recorded at %s, on a handle declared disjoint\n",
	     t, slot, what);
}
#else
static inline void __rcu_txn_disjoint_check(const struct rcu_txn *t,
					    void __rcu **slot, const char *what)
{
}
#endif

/**
 * rcu_txn_load() - load a slot as the transaction will leave it
 * @t: the handle
 * @slot: the slot
 * @tag: the slot's proxy tag
 *
 * On a handle declared disjoint, this returns the committed value without
 * looking for a pending one.
 *
 * CONFIG_DEBUG_RCU_TXN checks that an unrecorded slot holds no proxy under
 * @tag: this writer owns every store to the slot, and its earlier transactions
 * settled before commit() returned.
 *
 * Context: The caller excludes other writers of @slot.
 * Return: the pending new value of the transaction's record for @slot if it
 * has one, else the slot's current value.
 */
static inline void *rcu_txn_load(struct rcu_txn *t, void __rcu **slot,
				 unsigned long tag)
{
	struct rcu_txn_record *r;
	void *v;

	__rcu_txn_excl_owner(t, "load()");
	/* A consumed handle has no record left: it reads committed values. */
	WARN_ON_ONCE(t->state == RCU_TXN_DONE);
	if (!t->disjoint) {
		r = __rcu_txn_find_ryw(t, slot);
		if (r)
			return r->ptr[1];	/* our own pending write */
	}
	__rcu_txn_disjoint_check(t, slot, "load()");
	v = rcu_dereference_protected(*slot, 1);
	RCU_TXN_WARN_ON(__rcu_txn_holds_proxy(v, tag));
	return v;
}

/*
 * The read-your-own-writes half of rcu_txn_record_chain(): chain onto this
 * transaction's record for @slot, or append one if it has none.
 */
static inline bool __rcu_txn_chain(struct rcu_txn *t, void __rcu **slot,
				   void *old_ptr, void *new_ptr,
				   unsigned long tag)
{
	struct rcu_txn_record *r;

	r = __rcu_txn_find_ryw(t, slot);
	if (!r)
		return rcu_txn_record(t, slot, old_ptr, new_ptr, tag);
	RCU_TXN_WARN_ON(r->tag != tag);
	/* The caller must have read its own write. */
	RCU_TXN_WARN_ON(r->ptr[1] != old_ptr);
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
 * Record that the transaction switches @slot from @old_ptr to @new_ptr,
 * chaining onto this transaction's record for @slot if it has one: that record
 * keeps its committed old and takes @new_ptr.  Otherwise append, as
 * rcu_txn_record() does.  See the pair's contract above.
 *
 * On the chain path @old_ptr is the record's pending value, and a different
 * @old_ptr means the caller computed @new_ptr from a stale read, which
 * CONFIG_DEBUG_RCU_TXN warns about.
 *
 * Context: As rcu_txn_record().
 * Return: As rcu_txn_record().
 */
static inline bool rcu_txn_record_chain(struct rcu_txn *t, void __rcu **slot,
					void *old_ptr, void *new_ptr,
					unsigned long tag)
{
	if (unlikely(t->state != RCU_TXN_PREPARE)) {
		/* OOM is sticky.  DONE: commit() consumed the handle. */
		WARN_ON_ONCE(t->state != RCU_TXN_OOM);
		return false;
	}
	__rcu_txn_excl_owner(t, "record_chain()");
	if (t->disjoint) {		/* no slot repeats: skip the lookup */
		__rcu_txn_disjoint_check(t, slot, "record_chain()");
		return rcu_txn_record(t, slot, old_ptr, new_ptr, tag);
	}
	return __rcu_txn_chain(t, slot, old_ptr, new_ptr, tag);
}

/**
 * rcu_txn_cancel() - drop a transaction instead of committing it
 * @t: the handle
 *
 * No proxy was installed, so nothing is published, and no grace period is
 * owed: the descriptor is freed with kfree(), or with kfree_rcu() on
 * PREEMPT_RT (see lib/rcu_txn.c).  Like commit(), it consumes the handle, which
 * must be initialized again before it is reused.
 *
 * A handle ends in commit() or in cancel(): one that is merely dropped leaks
 * its descriptor.  A handle in the OOM state may be cancelled as well.
 *
 * Only the records are dropped: what an embedder changed outside the
 * transaction as it recorded is not rolled back.
 */
static inline void rcu_txn_cancel(struct rcu_txn *t)
{
	__rcu_txn_excl_owner(t, "cancel()");
	/* A consumed handle must be initialized again before reuse. */
	if (WARN_ON_ONCE(t->state == RCU_TXN_DONE))
		return;
	__rcu_txn_release_desc(t);	/* no proxy: free now */
	__rcu_txn_done(t);
}

/*
 * Install, flip, settle and retire the first @nr records of @desc: the commit
 * of a transaction that installs proxies, which takes two records or more.
 * @local: @desc is a local handle's, and goes back to its pool.
 */
void __rcu_txn_commit_desc(struct rcu_txn_desc *desc, unsigned int nr,
			   bool local);

/**
 * rcu_txn_commit() - commit a transaction
 * @t: the handle
 *
 * Install every record's proxy, flip the group so that every proxy resolves
 * to new atomically, then settle each slot to its new value.  commit() owns
 * reclaim and consumes the handle, which must be initialized again before it
 * is reused.
 *
 * With one record, a lone store-release to the slot is already an atomic
 * commit.  No proxy is installed, so no reader can hold one: no grace period is
 * owed, and the descriptor is freed as by rcu_txn_cancel().  The common
 * one-pointer publish thus needs no install and no settle.  An empty
 * transaction publishes nothing.
 *
 * With two or more records, the descriptor's reclaim is deferred with
 * kfree_rcu(): a reader may still hold a proxy.  A local handle's goes back
 * to its pool instead: see rcu_txn_init_local().
 *
 * Context: The caller excludes other writers of the recorded slots.  Does not
 * sleep, and may be called under a raw or a bit spinlock.
 * Return: 0, or -ENOMEM if a reserve() or a record() of this transaction
 * failed, in which case nothing is published.  -EINVAL, with a warning,
 * for a handle that was already consumed.
 */
static inline int rcu_txn_commit(struct rcu_txn *t)
{
	const unsigned int nr = t->nr;

	__rcu_txn_excl_owner(t, "commit()");
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
			__rcu_txn_excl_slot_unchanged(r, "commit");
			__rcu_txn_record_settle(r);
		}
		__rcu_txn_release_desc(t);	/* no proxy: free now */
	} else {
		/*
		 * The proxies live in the descriptor, which is reclaimed once
		 * no reader can still read them.
		 */
		__rcu_txn_commit_desc(t->desc, nr, t->local);
	}
	__rcu_txn_done(t);
	return 0;
}

#endif /* _LINUX_RCU_TXN_H */
