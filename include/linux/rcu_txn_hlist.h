/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * RCU hlist edited through single-writer RCU pseudo-transactions
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */
#ifndef _LINUX_RCU_TXN_HLIST_H
#define _LINUX_RCU_TXN_HLIST_H

/*
 * rcu_txn_hlist: an RCU list with a single-pointer head, shaped like hlist, for
 * a single writer (writers mutually excluded, as with hlist_add_head_rcu()).
 * Its edits are recorded into a transaction of <linux/rcu_txn.h>, whose commit
 * makes all of them, on one bucket or on many, visible to readers at once.
 *
 * Forward-only readers, and pprev
 * -------------------------------
 * A node's backward link is a pointer to the slot that points to it
 * (&prev->next or &head->first), not to a node, so no operation has a head
 * special case.
 *
 *   Node: { struct rcu_txn_hlist_node *next;    (node ptr; reader-visible)
 *           struct rcu_txn_hlist_node **pprev;  (slot ptr; writer-only) }
 *
 * Readers walk forward only and never load pprev, so each operation has exactly
 * one reader-visible edge: the next slot it re-points.
 *
 * Single updates and transactions
 * -------------------------------
 * The _rcu forms make a single update, outside of any transaction: they publish
 * its one edge with rcu_assign_pointer(), as hlist_add_head_rcu() does, then
 * write pprev with a plain store.  The writers are mutually excluded, and every
 * transaction settles its slots before its commit returns, so the slots they
 * read hold no proxy, and no other writer reads pprev in between.  A single
 * update allocates nothing and cannot fail.
 *
 * The _prepare forms record into a transaction of the caller's, which may
 * compose several updates, and which may fail or be cancelled.  They transact
 * pprev too, as a slot of its own.  A later _prepare of the same transaction
 * then loads its neighbour's pprev as the transaction leaves it, which is what
 * makes edits of one bucket compose (see rcu_txn_hlist_del_prepare()), and a
 * transaction that fails or is cancelled leaves every pprev as it was.  An
 * update with a successor records two edges, and so takes the descriptor path
 * at commit even alone: a single update is cheapest in its _rcu form.
 *
 * Composing edits of one hlist
 * ----------------------------
 * Operations whose neighbourhoods touch may share a transaction: each _prepare
 * loads the slots it reads, next/first and pprev, as the transaction leaves
 * them, and chains a second write to a slot onto the first.  Two rules remain
 * for the caller:
 *
 *   - Look up the nodes before editing, rather than by traversing the list in
 *     the middle of the transaction.
 *
 *   - Every node an operation is anchored on must still be in the list as the
 *     transaction leaves it.  A single-writer delete leaves no deletion mark:
 *     nothing in a deleted anchor distinguishes it from a live one.
 *
 *     Example: 1 -> 2 -> 3, del_prepare(2) then add_behind_prepare(9, 2).  The
 *     delete records {&1->next: 2 -> 3} and {&3->pprev: &2->next -> &1->next}.
 *     The add then records {&2->next: 3 -> 9}, and chains 3's pprev from
 *     &2->next, where the delete left it at &1->next.  CONFIG_DEBUG_RCU_TXN
 *     warns there, in rcu_txn_record_chain().  The commit still returns 0, and
 *     the committed chain is 1 -> 3: node 9 is reachable only through the
 *     removed node 2, and 3->pprev points to a slot that no live node holds.  A
 *     later del_rcu(3) follows that pprev, commits into the unreachable node 9
 *     and succeeds, while 1->next still points to 3, which the caller then
 *     frees.
 *
 *     add_before and replace anchored on a deleted node chain onto the delete's
 *     record of 1->next instead, where CONFIG_DEBUG_RCU_TXN warns as well, and
 *     replace warns again on 3's pprev.  The commit links the deleted node, or
 *     its replacement, back into the chain.
 *
 * Proxy tag
 * ---------
 * The slots, head->first, node->next and node->pprev, are transacted under
 * RCU_TXN_HLIST_TAG, bit 0.  No live value a slot holds carries it: a slot
 * holds NULL, the address of a node, or, for pprev, the address of a pointer.
 *
 * Read side
 * ---------
 * Iterate within an RCU read-side critical section, through
 * rcu_txn_hlist_first_rcu() / _next_rcu() or the macros below.  Never use what
 * rcu_dereference() returns for node->next directly: a slot may transiently
 * hold a tagged proxy.
 *
 * Write side
 * ----------
 * Writers must be mutually excluded.  A removed node keeps its own next, so a
 * reader standing on it still reaches the live chain; the caller reclaims it
 * after a grace period, as with hlist_del_rcu().
 *
 * The add forms take a node that no reader can reach, and build its next with
 * a plain store, so they cannot move a node.  To move an entry to another
 * bucket, delete its node and add a copy's, in one transaction.
 *
 * A writer that excludes the others walks the chain with the same iterators,
 * outside of an RCU read-side critical section, if it passes them a lockdep
 * condition for that exclusion, such as lockdep_is_held() of its lock, as with
 * hlist_for_each_entry_rcu(); rcu_txn_hlist_first_check(), _next_check() and
 * rcu_txn_hlist_empty() take one too.
 */

