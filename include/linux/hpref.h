// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _LINUX_HPREF_H
#define _LINUX_HPREF_H

/*
 * HPREF: Hazard Pointers Protected Reference Counters
 *
 * This API internally uses hazard pointers to provide existence
 * guarantees of objects, and promotes this to a reference count
 * increment before returning the object.
 *
 * This leverages the fact that both synchronization mechanisms aim to
 * guarantee existence of objects, and those existence guarantees can be
 * chained. Each mechanism achieves its purpose in a different way with
 * different tradeoffs. The hazard pointers are faster to read and scale
 * better than reference counters, but they consume more memory than a
 * per-object reference counter.
 *
 * This API uses a fixed number of hazard pointer slots (nr_cpus) across
 * the entire system.
 *
 * References:
 *
 * [1]: M. M. Michael, "Hazard pointers: safe memory reclamation for
 *      lock-free objects," in IEEE Transactions on Parallel and
 *      Distributed Systems, vol. 15, no. 6, pp. 491-504, June 2004
 */

#include <linux/refcount.h>
#include <linux/rcupdate.h>

/* Per-CPU hazard pointer slot. */
struct hpref_slot {
	/* Disable preemption, single HP reader per CPU. */
	void *addr;
};

struct hpref_hp {
	struct hpref_slot *slot;
	void *addr;
};

struct hpref_node {
	refcount_t refcount;
	void (*release)(struct hpref_node *node);
};

DECLARE_PER_CPU(struct hpref_slot, hpref_percpu_slots);

/*
 * hpref_synchronize: Wait for hazard pointer slots to be cleared.
 *
 * Wait to observe that each slot contains a value that differs from
 * @node. When hpref_hp_refcount_inc() is used concurrently to
 * dereference a pointer to a node, at least one hpref_synchronize() for
 * that node should complete between the point where all pointers to the
 * node observable by hpref_hp_refcount_inc() are unpublished and the
 * hpref_refcount_dec() associated with the node's initial reference.
 */
void hpref_synchronize(void *addr);

/*
 * hpref_slot_acquire: Obtain a hazard pointer to an object.
 *
 * Expects to be called with preemption disabled.
 */
static inline
struct hpref_hp hpref_hp_acquire(void * const * addr_p)
{
	struct hpref_slot *slot;
	struct hpref_hp hp;
	void *addr, *addr2;

	/* Use preempt off. */
	WARN_ON_ONCE(in_irq() || in_nmi());
	addr = READ_ONCE(*addr_p);
	if (!addr) {
		hp.slot = NULL;
		hp.addr = NULL;
		return hp;
	}
retry:
	slot = this_cpu_ptr(&hpref_percpu_slots);
	WARN_ON_ONCE(slot->addr);
	WRITE_ONCE(slot->addr, addr);
	/* Memory ordering: Store B before Load A. */
	smp_mb();
	/*
	 * Use RCU dereference without lockdep checks, because
	 * lockdep is not aware of HP guarantees.
	 */
	addr2 = rcu_access_pointer(*addr_p);	/* Load A */
	/*
	 * If @node_p content has changed since the first load,
	 * clear the hazard pointer and try again.
	 */
	if (!ptr_eq(addr2, addr)) {
		WRITE_ONCE(slot->addr, NULL);
		if (!addr2) {
			hp.slot = NULL;
			hp.addr = NULL;
			return hp;
		}
		addr = addr2;
		goto retry;
	}
	hp.slot = slot;
	hp.addr = addr2;
	return hp;
}

static inline
void hpref_hp_release(const struct hpref_hp hp)
{
	smp_store_release(&hp.slot->addr, NULL);
}

static inline
void *hpref_hp_addr(struct hpref_hp hp)
{
	return hp.addr;
}

/*
 * Decrement node reference count, execute release callback when
 * reaching 0.
 */
void hpref_refcount_dec(struct hpref_node *node);

/*
 * Initialize a hpref_node.
 */
static inline
void hpref_node_init(struct hpref_node *node,
		void (*release)(struct hpref_node *node))
{
	refcount_set(&node->refcount, 1);
	node->release = release;
}

/*
 * hpref_hp_refcount_inc: Obtain a reference to an object.
 *
 * Protected by hazard pointer internally, chained with increment of a
 * reference count. Returns a pointer to an object or NULL. If
 * the returned node is not NULL, the node is guaranteed to exist and
 * the caller owns a reference count to the node.
 */
static inline
struct hpref_node *hpref_hp_refcount_inc(struct hpref_node * const * node_p)
{
	struct hpref_node *node;
	struct hpref_hp hp;

	/* Use preempt off. */
	WARN_ON_ONCE(in_irq() || in_nmi());
	/* Disable preemption.*/
	guard(preempt)();
	/* Acquire hazard pointer. */
	hp = hpref_hp_acquire((void * const *)node_p);
	node = (struct hpref_node *) hpref_hp_addr(hp);
	if (!node)
		return NULL;
	/* Promote to reference count. */
	refcount_inc(&node->refcount);
	/* Release hazard pointer. */
	hpref_hp_release(hp);
	return node;
}

#endif /* _LINUX_HPREF_H */
