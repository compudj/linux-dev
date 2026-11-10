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
 * chains stay coherent under concurrent RCU readers.  A reader may iterate
 * in either direction.  It may also turn around in the middle of a walk.
 * Along its walk, it never sees the new value of one edge and then the old
 * value of the other.
 *
 * Why this is not just <linux/rculist.h>
 * --------------------------------------
 * The classic RCU list publishes the forward chain.  list_del_rcu() poisons
 * the prev of the removed entry.  A backward walk is allowed with
 * list_bidir_prev_rcu() and list_bidir_del_rcu().  But the forward and the
 * backward edge of an update are still two stores.  Between them, a reader
 * sees one direction changed and not the other.  So a step forward and then
 * a step backward may not bring it back to where it was.
 *
 * An insertion or a deletion in a doubly-linked list re-points exactly two
 * edges that readers see, one forward and one backward:
 *
 *   insert E between P and N:   P->next: N -> E   and   N->prev: P -> E
 *   delete E between P and N:   P->next: E -> N   and   N->prev: E -> P
 *
 * A move of E from between P and N to between Q and R re-points six edges.
 * These are the two of the delete, the two of the insert, and the next and
 * prev of E itself.  A reader standing on E follows the last two.
 *
 * rcu_txn_list commits the edges of an operation as one RCU
 * pseudo-transaction (see <linux/rcu_txn.h>).  So the forward and backward
 * chains switch together.
 *
 * Coherence guarantee
 * -------------------
 * There are two levels of coherence:
 *
 *   - Per-node coherence: a reader never finds the new value of an edge,
 *     then the old value of the other.  For two nodes a and b that are in
 *     the list when they are resolved, a->next == b implies b->prev == a.
 *     Every operation gives it, to a reader whose second load is ordered
 *     after its first (see below).
 *   - Per-walk coherence: a walk in one direction visits exactly once every
 *     node that stays in the list and does not move, and ends at its own
 *     head.  An add or a delete keeps it.  A move does not: see "Moving a
 *     node" below.
 *
 * As with any RCU structure, the list does not freeze for a long walk.  The
 * reader may see an operation that commits during its walk.
 *
 * The second load must be ordered after the first: see "Ordering" in
 * <linux/rcu_txn.h>.  For this list:
 *
 *   - A reader that steps from the node its previous load returned needs
 *     nothing more.  This is next then next, or a prev step from that node.
 *   - A reader that loads both next and prev of one node, or two slots it
 *     reached independently, loads the first one with
 *     rcu_txn_list_next_acquire_rcu() or _prev_acquire_rcu().  Or it puts
 *     smp_rmb() between the two loads.
 *
 * A removed node keeps its own next/prev pointing at its former neighbours.
 * So a reader standing on it can still reach the live list in either
 * direction.  The live list no longer points back at it.  The caller frees
 * the node after a grace period, as with list_del_rcu().
 *
 * Moving a node
 * -------------
 * A move switches its six edges in one commit, so it keeps per-node
 * coherence.  An RCU list cannot give that: a delete then an add are two
 * updates that readers observe separately.  The move does not wait for a
 * grace period.  But a reader standing on the moved node follows its new
 * links.  It may then skip or revisit nodes that did not move (see the
 * iterators).  The move fits:
 *
 *   - readers that take the first or last entry, or that reach a node
 *     through another index, then read its neighbours;
 *   - walkers that tolerate a skipped or repeated node, like an LRU scan,
 *     when the node moves within its own list;
 *   - writers that cannot sleep;
 *   - moves composed with other edits in one commit.
 *
 * Only the move forms can move a node.  The add forms build the links of
 * their node with plain stores, for a node that no reader can reach yet.
 *
 * The node must stay in its list.  A reader standing on it follows its new
 * next.  In another list, that reader would never reach its own head.  So a
 * writer cannot move nodes to a private list, such as a dispose list on its
 * stack, and there is no splice.
 *
 * For per-walk coherence, move the node in three steps:
 *
 *   1. rcu_txn_list_del_rcu()
 *   2. synchronize_rcu()
 *   3. rcu_txn_list_add_rcu(), or another add form
 *
 * A reader standing on the deleted node leaves in the old order.  The add
 * comes after every reader that saw the node at its old place.  So a walk
 * sees every other node exactly once, and the moved node at most once.  The
 * cost: the writer sleeps for a grace period, and walkers do not find the
 * node meanwhile.  This sequence can also move a node to another list.  A
 * reader standing on the node finishes its own list.
 *
 * This holds for readers that reach the node through this list.  A reader
 * that finds the node through another index, such as a hash table, can
 * still reach it while it is unlinked.  It must not follow the node's list
 * links then: they lead to its old neighbours, which may be freed by now.
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
 * records its edges in a transaction and commits them together.  These are
 * one forward and one backward edge, or six for a move.  The transaction
 * installs proxies, so it allocates a descriptor.  An _rcu mutator takes a
 * gfp_t.  It returns 0, or -ENOMEM if the transaction could not be
 * allocated.  The list is then unchanged.  A writer that must not fail
 * there reserves its transaction first and uses the _prepare forms.
 *
 * The list keeps no "unlinked" state, unlike list_del() with its poison.
 * The caller tracks which nodes are in the list.  CONFIG_DEBUG_RCU_TXN warns
 * when a delete, a replace or a move finds its node unlinked.
 *
 * A writer that excludes the others can walk the list with the same
 * iterators, outside of an RCU read-side critical section.  It passes them
 * a lockdep condition for that exclusion, such as lockdep_is_held() of its
 * lock, as with list_for_each_entry_rcu().  rcu_txn_list_next_check(),
 * _prev_check() and rcu_txn_list_empty() take one too.
 */

