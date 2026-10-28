/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * RCU doubly-linked list whose two directions stay coherent
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */
#ifndef _LINUX_RCU_TXN_LIST_H
#define _LINUX_RCU_TXN_LIST_H

/*
 * rcu_txn_list: a circular doubly-linked list whose forward and backward
 * chains stay mutually coherent under concurrent RCU readers.  A reader may
 * iterate in either direction, or reverse direction mid-traversal, and never
 * observe the two directions disagree.
 *
 * Why this is not just <linux/rculist.h>
 * --------------------------------------
 * The classic RCU list publishes the forward chain.  list_del_rcu() poisons
 * the removed entry's prev, and where a backward walk is allowed
 * (list_bidir_prev_rcu(), with list_bidir_del_rcu()), the forward and the
 * backward edge of an update are still two stores.  Between them a reader sees
 * one direction changed and not the other, so a step forward followed by a
 * step backward need not bring it back to where it was.
 *
 * Every insertion into or deletion from a doubly-linked list re-points exactly
 * two reader-visible edges, one forward and one backward:
 *
 *   insert E between P and N:   P->next: N -> E   and   N->prev: P -> E
 *   delete E between P and N:   P->next: E -> N   and   N->prev: E -> P
 *
 * A move of E from between P and N to between Q and R re-points six: those of
 * the delete and of the insert, and E's own next and prev, which a reader
 * standing on E follows.
 *
 * rcu_txn_list commits both edges as one RCU pseudo-transaction
 * (<linux/rcu_txn.h>): the two slots transiently hold tagged proxies that
 * share one selector, and one store-release switches both edges from old to
 * new.  At every instant the forward and backward chains describe the same
 * list.
 *
 * Coherence guarantee
 * -------------------
 * For any two nodes that are both members of the list at the instant they are
 * resolved, the links are mutual inverses: a->next == b implies b->prev == a.
 * No operation exposes a window where one direction sees a change and the other
 * does not.
 *
 * As with any RCU structure, this does not freeze the list for a long-lived
 * reader: it may observe an operation that commits during its walk.  What it
 * observes is coherent in both directions at each step.
 *
 * Coherence is a property of one resolution.  Carrying it across two
 * resolutions needs an ordering between them, which a dependency-chained walk
 * has (next-then-next, or a prev hop taken from the node the previous load
 * returned).  A reader comparing two independently-reached slots must supply
 * its own acquire on weakly-ordered hardware; see the monotonicity note in
 * <linux/rcu_txn.h>.
 *
 * A removed node keeps its own next/prev pointing at its former neighbours, so
 * a reader standing on it can still reach the live list in either direction,
 * but the live list no longer points back at it.  The caller reclaims the node
 * after a grace period, as with list_del_rcu().
 *
 * Read side
 * ---------
 * Iterate within an RCU read-side critical section.  A slot may transiently
 * hold a tagged proxy, so next/prev must be read through
 * rcu_txn_list_next_rcu() / rcu_txn_list_prev_rcu() or the iterators below,
 * never directly.  This is also why the node type is distinct from struct
 * list_head: the list_*_rcu accessors do not resolve proxies.
 *
 * Write side
 * ----------
 * Writers must be mutually excluded, as with list_add_rcu().  Each mutator
 * records its edges in a transaction, a forward and a backward one or six for
 * a move, and commits them together.  Such a transaction installs proxies, so
 * it needs a descriptor.  An _rcu mutator takes one from a pool of its CPU's,
 * with preemption disabled (rcu_txn_init_local()): it allocates nothing and
 * cannot fail, on a teardown path as anywhere else.  The _prepare forms record
 * into a transaction of the caller's, which may compose several operations,
 * and whose descriptor the caller allocates, or takes from the pool.
 *
 * A writer that excludes the others walks the list with the same iterators,
 * outside of an RCU read-side critical section, if it passes them a lockdep
 * condition for that exclusion, such as lockdep_is_held() of its lock, as with
 * list_for_each_entry_rcu(); rcu_txn_list_next_check(), _prev_check() and
 * rcu_txn_list_empty() take one too.
 */

#include <linux/build_bug.h>
#include <linux/container_of.h>
#include <linux/rcu_txn.h>
#include <linux/rcupdate.h>

/*
 * The list node, embedded in the embedder's structure.  next/prev hold a node
 * pointer or, transiently during a mutation, a tagged proxy: always read them
 * through the accessors.
 */
struct rcu_txn_list_node {
	struct rcu_txn_list_node __rcu *next, *prev;
};

