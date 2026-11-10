/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * RCU hlist edited through single-writer RCU pseudo-transactions
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */
#ifndef _LINUX_RCU_TXN_HLIST_H
#define _LINUX_RCU_TXN_HLIST_H

/*
 * rcu_txn_hlist: an RCU list with a single-pointer head, shaped like hlist.
 * It has a single writer: writers are mutually excluded, as with
 * hlist_add_head_rcu().  Its edits are recorded into a transaction of
 * <linux/rcu_txn.h>.  The commit makes all of them visible to readers at
 * once, on one bucket or on many.  It is the counterpart of
 * <linux/rcu_txn_list.h> for hash buckets.  Its head is one pointer, half
 * the size of a sentinel-node head.
 *
 * Forward-only readers, and pprev
 * -------------------------------
 * A node's backward link points to the slot that points to the node, not to
 * a node.  That slot is &prev->next or &head->first.  So no operation needs a
 * special case for the head.
 *
 *   Node: { struct rcu_txn_hlist_node *next;    (node ptr; reader-visible)
 *           struct rcu_txn_hlist_node **pprev;  (slot ptr; not walked) }
 *
 * Readers walk forward only.  They load pprev only to test
 * rcu_txn_hlist_unhashed().  So each operation has exactly one
 * reader-visible edge: the next slot it re-points.  This is where the hlist
 * differs from <linux/rcu_txn_list.h>, whose prev is reader-visible.
 *
 * A node in no chain has a NULL pprev, as hlist_unhashed() expects.  This
 * holds for a fresh node after rcu_txn_hlist_node_init().  It also holds for
 * a node that a delete or a replace removed, from the commit on.  A node must
 * be unhashed when it is added.  With CONFIG_DEBUG_RCU_TXN, the _prepare forms
 * warn otherwise.  The _rcu forms do not check it.
 * rcu_txn_hlist_unhashed() tests it, as d_unhashed() does for dcache.  A
 * removed node keeps its next.  So a reader standing on it still reaches the
 * live chain.
 *
 * Single updates and transactions
 * -------------------------------
 * The _rcu forms make a single update, outside of any transaction.  They
 * publish its one edge with rcu_assign_pointer(), as hlist_add_head_rcu()
 * does.  Then they write pprev with a plain store.  This is safe for two
 * reasons:
 *
 *   - The writers are mutually excluded.  So no other writer reads pprev in
 *     between.
 *
 *   - Every transaction settles its slots before its commit returns.  So the
 *     slots that a single update reads hold no proxy.
 *
 * A single update allocates nothing and cannot fail.  Do not call one on a
 * bucket while a transaction holds records on that bucket.  The single update
 * does not see these records, and the commit would then overwrite its change.
 *
 * The _prepare forms record into a transaction that the caller owns.  That
 * transaction may compose several updates.  It may also fail or be
 * cancelled.  These forms transact pprev too, as a slot of its own.  This has
 * two effects:
 *
 *   - A later _prepare of the same transaction loads the pending value of
 *     its neighbour's pprev: the value the transaction will leave in the
 *     slot.  This makes edits of one bucket compose (see
 *     rcu_txn_hlist_del_prepare()).
 *
 *   - A transaction that fails or is cancelled leaves every pprev as it was.
 *
 * The cost is more records.  An add records two or three edges, as it also
 * hashes the new node.  A delete records two or three, as it unhashes the
 * removed node.  A replace records three or four, as it does both.  So each
 * of them takes the descriptor path at commit, even alone.  A single update
 * is cheapest in its _rcu form.
 *
 * Composing edits of one hlist
 * ----------------------------
 * Operations whose neighbourhoods touch may share a transaction (see the
 * example at rcu_txn_hlist_del_prepare()).  A second write to a slot chains
 * onto the first.  The caller must still follow two rules:
 *
 *   - Look up the nodes before editing.  Do not traverse the list in the
 *     middle of the transaction.
 *
 *   - Every node an operation is anchored on must be in a chain, and no
 *     earlier edit of the transaction may delete it.  A deleted anchor has a
 *     NULL pending pprev.  CONFIG_DEBUG_RCU_TXN checks this.  Without it,
 *     nothing stops the edit.
 *
 *     Example: 1 -> 2 -> 3, del_prepare(2) then add_behind_prepare(9, 2).
 *     The records:
 *
 *       del:  {&1->next: 2 -> 3}
 *             {&3->pprev: &2->next -> &1->next}
 *             {&2->pprev: &1->next -> NULL}
 *       add:  {&2->next: 3 -> 9}
 *             {&9->pprev: NULL -> &2->next}
 *             {&3->pprev: &2->next -> &9->next}
 *
 *     The last one chains onto the delete's record of &3->pprev.  The add
 *     expects &2->next there, but the pending value is &1->next.
 *     CONFIG_DEBUG_RCU_TXN warns twice at the add: on 2's pending pprev, and
 *     in rcu_txn_record_chain().  The commit still returns 0.  It leaves:
 *
 *       1 -> 3                  the live chain
 *       2 -> 9 -> 3             2 is removed, 9 is reachable only through 2
 *       3->pprev == &9->next
 *
 *     A later del_rcu(3) stores through 3->pprev, into node 9, without a
 *     warning.  1->next still points to 3.  The caller then frees 3: the
 *     next walk is a use after free.
 *
 *     add_before, replace and a second delete read the anchor's pending
 *     pprev.  For a deleted anchor, it is NULL.  CONFIG_DEBUG_RCU_TXN warns.
 *     The commit then writes through that NULL slot and faults, as
 *     hlist_del_rcu() does on its poison.
 *
 * Proxy tag
 * ---------
 * The slots are head->first, node->next and node->pprev.  They are transacted
 * under RCU_TXN_HLIST_TAG, bit 0.  No live value of a slot has that bit set.
 * A slot holds NULL, the address of a node, or (for pprev) the address of a
 * pointer.
 *
 * Read side
 * ---------
 * Iterate within an RCU read-side critical section, through
 * rcu_txn_hlist_first_rcu() / _next_rcu() or the macros below.  Do not use
 * rcu_dereference() on node->next directly.  A slot may hold a tagged proxy
 * for a short time.
 *
 * Write side
 * ----------
 * Writers must be mutually excluded.  The caller reclaims a removed node
 * after a grace period, as with hlist_del_rcu().
 *
 * The add forms take a node that no reader can reach.  They build its next
 * with a plain store, so they cannot move a node.  To move an entry to
 * another bucket, delete its node and add the node of a copy, in one
 * transaction.
 *
 * A writer that excludes the others may walk the chain outside of an RCU
 * read-side critical section.  It uses the same iterators, and passes them a
 * lockdep condition for that exclusion, as with hlist_for_each_entry_rcu().
 * An example is lockdep_is_held() of its lock.  rcu_txn_hlist_first_check(),
 * _next_check(), rcu_txn_hlist_empty() and rcu_txn_hlist_unhashed() take one
 * too.
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

/* Initialize a node in no chain.  It is unhashed until it is added. */
static inline void rcu_txn_hlist_node_init(struct rcu_txn_hlist_node *node)
{
	RCU_INIT_POINTER(node->next, NULL);
	RCU_INIT_POINTER(node->pprev, NULL);
}