#include <linux/container_of.h>
#include <linux/gfp_types.h>
#include <linux/rcu_txn.h>
#include <linux/rcupdate.h>

/*
 * The list node, embedded in the user's structure.  next/prev hold a node
 * pointer, or a tagged proxy during a mutation.  Always read them through
 * the accessors.
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
 * Under a zero tag every plain value would pass for a proxy.  A tag beyond
 * RCU_TXN_TAG_MASK would overlap the address of a record.
 */
static_assert(RCU_TXN_LIST_TAG != 0 &&
	      (RCU_TXN_LIST_TAG & ~RCU_TXN_TAG_MASK) == 0);

/*
 * Read a link slot on the write side.  Return the pending write of @txn to
 * @slot if it recorded one, else the committed value of the slot.  This is a
 * typed wrapper over rcu_txn_load().
 *
 * Every _prepare below reads its neighbour links through this function, not
 * through ->next / ->prev.  This lets edits compose on one list (see
 * rcu_txn_list_add_after_prepare()).  Readers use rcu_txn_list_next_rcu() /
 * _prev_rcu() instead.
 */
static inline struct rcu_txn_list_node *
rcu_txn_list_pending(struct rcu_txn *txn, struct rcu_txn_list_node __rcu **slot)
{
	return rcu_txn_load(txn, rcu_txn_slot(slot), RCU_TXN_LIST_TAG);
}

/*
 * Step forward or backward.  Call within an RCU read-side critical section,
 * or where the lockdep condition @c holds.
 */
#define rcu_txn_list_next_check(node, c)				\
	rcu_txn_dereference_check((node)->next, RCU_TXN_LIST_TAG, c)

#define rcu_txn_list_prev_check(node, c)				\
	rcu_txn_dereference_check((node)->prev, RCU_TXN_LIST_TAG, c)

/* Step forward or backward, within an RCU read-side critical section. */
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
 * Step forward or backward with a load-acquire, within an RCU read-side
 * critical section.  A reader that loads both links of one node uses one of
 * these for its first load.  See rcu_txn_dereference_acquire().
 */
static inline struct rcu_txn_list_node *
rcu_txn_list_next_acquire_rcu(struct rcu_txn_list_node *node)
{
	return rcu_txn_dereference_acquire(node->next, RCU_TXN_LIST_TAG);
}

static inline struct rcu_txn_list_node *
rcu_txn_list_prev_acquire_rcu(struct rcu_txn_list_node *node)
{
	return rcu_txn_dereference_acquire(node->prev, RCU_TXN_LIST_TAG);
}

/*
 * Return true if the list is empty.  Call within an RCU read-side critical
 * section, or where @cond holds.  @cond is an optional lockdep condition.
 *
 * Unlike list_empty(), this may have to resolve a proxy.  That reads the
 * descriptor of a commit in flight: see "Readers and reclaim" in
 * <linux/rcu_txn.h>.  A writer that excludes the others finds no proxy.  It
 * passes a lockdep condition for that exclusion as @cond.
 */
#define rcu_txn_list_empty(head, cond...)				\
	(&(head)->node ==						\
	 rcu_txn_list_next_check(&(head)->node, __rcu_txn_cond(cond)))

/*
 * CONFIG_DEBUG_RCU_TXN: warn if @elem is not linked between @prev and @next.
 * The check reads the pending links, with rcu_txn_list_pending().  A second
 * delete of a node, or a move of a deleted one, would otherwise corrupt the
 * list.
 */
static inline void
__rcu_txn_list_check_linked(struct rcu_txn *txn, struct rcu_txn_list_node *elem,
			    struct rcu_txn_list_node *prev,
			    struct rcu_txn_list_node *next)
{
	if (!IS_ENABLED(CONFIG_DEBUG_RCU_TXN))
		return;
	RCU_TXN_WARN_ON(rcu_txn_list_pending(txn, &prev->next) != elem ||
			rcu_txn_list_pending(txn, &next->prev) != elem);
}