#include <linux/build_bug.h>
#include <linux/container_of.h>
#include <linux/rcu_txn.h>
#include <linux/rcupdate.h>

/* Proxy tag of the slots, head->first, node->next and node->pprev. */
#define RCU_TXN_HLIST_TAG	1UL

/*
 * Under a zero tag every plain value would pass for a proxy.  A tag beyond
 * RCU_TXN_TAG_MASK would overlap the address of a record.
 */
static_assert(RCU_TXN_HLIST_TAG != 0 &&
	      (RCU_TXN_HLIST_TAG & ~RCU_TXN_TAG_MASK) == 0);

struct rcu_txn_hlist_node {
	struct rcu_txn_hlist_node __rcu *next;		/* reader-visible */
	struct rcu_txn_hlist_node __rcu * __rcu *pprev;	/* writer-only */
};

/* A bucket head is one pointer, like struct hlist_head. */
struct rcu_txn_hlist_head {
	struct rcu_txn_hlist_node __rcu *first;
};

#define RCU_TXN_HLIST_HEAD_INIT	{ .first = NULL }

static inline void rcu_txn_hlist_init(struct rcu_txn_hlist_head *head)
{
	RCU_INIT_POINTER(head->first, NULL);
}

/*
 * First node and forward step, within an RCU read-side critical section or
 * where @c, a lockdep condition, holds.
 */
#define rcu_txn_hlist_first_check(head, c)				\
	rcu_txn_dereference_check((head)->first, RCU_TXN_HLIST_TAG, c)

#define rcu_txn_hlist_next_check(node, c)				\
	rcu_txn_dereference_check((node)->next, RCU_TXN_HLIST_TAG, c)

/* First node and forward step, for use within an RCU read-side section. */
static inline struct rcu_txn_hlist_node *
rcu_txn_hlist_first_rcu(struct rcu_txn_hlist_head *head)
{
	return rcu_txn_dereference(head->first, RCU_TXN_HLIST_TAG);
}

static inline struct rcu_txn_hlist_node *
rcu_txn_hlist_next_rcu(struct rcu_txn_hlist_node *node)
{
	return rcu_txn_dereference(node->next, RCU_TXN_HLIST_TAG);
}

/*
 * True iff the chain is empty, within an RCU read-side critical section or
 * where @cond, an optional lockdep condition, holds.
 *
 * Unlike hlist_empty(), which is a plain load, this needs one or the other:
 * while a commit of two records or more is in flight on head->first, the slot
 * holds a proxy, and telling an empty chain from another then reads the
 * transaction's descriptor, which is freed after a grace period.  A writer that
 * excludes the others finds no proxy there, and passes a lockdep condition for
 * that exclusion as @cond.
 */
#define rcu_txn_hlist_empty(head, cond...)				\
	(!rcu_txn_hlist_first_check(head, __rcu_txn_cond(cond)))

/*
 * Write-side read of a head->first or node->next slot: this transaction's
 * pending write to it if it recorded one, else the slot's committed value.  A
 * typed wrapper over rcu_txn_load().  The _prepare forms read every forward
 * link through this, which is what lets edits compose on one bucket (see
 * rcu_txn_hlist_del_prepare()).  Readers use rcu_txn_hlist_first_rcu() /
 * _next_rcu() instead.
 */