/*
 * Return the first node, or the next one.  Call these within an RCU
 * read-side critical section, or where the lockdep condition @c holds.
 */
#define rcu_txn_hlist_first_check(head, c)				\
	rcu_txn_dereference_check((head)->first, RCU_TXN_HLIST_TAG, c)

#define rcu_txn_hlist_next_check(node, c)				\
	rcu_txn_dereference_check((node)->next, RCU_TXN_HLIST_TAG, c)

/* Return the first node or the next one, within an RCU read-side section. */
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
 * Return true iff the chain is empty.  Call it within an RCU read-side
 * critical section, or where @cond holds.  @cond is an optional lockdep
 * condition.
 *
 * Unlike hlist_empty(), this may have to resolve a proxy.  That reads the
 * descriptor of a commit in flight: see "Readers and reclaim" in
 * <linux/rcu_txn.h>.  A writer that excludes the others finds no proxy.  It
 * passes a lockdep condition for that exclusion as @cond.
 */
#define rcu_txn_hlist_empty(head, cond...)				\
	(!rcu_txn_hlist_first_check(head, __rcu_txn_cond(cond)))

/*
 * Return true iff @node is in no chain.  Either it was never added, or a
 * delete or a replace removed it (from the commit on).  As for
 * rcu_txn_hlist_empty(), call it within an RCU read-side critical section or
 * where @cond holds.  pprev is transacted, so it holds a proxy while a commit
 * on it is in flight.  A _prepare form switches the node's link and its pprev
 * in one commit.  An _rcu form stores them one after the other: a reader
 * without the writers' lock can find the node unlinked but not yet unhashed.
 */
