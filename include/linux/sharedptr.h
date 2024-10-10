// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _LINUX_SHAREDPTR_H
#define _LINUX_SHAREDPTR_H

/*
 * sharedptr: Synchronized Shared Pointers
 *
 * Synchronized shared pointers guarantee existence of objects when the
 * synchronized pointer is dereferenced. It is meant to help solving the
 * problem of object existence guarantees faced by Rust when interfacing
 * with C.
 *
 * Those shared pointers are based on a reference counter embedded into
 * the object, using hazard pointers to provide object existence
 * guarantee based on pointer dereference for synchronized shared
 * pointers.
 *
 * References:
 *
 * [1]: M. M. Michael, "Hazard pointers: safe memory reclamation for
 *      lock-free objects," in IEEE Transactions on Parallel and
 *      Distributed Systems, vol. 15, no. 6, pp. 491-504, June 2004
 */

#include <linux/hazptr.h>
#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/rcupdate.h>

DECLARE_HAZPTR_DOMAIN(hazptr_domain_sharedptr);

struct sharedptr_node {
	refcount_t refcount;
};

/*
 * Local copy of a shared pointer, holding a reference to a
 * shared pointer node.
 */
struct sharedptr {
	struct sharedptr_node *spn;
};

/*
 * A syncsharedptr has a single updater, but many threads can
 * concurrently copy a shared pointer from it using
 * sharedptr_copy_from_sync(). Just like a sharedptr, a syncsharedptr
 * holds a reference to a shared pointer node.
 */
struct syncsharedptr {
	struct sharedptr_node *spn;
};

/*
 * Initialize shared pointer node with refcount=1. Returns a shared pointer.
 */
static inline
struct sharedptr sharedptr_create(struct sharedptr_node *spn)
{
	struct sharedptr sp = {
		.spn = spn,
	};
	if (spn)
		refcount_set(&spn->refcount, 1);
	return sp;
}

static inline
struct sharedptr sharedptr_copy(struct sharedptr sp)
{
	struct sharedptr_node *spn = sp.spn;

	if (spn)
		refcount_inc(&spn->refcount);
	return sp;
}

static inline
bool sharedptr_is_null(struct sharedptr sp)
{
	return sp.spn == NULL;
}

/* Move sharedptr to a syncsharedptr. */
static inline
void sharedptr_move_to_sync(struct syncsharedptr *dst, struct sharedptr *src)
{
	WARN_ON_ONCE(dst->spn);	/* Single updater, expect dst==NULL. */
	rcu_assign_pointer(dst->spn, src->spn);
	src->spn = NULL;	/* Transfer ownership. */
}

/*
 * Copy sharedptr to a syncsharedptr, incrementing the reference.
 */
static inline
void sharedptr_copy_to_sync(struct syncsharedptr *dst, const struct sharedptr *src)
{
	struct sharedptr_node *spn = src->spn;

	WARN_ON_ONCE(dst->spn);	/* Single updater, expect dst==NULL. */
	if (spn)
		refcount_inc(&spn->refcount);
	rcu_assign_pointer(dst->spn, spn);
}

/*
 * Obtain a shared pointer copy from a syncsharedptr.
 */
static inline
struct sharedptr sharedptr_copy_from_sync(const struct syncsharedptr *ssp)
{
	struct sharedptr_node *spn, *hp;
	struct hazptr_slot *slot;
	struct sharedptr sp;

	preempt_disable();
	hp = spn = hazptr_load_try_protect(&hazptr_domain_sharedptr, &ssp->spn, &slot);
	if (!spn)
		goto end;
	if (!refcount_inc_not_zero(&spn->refcount))
		spn = NULL;
	hazptr_release(slot, hp);
end:
	sp.spn = spn;
	preempt_enable();
	return sp;
}

static inline
void syncsharedptr_delete(struct syncsharedptr *ssp,
			  void (*sharedptr_node_release)(struct sharedptr_node *spn))
{
	struct sharedptr_node *spn = ssp->spn;

	if (!spn)
		return;
	WRITE_ONCE(ssp->spn, NULL);
	if (refcount_dec_and_test(&spn->refcount)) {
		hazptr_scan(&hazptr_domain_sharedptr, spn, NULL);
		sharedptr_node_release(spn);
	}
}

static inline
void sharedptr_delete(struct sharedptr *sp,
		      void (*sharedptr_node_release)(struct sharedptr_node *spn))
{
	struct sharedptr_node *spn = sp->spn;

	if (!spn)
		return;
	WRITE_ONCE(sp->spn, NULL);
	if (refcount_dec_and_test(&spn->refcount)) {
		hazptr_scan(&hazptr_domain_sharedptr, spn, NULL);
		sharedptr_node_release(spn);
	}
}

#endif /* _LINUX_SHAREDPTR_H */