static inline struct rcu_txn_hlist_node *
rcu_txn_hlist_pending_next(struct rcu_txn *txn,
			   struct rcu_txn_hlist_node __rcu **slot)
{
	return rcu_txn_load(txn, rcu_txn_slot(slot), RCU_TXN_HLIST_TAG);
}

/* The pprev counterpart of rcu_txn_hlist_pending_next(). */
static inline struct rcu_txn_hlist_node __rcu **
__rcu_txn_hlist_pending_pprev(struct rcu_txn *txn,
			      struct rcu_txn_hlist_node *node)
{
	return rcu_txn_load(txn, rcu_txn_slot(&node->pprev), RCU_TXN_HLIST_TAG);
}

/*
 * Record that @node's pprev, @old as the transaction leaves it so far, becomes
 * @new.
 */
static inline void
__rcu_txn_hlist_record_pprev(struct rcu_txn *txn,
			     struct rcu_txn_hlist_node *node,
			     struct rcu_txn_hlist_node __rcu **old,
			     struct rcu_txn_hlist_node __rcu **new)
{
	rcu_txn_record_chain(txn, rcu_txn_slot(&node->pprev), old, new,
			     RCU_TXN_HLIST_TAG);
}

/**
 * rcu_txn_hlist_insert_at_slot_prepare() - record an insertion at a slot
 * @txn: the transaction
 * @new: the node to insert
 * @slot: the address of head->first, to insert at the head, or of pos->next,
 *        to insert behind @pos
 * @succ: the node @slot points to as this transaction leaves it, or NULL
 *
 * Record the edge that makes @slot point to @new, without committing.  When
 * @succ is non-NULL, its pprev becomes the address of new->next.
 *
 * @succ must be read with rcu_txn_hlist_pending_next(): @new is built pointing
 * at it, and a value read directly would link @new behind a node that an
 * earlier edit of the same transaction displaced.  The add_head and add_behind
 * forms below do so.
 */
static inline void
rcu_txn_hlist_insert_at_slot_prepare(struct rcu_txn *txn,
				     struct rcu_txn_hlist_node *new,
				     struct rcu_txn_hlist_node __rcu **slot,
				     struct rcu_txn_hlist_node *succ)
{
	/* Build the fresh node's links before it becomes reachable. */
	RCU_INIT_POINTER(new->next, succ);
	RCU_INIT_POINTER(new->pprev, slot);

	rcu_txn_record_chain(txn, rcu_txn_slot(slot), succ, new,
			     RCU_TXN_HLIST_TAG);
	if (succ)
		__rcu_txn_hlist_record_pprev(txn, succ, slot, &new->next);
}

/**
 * rcu_txn_hlist_add_head_prepare() - record an insertion at the head
 * @txn: the transaction
 * @new: the node to insert
 * @head: the list
 *
 * See rcu_txn_hlist_insert_at_slot_prepare().
 */
static inline void
rcu_txn_hlist_add_head_prepare(struct rcu_txn *txn,
			       struct rcu_txn_hlist_node *new,
			       struct rcu_txn_hlist_head *head)
{
	struct rcu_txn_hlist_node *first =
			rcu_txn_hlist_pending_next(txn, &head->first);

	rcu_txn_hlist_insert_at_slot_prepare(txn, new, &head->first, first);
}

/**
 * rcu_txn_hlist_add_behind_prepare() - record an insertion behind a node
 * @txn: the transaction
 * @new: the node to insert
 * @pos: the node to insert it behind
 *
 * See rcu_txn_hlist_insert_at_slot_prepare().
 */
static inline void
rcu_txn_hlist_add_behind_prepare(struct rcu_txn *txn,
				 struct rcu_txn_hlist_node *new,
				 struct rcu_txn_hlist_node *pos)
{
	struct rcu_txn_hlist_node *next =
			rcu_txn_hlist_pending_next(txn, &pos->next);

	rcu_txn_hlist_insert_at_slot_prepare(txn, new, &pos->next, next);
}

/**
 * rcu_txn_hlist_add_before_prepare() - record an insertion before a node
 * @txn: the transaction
 * @new: the node to insert
 * @pos: the node to insert it before
 *
 * @pos's pprev points to the slot to re-point.
 */