#define rcu_txn_hlist_unhashed(node, cond...)				\
	(!rcu_txn_dereference_check((node)->pprev, RCU_TXN_HLIST_TAG,	\
				    __rcu_txn_cond(cond)))

/*
 * Read a head->first or node->next slot on the write side.  Return this
 * transaction's pending write to it if it recorded one, else the slot's
 * committed value.  This is a typed wrapper over rcu_txn_load().  The _prepare
 * forms read every forward link through it, so edits compose on one bucket
 * (see rcu_txn_hlist_del_prepare()).  Readers use rcu_txn_hlist_first_rcu() /
 * _next_rcu() instead.
 */
static inline struct rcu_txn_hlist_node *
rcu_txn_hlist_pending_next(struct rcu_txn *txn,
			   struct rcu_txn_hlist_node __rcu **slot)
{
	return rcu_txn_load(txn, rcu_txn_slot(slot), RCU_TXN_HLIST_TAG);
}

/*
 * Read @node's pending pprev, like rcu_txn_hlist_pending_next().
 * CONFIG_DEBUG_RCU_TXN warns if it is NULL.  The operation would then be
 * anchored on a node that is unhashed, or that an earlier edit of the
 * transaction removed.
 */
static inline struct rcu_txn_hlist_node __rcu **
__rcu_txn_hlist_pending_pprev(struct rcu_txn *txn,
			      struct rcu_txn_hlist_node *node)
{
	struct rcu_txn_hlist_node __rcu **slot =
			rcu_txn_load(txn, rcu_txn_slot(&node->pprev),
				     RCU_TXN_HLIST_TAG);

	RCU_TXN_WARN_ON(!slot);
	return slot;
}

/* Run only the check of __rcu_txn_hlist_pending_pprev(), for an add_behind. */
static inline void __rcu_txn_hlist_check_anchor(struct rcu_txn *txn,
						struct rcu_txn_hlist_node *node)
{
	if (IS_ENABLED(CONFIG_DEBUG_RCU_TXN))
		__rcu_txn_hlist_pending_pprev(txn, node);
}

/*
 * Record that @node's pprev becomes @new.  @old is its pending value so far.
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

/*
 * Record that @new's pprev becomes @slot.  @new's pending pprev must be NULL:
 * CONFIG_DEBUG_RCU_TXN warns otherwise.  A cancel, or a commit that fails,
 * leaves @new unhashed.
 */
static inline void
__rcu_txn_hlist_record_hashed(struct rcu_txn *txn,
			      struct rcu_txn_hlist_node *new,
			      struct rcu_txn_hlist_node __rcu **slot)
{
	struct rcu_txn_hlist_node __rcu **was =
			rcu_txn_load(txn, rcu_txn_slot(&new->pprev),
				     RCU_TXN_HLIST_TAG);

	RCU_TXN_WARN_ON(was);
	__rcu_txn_hlist_record_pprev(txn, new, was, slot);
}

/**
 * rcu_txn_hlist_insert_at_slot_prepare() - record an insertion at a slot
 * @txn: the transaction
 * @new: the node to insert
 * @slot: the address of head->first, to insert at the head, or of pos->next,
 *        to insert behind @pos
 * @succ: the pending value of @slot: the node it points to, or NULL
 *
 * Record the edge that makes @slot point to @new, without committing.  @new's
 * pprev becomes @slot.  When @succ is non-NULL, its pprev becomes the address
 * of new->next.
 *
 * Read @succ with rcu_txn_hlist_pending_next(), as the add_head and
 * add_behind forms below do.  @new is built pointing at it.
 */
static inline void
rcu_txn_hlist_insert_at_slot_prepare(struct rcu_txn *txn,
				     struct rcu_txn_hlist_node *new,
				     struct rcu_txn_hlist_node __rcu **slot,
				     struct rcu_txn_hlist_node *succ)
{
	/* Build the fresh node's next before it becomes reachable. */
	RCU_INIT_POINTER(new->next, succ);