/* The list head embeds the circular sentinel node. */
struct rcu_txn_list_head {
	struct rcu_txn_list_node node;
};

#define RCU_TXN_LIST_HEAD_INIT(name)					\
	{ .node = { .next = RCU_INITIALIZER(&(name).node),		\
		    .prev = RCU_INITIALIZER(&(name).node) } }

#define RCU_TXN_LIST_HEAD(name)						\
	struct rcu_txn_list_head name = RCU_TXN_LIST_HEAD_INIT(name)

static inline void rcu_txn_list_init(struct rcu_txn_list_head *head)
{
	RCU_INIT_POINTER(head->node.next, &head->node);
	RCU_INIT_POINTER(head->node.prev, &head->node);
}

/*
 * A slot value with bit 0 set is a tagged struct rcu_txn_record * rather than
 * a node pointer.  Nodes and records are at least pointer-aligned, so bit 0 is
 * free.
 */
#define RCU_TXN_LIST_TAG	1UL

/*
 * Write-side read of a link slot: this transaction's pending write to @slot if
 * it recorded one, else the slot's committed value.  A typed wrapper over
 * rcu_txn_load().
 *
 * Every _prepare below reads its neighbour links through this rather than
 * ->next / ->prev, which is what lets edits compose on one list (see
 * rcu_txn_list_add_after_prepare()).  Readers use rcu_txn_list_next_rcu() /
 * _prev_rcu() instead.
 */
static inline struct rcu_txn_list_node *
rcu_txn_list_pending(struct rcu_txn *txn, struct rcu_txn_list_node __rcu **slot)
{
	return rcu_txn_load(txn, rcu_txn_slot(slot), RCU_TXN_LIST_TAG);
}

/*
 * Forward and backward steps, within an RCU read-side critical section or
 * where @c, a lockdep condition, holds.
 */
#define rcu_txn_list_next_check(node, c)				\
	rcu_txn_dereference_check((node)->next, RCU_TXN_LIST_TAG, c)

#define rcu_txn_list_prev_check(node, c)				\
	rcu_txn_dereference_check((node)->prev, RCU_TXN_LIST_TAG, c)

/* Forward and backward steps, for use within an RCU read-side section. */
static inline struct rcu_txn_list_node *
rcu_txn_list_next_rcu(struct rcu_txn_list_node *node)
{
	return rcu_txn_dereference(node->next, RCU_TXN_LIST_TAG);
}

static inline struct rcu_txn_list_node *
rcu_txn_list_prev_rcu(struct rcu_txn_list_node *node)
{
	return rcu_txn_dereference(node->prev, RCU_TXN_LIST_TAG);
}

/*
 * True iff the list is empty, within an RCU read-side critical section or
 * where @cond, an optional lockdep condition, holds.
 *
 * Unlike list_empty(), which is a plain load, this needs one or the other:
 * while a commit is in flight on head->node.next, the slot holds a proxy, and
 * telling an empty list from another then reads the transaction's descriptor,
 * which is freed after a grace period.  A writer that excludes the others finds
 * no proxy there, and passes a lockdep condition for that exclusion as @cond.
 */
#define rcu_txn_list_empty(head, cond...)				\
	(&(head)->node ==						\
	 rcu_txn_list_next_check(&(head)->node, __rcu_txn_cond(cond)))

/**
 * rcu_txn_list_add_after_prepare() - record the insertion of a node after
 *                                    another
 * @txn: the transaction
 * @new: the node to insert
 * @pos: the node to insert it after
 *
 * Record the two edges of the insertion into the caller's transaction, without
 * committing.  The caller owns the transaction, from init to commit, and may
 * combine these records with others, including records of other structures:
 * publish a node into a tree and splice it into this list atomically, for
 * instance.
 *
 * Several _prepare calls may share a transaction, even when their
 * neighbourhoods touch.  Each reads its neighbour links through
 * rcu_txn_list_pending() and records through rcu_txn_record_chain(), so it
 * sees the transaction's own pending edits, and a second write to a slot
 * chains onto the first.
 *
 * Example: P -> E1 -> E2 -> N, deleting E1 and E2 in one transaction, a record
 * being written {slot: old -> new}.  del(E1) records {P->next: E1 -> E2} and
 * {E2->prev: E1 -> P}.  del(E2) then reads E2->prev and gets the pending P, not
 * the committed E1, so it records against P->next, finds it recorded, and
 * chains: {P->next: E1 -> N}.  With {N->prev: E2 -> P}, P and N end up linked
 * and both victims unlinked.
 *
 * Two rules remain for the caller:
 *
 *   - Look up the nodes before editing.  A traversal made with the _rcu
 *     accessors sees the committed list, not the transaction's pending one, so
 *     a caller that walks and edits in the same transaction may find a node
 *     that its own earlier edit displaced.
 *
 *   - Do not call rcu_txn_declare_disjoint() on a transaction that composes
 *     edits.  It turns off both mechanisms above: in the example, del(E2) would
 *     read the committed E1 and record {E1->next: E2 -> N} and
 *     {N->prev: E2 -> E1}, and the commit would link P and N to deleted
 *     nodes.  The four slots are distinct, so only CONFIG_DEBUG_RCU_TXN
 *     reports it, at the load.  The _rcu forms below declare disjoint because
 *     each carries a single operation.
 */