/**
 * rcu_txn_list_add_after_prepare() - record the insertion of a node after
 *                                    another
 * @txn: the transaction
 * @new: the node to insert
 * @pos: the node to insert it after
 *
 * Record the two edges of the insertion into the transaction of the caller.
 * Do not commit.  The caller owns the transaction, from init to commit.  It
 * may add other records to it, also for other structures.  For instance, it
 * can publish a node into a tree and add it to this list atomically.
 *
 * Several _prepare calls may share a transaction, even when they edit
 * neighbouring nodes.  Each reads its neighbour links through
 * rcu_txn_list_pending(), so it sees the pending edits of the transaction.
 * Each records through rcu_txn_record_chain(), so a second write to a slot
 * chains onto the first.
 *
 * Example: delete E1 and E2 from P -> E1 -> E2 -> N in one transaction.  A
 * record is written {slot: old -> new}.
 *
 *   - del(E1) records {P->next: E1 -> E2} and {E2->prev: E1 -> P}.
 *   - del(E2) reads E2->prev.  It gets the pending P, not the committed E1.
 *     It finds P->next already recorded and chains {P->next: E1 -> N}.
 *   - del(E2) also records {N->prev: E2 -> P}.
 *
 * P and N end up linked, and both deleted nodes are unlinked.
 *
 * The caller must follow two rules:
 *
 *   - Look up the nodes before editing.  A walk with the _rcu accessors sees
 *     the committed list, not the pending one.  A caller that walks and edits
 *     in the same transaction may find a node that its own earlier edit has
 *     already unlinked or moved.
 *
 *   - Do not call rcu_txn_declare_disjoint() on a transaction that composes
 *     edits.  It turns off both mechanisms above.  In the example, del(E2)
 *     would read the committed E1.  It would record {E1->next: E2 -> N} and
 *     {N->prev: E2 -> E1}, and the commit would link P and N to deleted
 *     nodes.  The four recorded slots are distinct, so only
 *     CONFIG_DEBUG_RCU_TXN reports it, at the load of E2->prev.  The _rcu
 *     forms below declare disjoint because each carries a single operation.
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
 * See rcu_txn_list_add_after_prepare().  The next/prev of @elem stay as they
 * are, so a reader standing on @elem can still leave in either direction.
 * The caller frees @elem a grace period after the commit.
 */
static inline void rcu_txn_list_del_prepare(struct rcu_txn *txn,
					    struct rcu_txn_list_node *elem)
{
	struct rcu_txn_list_node *prev = rcu_txn_list_pending(txn, &elem->prev);
	struct rcu_txn_list_node *next = rcu_txn_list_pending(txn, &elem->next);

	__rcu_txn_list_check_linked(txn, elem, prev, next);
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

	__rcu_txn_list_check_linked(txn, old, prev, next);
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
 * Record the six edges of the move.  See "Moving a node" at the top of this
 * file for what readers see of a move, and for the other way to move a node.
 * See rcu_txn_list_add_after_prepare() for how _prepare calls compose.
 *
 * If @elem already follows @pos, nothing is recorded.  @pos must not be
 * @elem.  @pos must be in the same list as @elem.
 */
static inline void
rcu_txn_list_move_after_prepare(struct rcu_txn *txn,
				struct rcu_txn_list_node *elem,
				struct rcu_txn_list_node *pos)
{
	struct rcu_txn_list_node *prev = rcu_txn_list_pending(txn, &elem->prev);
	struct rcu_txn_list_node *next = rcu_txn_list_pending(txn, &elem->next);
	struct rcu_txn_list_node *succ;

	__rcu_txn_list_check_linked(txn, elem, prev, next);
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
 * The _rcu forms run one operation each, in a transaction of their own.
 * They return 0, or -ENOMEM if the transaction could not be allocated with
 * @gfp.  The list is then unchanged.  The caller frees a removed or replaced
 * node after a grace period.
 */

/*
 * Start the transaction of a single operation, with @nr records.  Its slots
 * are distinct, so declare it disjoint.
 */
static inline void __rcu_txn_list_init_one(struct rcu_txn *txn, gfp_t gfp,
					   unsigned int nr)
{
	rcu_txn_init(txn, gfp);
	rcu_txn_declare_disjoint(txn);
	/* An OOM is sticky: commit reports it. */
	rcu_txn_reserve(txn, nr);
}

static inline int rcu_txn_list_add_after_rcu(struct rcu_txn_list_node *new,
					     struct rcu_txn_list_node *pos,
					     gfp_t gfp)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn, gfp, 2);
	rcu_txn_list_add_after_prepare(&txn, new, pos);
	return rcu_txn_commit(&txn);
}