	rcu_txn_record_chain(txn, rcu_txn_slot(slot), succ, new,
			     RCU_TXN_HLIST_TAG);
	__rcu_txn_hlist_record_hashed(txn, new, slot);
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

	__rcu_txn_hlist_check_anchor(txn, pos);
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

	rcu_txn_record_chain(txn, rcu_txn_slot(slot), pos, new,
			     RCU_TXN_HLIST_TAG);
	__rcu_txn_hlist_record_hashed(txn, new, slot);
	__rcu_txn_hlist_record_pprev(txn, pos, slot, &new->next);
}

/**
 * rcu_txn_hlist_del_prepare() - record the unlink of a node
 * @txn: the transaction
 * @elem: the node to unlink
 *
 * @elem becomes a removed node: see "Forward-only readers, and pprev" at the
 * top of this file.  The caller frees @elem a grace period after the commit.
 *
 * Example of composing on one bucket: delete the adjacent A and B from
 * head -> A -> B -> C in one transaction.  {slot: old -> new} is a record.
 *
 *   - del(A) records {head->first: A -> B}.  It records that B's pprev now
 *     points to head->first, and that A's is NULL.
 *
 *   - del(B) loads B's pending pprev.  It finds head->first.  So it records
 *     against head->first, finds it recorded, and chains:
 *     {head->first: A -> C}.  It then records that C's pprev points to
 *     head->first, and that B's is NULL.
 *
 * Had del(B) read B->pprev directly, it would have found A->next.  That slot
 * is inside the node that del(A) unlinks.  head->first would then still point
 * to the deleted B.
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
	__rcu_txn_hlist_record_pprev(txn, elem, ppv, NULL);	/* unhashed */
}

/**
 * rcu_txn_hlist_replace_prepare() - record the replacement of a node
 * @txn: the transaction
 * @old: the node to replace
 * @new: the node that replaces it
 *
 * @new takes @old's slot and successor.  @old becomes a removed node, as for
 * rcu_txn_hlist_del_prepare().  The caller frees @old a grace period after the
 * commit.  The arguments are (old, new), as for hlist_replace_rcu().
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

	rcu_txn_record_chain(txn, rcu_txn_slot(ppv), old, new,
			     RCU_TXN_HLIST_TAG);
	__rcu_txn_hlist_record_hashed(txn, new, ppv);
	if (next)
		__rcu_txn_hlist_record_pprev(txn, next, &old->next,
					     &new->next);
	__rcu_txn_hlist_record_pprev(txn, old, ppv, NULL);	/* unhashed */
}

/*
 * Each _rcu form below makes a single update, outside of any transaction.
 * See "Single updates and transactions" above.
 */

/*
 * Read a head->first or node->next slot outside of any transaction.
 * CONFIG_DEBUG_RCU_TXN warns if it holds a proxy.  Such a proxy would belong
 * to another writer in the middle of its commit.
 */
static inline struct rcu_txn_hlist_node *
__rcu_txn_hlist_load(struct rcu_txn_hlist_node __rcu **slot)
{
	struct rcu_txn_hlist_node *v = rcu_dereference_protected(*slot, 1);

	RCU_TXN_WARN_ON(__rcu_txn_holds_proxy(v, RCU_TXN_HLIST_TAG));
	return v;
}

/*
 * Read a node's pprev outside of any transaction.  Only the writers access
 * it.  CONFIG_DEBUG_RCU_TXN warns if it is NULL.  The update would then be
 * anchored on a node that is in no chain.
 */
static inline struct rcu_txn_hlist_node __rcu **
__rcu_txn_hlist_pprev(struct rcu_txn_hlist_node *node)
{
	struct rcu_txn_hlist_node __rcu **slot =
			rcu_dereference_protected(node->pprev, 1);

	RCU_TXN_WARN_ON(!slot);
	return slot;
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
	/* @pos must be in a chain: CONFIG_DEBUG_RCU_TXN checks its pprev. */
	if (IS_ENABLED(CONFIG_DEBUG_RCU_TXN))
		__rcu_txn_hlist_pprev(pos);
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
	RCU_INIT_POINTER(elem->pprev, NULL);		/* unhashed */
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
	RCU_INIT_POINTER(old->pprev, NULL);		/* unhashed */
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
 * Iterate forward over a bucket.  Call it within an RCU read-side critical
 * section, or where @cond holds.  @cond is an optional lockdep condition.
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