static inline void rcu_txn_list_add_after_prepare(struct rcu_txn *txn,
						  struct rcu_txn_list_node *new,
						  struct rcu_txn_list_node *pos)
{
	struct rcu_txn_list_node *next = rcu_txn_list_pending(txn, &pos->next);

	/* Build the fresh node's links before it becomes reachable. */
	RCU_INIT_POINTER(new->prev, pos);
	RCU_INIT_POINTER(new->next, next);

	/* pos->next: next -> new ; next->prev: pos -> new */
	rcu_txn_record_chain(txn, rcu_txn_slot(&pos->next), next, new,
			     RCU_TXN_LIST_TAG);
	rcu_txn_record_chain(txn, rcu_txn_slot(&next->prev), pos, new,
			     RCU_TXN_LIST_TAG);
}

/**
 * rcu_txn_list_add_before_prepare() - record the insertion of a node before
 *                                     another
 * @txn: the transaction
 * @new: the node to insert
 * @pos: the node to insert it before
 *
 * See rcu_txn_list_add_after_prepare().
 */
static inline void
rcu_txn_list_add_before_prepare(struct rcu_txn *txn,
				struct rcu_txn_list_node *new,
				struct rcu_txn_list_node *pos)
{
	struct rcu_txn_list_node *prev = rcu_txn_list_pending(txn, &pos->prev);

	RCU_INIT_POINTER(new->next, pos);
	RCU_INIT_POINTER(new->prev, prev);

	/* prev->next: pos -> new ; pos->prev: prev -> new */
	rcu_txn_record_chain(txn, rcu_txn_slot(&prev->next), pos, new,
			     RCU_TXN_LIST_TAG);
	rcu_txn_record_chain(txn, rcu_txn_slot(&pos->prev), prev, new,
			     RCU_TXN_LIST_TAG);
}

/**
 * rcu_txn_list_del_prepare() - record the unlink of a node
 * @txn: the transaction
 * @elem: the node to unlink
 *
 * See rcu_txn_list_add_after_prepare().  @elem's own next/prev are left as
 * they are, so a reader standing on it can still leave in either direction;
 * the caller frees @elem a grace period after the commit.
 */
static inline void rcu_txn_list_del_prepare(struct rcu_txn *txn,
					    struct rcu_txn_list_node *elem)
{
	struct rcu_txn_list_node *prev = rcu_txn_list_pending(txn, &elem->prev);
	struct rcu_txn_list_node *next = rcu_txn_list_pending(txn, &elem->next);

	/* prev->next: elem -> next ; next->prev: elem -> prev */
	rcu_txn_record_chain(txn, rcu_txn_slot(&prev->next), elem, next,
			     RCU_TXN_LIST_TAG);
	rcu_txn_record_chain(txn, rcu_txn_slot(&next->prev), elem, prev,
			     RCU_TXN_LIST_TAG);
}

/**
 * rcu_txn_list_replace_prepare() - record the replacement of a node
 * @txn: the transaction
 * @old: the node to replace
 * @new: the node that replaces it
 *
 * See rcu_txn_list_add_after_prepare().  @new takes @old's neighbours, and
 * @old keeps its own next/prev for readers standing on it.
 */
static inline void rcu_txn_list_replace_prepare(struct rcu_txn *txn,
						struct rcu_txn_list_node *old,
						struct rcu_txn_list_node *new)
{
	struct rcu_txn_list_node *prev = rcu_txn_list_pending(txn, &old->prev);
	struct rcu_txn_list_node *next = rcu_txn_list_pending(txn, &old->next);

	RCU_INIT_POINTER(new->prev, prev);
	RCU_INIT_POINTER(new->next, next);

	/* prev->next: old -> new ; next->prev: old -> new */
	rcu_txn_record_chain(txn, rcu_txn_slot(&prev->next), old, new,
			     RCU_TXN_LIST_TAG);
	rcu_txn_record_chain(txn, rcu_txn_slot(&next->prev), old, new,
			     RCU_TXN_LIST_TAG);
}