static inline int rcu_txn_list_add_before_rcu(struct rcu_txn_list_node *new,
					      struct rcu_txn_list_node *pos,
					      gfp_t gfp)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn, gfp, 2);
	rcu_txn_list_add_before_prepare(&txn, new, pos);
	return rcu_txn_commit(&txn);
}

/* Add @new at the head of the list (just after @head). */
static inline int rcu_txn_list_add_rcu(struct rcu_txn_list_node *new,
				       struct rcu_txn_list_head *head,
				       gfp_t gfp)
{
	return rcu_txn_list_add_after_rcu(new, &head->node, gfp);
}

/* Add @new at the tail of the list (just before @head). */
static inline int rcu_txn_list_add_tail_rcu(struct rcu_txn_list_node *new,
					    struct rcu_txn_list_head *head,
					    gfp_t gfp)
{
	return rcu_txn_list_add_before_rcu(new, &head->node, gfp);
}

static inline int rcu_txn_list_del_rcu(struct rcu_txn_list_node *elem,
				       gfp_t gfp)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn, gfp, 2);
	rcu_txn_list_del_prepare(&txn, elem);
	return rcu_txn_commit(&txn);
}

static inline int rcu_txn_list_replace_rcu(struct rcu_txn_list_node *old,
					   struct rcu_txn_list_node *new,
					   gfp_t gfp)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn, gfp, 2);
	rcu_txn_list_replace_prepare(&txn, old, new);
	return rcu_txn_commit(&txn);
}

/*
 * Move @elem after or before @pos.  @pos must be in the same list as @elem:
 * see rcu_txn_list_move_after_prepare().
 */
static inline int rcu_txn_list_move_after_rcu(struct rcu_txn_list_node *elem,
					      struct rcu_txn_list_node *pos,
					      gfp_t gfp)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn, gfp, 6);
	rcu_txn_list_move_after_prepare(&txn, elem, pos);
	return rcu_txn_commit(&txn);
}

static inline int rcu_txn_list_move_before_rcu(struct rcu_txn_list_node *elem,
					       struct rcu_txn_list_node *pos,
					       gfp_t gfp)
{
	struct rcu_txn txn;

	__rcu_txn_list_init_one(&txn, gfp, 6);
	rcu_txn_list_move_before_prepare(&txn, elem, pos);
	return rcu_txn_commit(&txn);
}

/*
 * Move @elem to the head of its list (just after @head), or to its tail (just
 * before @head).  Unlike list_move(), @head must be the list @elem is in: see
 * rcu_txn_list_move_after_prepare().
 */
static inline int rcu_txn_list_move_rcu(struct rcu_txn_list_node *elem,
					struct rcu_txn_list_head *head,
					gfp_t gfp)
{
	return rcu_txn_list_move_after_rcu(elem, &head->node, gfp);
}

static inline int rcu_txn_list_move_tail_rcu(struct rcu_txn_list_node *elem,
					     struct rcu_txn_list_head *head,
					     gfp_t gfp)
{
	return rcu_txn_list_move_before_rcu(elem, &head->node, gfp);
}

#define rcu_txn_list_entry(ptr, type, member)				\
	container_of(ptr, type, member)

/*
 * Return the first or the last entry, or NULL if the list is empty.  Call
 * within an RCU read-side critical section.  As with list_first_or_null_rcu(),
 * a reader cannot test for an empty list first: the list may change between
 * the test and the load.
 */
#define rcu_txn_list_first_or_null_rcu(head, type, member)		\
	__rcu_txn_list_entry_or_null(head,				\
		rcu_txn_list_next_rcu(&(head)->node), type, member)

#define rcu_txn_list_last_or_null_rcu(head, type, member)		\
	__rcu_txn_list_entry_or_null(head,				\
		rcu_txn_list_prev_rcu(&(head)->node), type, member)

#define __rcu_txn_list_entry_or_null(head, ptr, type, member)		\
({									\
	struct rcu_txn_list_node *__node = (ptr);			\
									\
	__node != &(head)->node ?					\
		rcu_txn_list_entry(__node, type, member) : NULL;	\
})

/*
 * Iterate over the list.  Call within an RCU read-side critical section, or
 * where @cond holds.  @cond is an optional lockdep condition.
 *
 * At each step, a walk sees the list as of some instant.  But a reader
 * standing on a node that moves follows its new links.  Take H -> A -> B -> C:
 *
 *   - A reader on A when A moves after C steps to H.  It never sees B or C.
 *   - A reader on C when C moves to the front walks A and B again.
 *
 * A walk that must see every node once has to tolerate this, or its writers
 * must move nodes by a delete, a grace period, then an add (see "Moving a
 * node" at the top of this file).  An LRU scan that gives second chances
 * tolerates it.
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
