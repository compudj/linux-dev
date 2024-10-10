// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _LINUX_SHAREDPTR_H
#define _LINUX_SHAREDPTR_H

/*
 * sharedtr: Shared Pointers
 *
 * Shared pointers provide existence guarantees of objects when the
 * pointer is dereferenced. It is meant to help solving the general
 * problem of object existence guarantees faced by Rust when interfacing
 * with C.
 *
 * Those shared pointers are based on a reference counter embedded into
 * the object, using hazard pointers to provide object existence
 * guarantee based on pointer dereference.
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

struct sharedptr_node {
	refcount_t refcount;
};

struct sharedptr {
	struct sharedptr_node *spn;
};

struct syncsharedptr {
	struct sharedptr_node *spn;
};

/*
 * Initialize shared pointer node with refcount=1. Returns a shared pointer.
 */
struct sharedptr sharedptr_create(struct sharedptr_node *spn)
{
	struct sharedptr sp = {
		.spn = spn,
	};
	refcount_set(&spn->refcount, 1);
	return sp;
}

struct sharedptr sharedptr_copy(struct sharedptr sp)
{
	refcount_inc(&sp->spn->refcount);
	return sp;
}

bool sharedptr_move_to_sync(struct syncsharedptr *dst, struct sharedptr *src)
{

}


bool sharedptr_copy_to_sync(struct syncsharedptr *dst, const struct sharedptr *src)
{

}

/*
 * @src can be deleted concurrently with copy.
 */
bool sharedptr_copy_from_sync(struct sharedptr *dst, const struct syncsharedptr *src)
{
	struct sharedptr_node *spn;
	struct hazptr_slot *slot;
	refcount_t *hr;

	preempt_disable();
	hr = hazptr_load_try_protect(&hazptr_domain_refcount, &src->spn, &slot);
	if (!hr)
		goto null;
	spn = container_of(hr, struct sharedptr_node, refcount);
	if (!refcount_inc_not_zero(&spn->refcount)) {
		hazptr_release(slot, hr);
		goto null;
	}
	dst->spn = spn;
	hazptr_release(slot, hr);
	preempt_enable();
	return true;

null:
	dst->spn = NULL;
	preempt_enable();
	return false;
}

void syncsharedptr_delete(struct syncsharedptr *msp,
			  void (*sharedptr_node_release)(struct sharedptr_node *spn))
{
	struct sharedptr_node *spn = sp->spn;

	WRITE_ONCE(sp->spn, NULL);
	if (hazptr_refcount_dec_and_test(&spn->refcount))
		sharedptr_release(spn);

}

void sharedptr_delete(struct sharedptr *sp,
		      void (*sharedptr_node_release)(struct sharedptr_node *spn))
{
	struct sharedptr_node *spn = sp->spn;

	WRITE_ONCE(sp->spn, NULL);
	if (hazptr_refcount_dec_and_test(&spn->refcount))
		sharedptr_release(spn);
}

#endif /* _LINUX_SHAREDPTR_H */