/**
 * rcu_txn_list_move_after_prepare() - record the move of a node after another
 * @txn: the transaction
 * @elem: the node to move, a node of the list
 * @pos: the node to move it after
 *
 * See rcu_txn_list_add_after_prepare().  Six edges switch together: the two
 * that unlink @elem, the two that link it after @pos, and @elem's own next and
 * prev, so that a reader standing on @elem leaves it by its old neighbours
 * until the commit and by its new ones from then on.  The add forms cannot
 * move a node: they build its links with plain stores, for a node that no
 * reader can reach yet.
 *
 * A move of @elem after the node it already follows records nothing.  @pos
 * must not be @elem.
 */
static inline void
rcu_txn_list_move_after_prepare(struct rcu_txn *txn,
				struct rcu_txn_list_node *elem,
				struct rcu_txn_list_node *pos)
{
	struct rcu_txn_list_node *prev = rcu_txn_list_pending(txn, &elem->prev);
	struct rcu_txn_list_node *next = rcu_txn_list_pending(txn, &elem->next);
	struct rcu_txn_list_node *succ;

	if (WARN_ON_ONCE(pos == elem) || pos == prev)
		return;
	succ = rcu_txn_list_pending(txn, &pos->next);

	/* prev->next: elem -> next ; next->prev: elem -> prev */
	rcu_txn_record_chain(txn, rcu_txn_slot(&prev->next), elem, next,
			     RCU_TXN_LIST_TAG);
	rcu_txn_record_chain(txn, rcu_txn_slot(&next->prev), elem, prev,
			     RCU_TXN_LIST_TAG);
	/* pos->next: succ -> elem ; succ->prev: pos -> elem */
	rcu_txn_record_chain(txn, rcu_txn_slot(&pos->next), succ, elem,
			     RCU_TXN_LIST_TAG);
	rcu_txn_record_chain(txn, rcu_txn_slot(&succ->prev), pos, elem,
			     RCU_TXN_LIST_TAG);
	/* elem->next: next -> succ ; elem->prev: prev -> pos */
	rcu_txn_record_chain(txn, rcu_txn_slot(&elem->next), next, succ,
			     RCU_TXN_LIST_TAG);
	rcu_txn_record_chain(txn, rcu_txn_slot(&elem->prev), prev, pos,
			     RCU_TXN_LIST_TAG);
}

/**
 * rcu_txn_list_move_before_prepare() - record the move of a node before
 *                                      another
 * @txn: the transaction
 * @elem: the node to move, a node of the list
 * @pos: the node to move it before
 *
 * See rcu_txn_list_move_after_prepare().
 */
static inline void
rcu_txn_list_move_before_prepare(struct rcu_txn *txn,
				 struct rcu_txn_list_node *elem,
				 struct rcu_txn_list_node *pos)
{
	struct rcu_txn_list_node *prev = rcu_txn_list_pending(txn, &pos->prev);

	if (WARN_ON_ONCE(pos == elem) || prev == elem)
		return;
	rcu_txn_list_move_after_prepare(txn, elem, prev);
}

/*
 * The _rcu forms: each runs one operation in a transaction of its own, a local
 * one (rcu_txn_init_local()).  They allocate nothing and cannot fail, and run
 * with preemption disabled.  The caller frees a removed or replaced node after
 * a grace period.
 */

/* A move, the largest operation, fits a local transaction. */
static_assert(RCU_TXN_LOCAL_CAP >= 6);

/* Start the transaction of a single operation: its slots are distinct. */
static inline void __rcu_txn_list_init_one(struct rcu_txn *txn)
{
	rcu_txn_init_local(txn);
	rcu_txn_declare_disjoint(txn);
}

/*
 * Commit the transaction of a single operation, which cannot fail: it records
 * no more than RCU_TXN_LOCAL_CAP edges, in a handle of its context alone.
 */
static inline void __rcu_txn_list_commit_one(struct rcu_txn *txn)
{
	rcu_txn_commit(txn);
}

static inline void rcu_txn_list_add_after_rcu(struct rcu_txn_list_node *new,
					      struct rcu_txn_list_node *pos)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn);
	rcu_txn_list_add_after_prepare(&txn, new, pos);
	__rcu_txn_list_commit_one(&txn);
}

static inline void rcu_txn_list_add_before_rcu(struct rcu_txn_list_node *new,
					       struct rcu_txn_list_node *pos)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn);
	rcu_txn_list_add_before_prepare(&txn, new, pos);
	__rcu_txn_list_commit_one(&txn);
}