static inline void
rcu_txn_hlist_add_before_prepare(struct rcu_txn *txn,
				 struct rcu_txn_hlist_node *new,
				 struct rcu_txn_hlist_node *pos)
{
	struct rcu_txn_hlist_node __rcu **slot =
			__rcu_txn_hlist_pending_pprev(txn, pos);

	RCU_INIT_POINTER(new->next, pos);
	RCU_INIT_POINTER(new->pprev, slot);

	rcu_txn_record_chain(txn, rcu_txn_slot(slot), pos, new,
			     RCU_TXN_HLIST_TAG);
	__rcu_txn_hlist_record_pprev(txn, pos, slot, &new->next);
}

/**
 * rcu_txn_hlist_del_prepare() - record the unlink of a node
 * @txn: the transaction
 * @elem: the node to unlink
 *
 * @elem's own next and pprev are left as they are, so a reader standing on it
 * still reaches the live chain; the caller frees @elem a grace period after the
 * commit.
 *
 * Composing on one bucket, by example: delete the adjacent A and B from head ->
 * A -> B -> C in one transaction, a record being written {slot: old -> new}.
 * del(A) records {head->first: A -> B}, and that B's pprev now points to
 * head->first.  del(B) loads B's pprev as the transaction leaves it and finds
 * head->first, not the stale A->next, so it records against head->first, finds
 * it recorded, and chains: {head->first: A -> C}.  It then records that C's
 * pprev points to head->first.
 *
 * Had del(B) read B->pprev directly, it would have found A->next, a slot inside
 * the node that del(A) unlinks, and head->first would be left pointing to the
 * deleted B.
 */
static inline void rcu_txn_hlist_del_prepare(struct rcu_txn *txn,
					     struct rcu_txn_hlist_node *elem)
{
	struct rcu_txn_hlist_node *next =
			rcu_txn_hlist_pending_next(txn, &elem->next);
	struct rcu_txn_hlist_node __rcu **ppv =
			__rcu_txn_hlist_pending_pprev(txn, elem);

	rcu_txn_record_chain(txn, rcu_txn_slot(ppv), elem, next,
			     RCU_TXN_HLIST_TAG);
	if (next)
		__rcu_txn_hlist_record_pprev(txn, next, &elem->next, ppv);
}

/**
 * rcu_txn_hlist_replace_prepare() - record the replacement of a node
 * @txn: the transaction
 * @old: the node to replace
 * @new: the node that replaces it
 *
 * @new takes @old's slot and successor, and @old keeps its own next for readers
 * standing on it.  Arguments are (old, new), as hlist_replace_rcu().
 */
static inline void rcu_txn_hlist_replace_prepare(struct rcu_txn *txn,
						 struct rcu_txn_hlist_node *old,
						 struct rcu_txn_hlist_node *new)
{
	struct rcu_txn_hlist_node *next =
			rcu_txn_hlist_pending_next(txn, &old->next);
	struct rcu_txn_hlist_node __rcu **ppv =
			__rcu_txn_hlist_pending_pprev(txn, old);

	RCU_INIT_POINTER(new->next, next);
	RCU_INIT_POINTER(new->pprev, ppv);

	rcu_txn_record_chain(txn, rcu_txn_slot(ppv), old, new,
			     RCU_TXN_HLIST_TAG);
	if (next)
		__rcu_txn_hlist_record_pprev(txn, next, &old->next,
					     &new->next);
}

/*
 * The _rcu forms: each makes a single update, outside of any transaction.  See
 * "Single updates and transactions" above.
 */

/*
 * Read a head->first or node->next slot outside of any transaction.
 * CONFIG_DEBUG_RCU_TXN checks that it holds no proxy: one would be another
 * writer's, in the middle of its commit.
 */
static inline struct rcu_txn_hlist_node *
__rcu_txn_hlist_load(struct rcu_txn_hlist_node __rcu **slot)
{
	struct rcu_txn_hlist_node *v = rcu_dereference_protected(*slot, 1);

	RCU_TXN_WARN_ON(__rcu_txn_holds_proxy(v, RCU_TXN_HLIST_TAG));
	return v;
}

/* A node's pprev, which only the writers access, outside of any transaction. */
static inline struct rcu_txn_hlist_node __rcu **
__rcu_txn_hlist_pprev(struct rcu_txn_hlist_node *node)
{
	return rcu_dereference_protected(node->pprev, 1);
}

static inline void
__rcu_txn_hlist_insert_at_slot_rcu(struct rcu_txn_hlist_node *new,
				   struct rcu_txn_hlist_node __rcu **slot)
{
	struct rcu_txn_hlist_node *succ = __rcu_txn_hlist_load(slot);

	RCU_INIT_POINTER(new->next, succ);
	RCU_INIT_POINTER(new->pprev, slot);
	rcu_assign_pointer(*slot, new);
	if (succ)
		RCU_INIT_POINTER(succ->pprev, &new->next);
}

static inline void rcu_txn_hlist_add_head_rcu(struct rcu_txn_hlist_node *new,
					      struct rcu_txn_hlist_head *head)
{
	__rcu_txn_hlist_insert_at_slot_rcu(new, &head->first);
}

static inline void rcu_txn_hlist_add_behind_rcu(struct rcu_txn_hlist_node *new,
						struct rcu_txn_hlist_node *pos)
{
	__rcu_txn_hlist_insert_at_slot_rcu(new, &pos->next);
}

static inline void rcu_txn_hlist_add_before_rcu(struct rcu_txn_hlist_node *new,
						struct rcu_txn_hlist_node *pos)
{
	struct rcu_txn_hlist_node __rcu **slot = __rcu_txn_hlist_pprev(pos);

	RCU_INIT_POINTER(new->next, pos);
	RCU_INIT_POINTER(new->pprev, slot);
	rcu_assign_pointer(*slot, new);
	RCU_INIT_POINTER(pos->pprev, &new->next);
}

static inline void rcu_txn_hlist_del_rcu(struct rcu_txn_hlist_node *elem)
{
	struct rcu_txn_hlist_node __rcu **ppv = __rcu_txn_hlist_pprev(elem);
	struct rcu_txn_hlist_node *next = __rcu_txn_hlist_load(&elem->next);

	rcu_assign_pointer(*ppv, next);
	if (next)
		RCU_INIT_POINTER(next->pprev, ppv);
}

static inline void rcu_txn_hlist_replace_rcu(struct rcu_txn_hlist_node *old,
					     struct rcu_txn_hlist_node *new)
{
	struct rcu_txn_hlist_node __rcu **ppv = __rcu_txn_hlist_pprev(old);
	struct rcu_txn_hlist_node *next = __rcu_txn_hlist_load(&old->next);

	RCU_INIT_POINTER(new->next, next);
	RCU_INIT_POINTER(new->pprev, ppv);
	rcu_assign_pointer(*ppv, new);
	if (next)
		RCU_INIT_POINTER(next->pprev, &new->next);
}

#define rcu_txn_hlist_entry(ptr, type, member)				\
	container_of(ptr, type, member)

#define rcu_txn_hlist_entry_safe(ptr, type, member)			\
({									\
	typeof(ptr) ____ptr = (ptr);					\
									\
	____ptr ? rcu_txn_hlist_entry(____ptr, type, member) : NULL;	\
})

/*
 * Iterate forward over a bucket, within an RCU read-side critical section, or
 * where @cond, an optional lockdep condition, holds.
 */
#define rcu_txn_hlist_for_each_rcu(pos, head, cond...)			\
	for (pos = rcu_txn_hlist_first_check(head, __rcu_txn_cond(cond)); \
	     pos;							\
	     pos = rcu_txn_hlist_next_check(pos, __rcu_txn_cond(cond)))

#define rcu_txn_hlist_for_each_entry_rcu(pos, head, member, cond...)	\
	for (pos = rcu_txn_hlist_entry_safe(				\
			rcu_txn_hlist_first_check(head, __rcu_txn_cond(cond)), \
			typeof(*(pos)), member);			\
	     pos;							\
	     pos = rcu_txn_hlist_entry_safe(				\
			rcu_txn_hlist_next_check(&(pos)->member,	\
						 __rcu_txn_cond(cond)),	\
			typeof(*(pos)), member))

#endif /* _LINUX_RCU_TXN_HLIST_H */