/* Add @new at the head of the list (just after @head). */
static inline void rcu_txn_list_add_rcu(struct rcu_txn_list_node *new,
					struct rcu_txn_list_head *head)
{
	rcu_txn_list_add_after_rcu(new, &head->node);
}

/* Add @new at the tail of the list (just before @head). */
static inline void rcu_txn_list_add_tail_rcu(struct rcu_txn_list_node *new,
					     struct rcu_txn_list_head *head)
{
	rcu_txn_list_add_before_rcu(new, &head->node);
}

static inline void rcu_txn_list_del_rcu(struct rcu_txn_list_node *elem)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn);
	rcu_txn_list_del_prepare(&txn, elem);
	__rcu_txn_list_commit_one(&txn);
}

static inline void rcu_txn_list_replace_rcu(struct rcu_txn_list_node *old,
					    struct rcu_txn_list_node *new)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn);
	rcu_txn_list_replace_prepare(&txn, old, new);
	__rcu_txn_list_commit_one(&txn);
}

static inline void rcu_txn_list_move_after_rcu(struct rcu_txn_list_node *elem,
					       struct rcu_txn_list_node *pos)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn);
	rcu_txn_list_move_after_prepare(&txn, elem, pos);
	__rcu_txn_list_commit_one(&txn);
}

static inline void rcu_txn_list_move_before_rcu(struct rcu_txn_list_node *elem,
						struct rcu_txn_list_node *pos)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn);
	rcu_txn_list_move_before_prepare(&txn, elem, pos);
	__rcu_txn_list_commit_one(&txn);
}

/* Move @elem to the head of the list (just after @head). */
static inline void rcu_txn_list_move_rcu(struct rcu_txn_list_node *elem,
					 struct rcu_txn_list_head *head)
{
	rcu_txn_list_move_after_rcu(elem, &head->node);
}

/* Move @elem to the tail of the list (just before @head). */
static inline void rcu_txn_list_move_tail_rcu(struct rcu_txn_list_node *elem,
					      struct rcu_txn_list_head *head)
{
	rcu_txn_list_move_before_rcu(elem, &head->node);
}

#define rcu_txn_list_entry(ptr, type, member)				\
	container_of(ptr, type, member)

#define rcu_txn_list_first_entry_rcu(head, type, member)		\
	rcu_txn_list_entry(rcu_txn_list_next_rcu(&(head)->node), type, member)

#define rcu_txn_list_last_entry_rcu(head, type, member)			\
	rcu_txn_list_entry(rcu_txn_list_prev_rcu(&(head)->node), type, member)

/*
 * Iterate over the list, within an RCU read-side critical section, or where
 * @cond, an optional lockdep condition, holds.
 */
#define rcu_txn_list_for_each_rcu(pos, head, cond...)			\
	for (pos = rcu_txn_list_next_check(&(head)->node,		\
					   __rcu_txn_cond(cond));	\
	     pos != &(head)->node;					\
	     pos = rcu_txn_list_next_check(pos, __rcu_txn_cond(cond)))

#define rcu_txn_list_for_each_reverse_rcu(pos, head, cond...)		\
	for (pos = rcu_txn_list_prev_check(&(head)->node,		\
					   __rcu_txn_cond(cond));	\
	     pos != &(head)->node;					\
	     pos = rcu_txn_list_prev_check(pos, __rcu_txn_cond(cond)))

#define rcu_txn_list_for_each_entry_rcu(pos, head, member, cond...)	\
	for (pos = rcu_txn_list_entry(					\
			rcu_txn_list_next_check(&(head)->node,		\
						__rcu_txn_cond(cond)),	\
			typeof(*(pos)), member);			\
	     &(pos)->member != &(head)->node;				\
	     pos = rcu_txn_list_entry(					\
			rcu_txn_list_next_check(&(pos)->member,		\
						__rcu_txn_cond(cond)),	\
			typeof(*(pos)), member))

#define rcu_txn_list_for_each_entry_reverse_rcu(pos, head, member, cond...) \
	for (pos = rcu_txn_list_entry(					\
			rcu_txn_list_prev_check(&(head)->node,		\
						__rcu_txn_cond(cond)),	\
			typeof(*(pos)), member);			\
	     &(pos)->member != &(head)->node;				\
	     pos = rcu_txn_list_entry(					\
			rcu_txn_list_prev_check(&(pos)->member,		\
						__rcu_txn_cond(cond)),	\
			typeof(*(pos)), member))

#endif /* _LINUX_RCU_TXN_LIST_H */
