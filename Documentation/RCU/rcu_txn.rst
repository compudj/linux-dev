.. SPDX-License-Identifier: GPL-2.0

=====================================
Single-writer RCU pseudo-transactions
=====================================

An RCU pseudo-transaction switches a *set* of RCU-protected pointers from
their old values to their new values.  Concurrent RCU readers see the whole
set switch with a single store.  Before that store, every pointer of the
set reads old.  After it, every pointer reads new.  Consider a reader that
follows several of these pointers, each load depending on the previous one.
It sees old values, then new ones, and never an old value after a new one.

The implementation is in ``include/linux/rcu_txn.h`` and ``lib/rcu_txn.c``.
Code that uses it selects ``CONFIG_RCU_TXN``.

When to use it
==============

``rcu_assign_pointer()`` publishes one pointer atomically.  An update that
re-points several pointers one at a time exposes its intermediate states.  A
reader that loads two of them may get the new value of one, then the old
value of the other.  Moving an object between two hash buckets is such an
update.  So is every insertion into or removal from a doubly-linked list.

A pseudo-transaction removes the intermediate states.  It does not replace the
update-side lock.  The caller excludes the writers of each pointer.  The
transaction neither detects nor resolves conflicts between writers.  This is
why it is "pseudo": there is no read set and no abort, and readers get no
snapshot.

A pseudo-transaction is not free, above all on the update side.  Every update
costs an allocation, which can fail, and a free deferred by a grace period.
``rcu_assign_pointer()`` needs neither.  The reader tests the low bits of every
pointer it loads, which is nearly free.  It pays more only when it meets a
commit in progress on that pointer.

How it works
============

The pointers that a transaction switches are called *slots*.  They are the
*edges* of the data structure that the update re-points.  The writer builds a
*record* per slot, which holds the slot's old and new targets.  Then it
commits in three steps:

1. *Install.*  Each slot is made to point at its record.  Tag bits in this
   value mark it as a *proxy*, not a target.  A reader that finds a proxy
   resolves it through the record.  The record still gives the old target.

2. *Commit.*  The records of a transaction share one word, the *selector*.
   The writer stores 1 to it.  From then on every proxy of the transaction
   resolves to its new target.

3. *Settle.*  Each slot is overwritten with its new target.  The records and
   the selector are one allocation, the *descriptor*.  It is freed after a
   grace period.

The selector is written once, from 0 to 1.  This gives the guarantee stated at
the top of this page.  ``include/linux/rcu_txn.h`` describes the memory
ordering in detail.

A transaction of a single record installs no proxy: its commit is one
``rcu_assign_pointer()``.

Tags
====

The embedder chooses the tag that marks a proxy in its slots.  It is typically
bit 0, or a reserved value of a type code that its pointers already carry.
The tag must be within ``RCU_TXN_TAG_MASK``.  These are the low bits that the
alignment of a record leaves clear.  No value that the embedder stores in a
slot may carry all of the tag's bits.

Readers
=======

A reader loads a slot with ``rcu_txn_dereference()`` instead of
``rcu_dereference()``::

	rcu_read_lock();
	child = rcu_txn_dereference(node->left, FOO_TAG);
	if (child)
		do_something_with(child);
	rcu_read_unlock();

``rcu_txn_dereference()`` is ``rcu_dereference()`` followed by
``rcu_txn_resolve()``.  A reader that already has the loaded value, for
instance because it tests other bits of it, calls ``rcu_txn_resolve()``
itself.

The load and the resolution must happen in one RCU read-side critical section:
the record a proxy points at is freed with ``kfree_rcu()``.

Writers
=======

The writer declares a handle on its stack, records the edges of the update and
commits them::

	struct rcu_txn txn;
	struct foo *child;
	int err;

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_reserve(&txn, 2);

	spin_lock(&foo_lock);
	child = rcu_dereference_protected(a->left, lockdep_is_held(&foo_lock));
	rcu_txn_record(&txn, rcu_txn_slot(&a->left), child, NULL, FOO_TAG);
	rcu_txn_record(&txn, rcu_txn_slot(&b->right), NULL, child, FOO_TAG);
	err = rcu_txn_commit(&txn);
	spin_unlock(&foo_lock);

Here ``child`` moves from ``a->left`` to an empty ``b->right``.  Both slots
switch with one store.

Slots are ``__rcu`` pointers.  ``rcu_txn_slot()`` converts the address of a
slot of any type to the ``void __rcu **`` that the engine takes.  It lets
sparse check the ``__rcu`` annotation.

A handle ends in ``rcu_txn_commit()`` or in ``rcu_txn_cancel()``.  Either
consumes it, and it must be initialized again before it is reused.

Allocation
----------

A transaction allocates a descriptor, with the ``gfp_t`` given to
``rcu_txn_init()``.  ``rcu_txn_reserve()`` allocates it for a given number of
records.  Without a reserve, the first ``rcu_txn_record()`` allocates it, even
for a single record.  A later ``rcu_txn_record()`` may grow it.

A failed allocation is sticky.  Later records do nothing.
``rcu_txn_commit()`` then publishes nothing and returns ``-ENOMEM``.  A writer
may therefore ignore the return values of ``rcu_txn_reserve()`` and
``rcu_txn_record()``.  It checks only the return value of
``rcu_txn_commit()``, as in the example above.

A writer that records under a spinlock can reserve its record bound before it
takes the lock, as above.  It can also initialize the handle with a
non-sleeping ``gfp_t``.  Under a raw or a bit spinlock, only the reserve
works: ``PREEMPT_RT`` does not allow the allocator there.
``rcu_txn_commit()`` and ``rcu_txn_cancel()`` allocate nothing and do not
sleep.  They may be called under such a lock.

An update that re-points a single slot needs no transaction.  Every
transaction settles its slots before its commit returns.  The writers of the
slot are excluded.  So the slot holds no proxy, and ``rcu_assign_pointer()``
publishes the update.

Writing a slot twice
--------------------

``rcu_txn_record()`` requires the records of a transaction to target distinct
slots.  A writer may need to write the same slot twice in one transaction.  It
reads the slot with ``rcu_txn_load()``, which returns the transaction's
pending value for it.  It records with ``rcu_txn_record_chain()``, which
updates the record that the slot already has::

	old = rcu_txn_load(&txn, slot, FOO_TAG);
	rcu_txn_record_chain(&txn, slot, old, new, FOO_TAG);

This is what allows several operations on one data structure to be composed in
one transaction.  The lookup among the transaction's records is a linear scan.
``rcu_txn_declare_disjoint()`` turns it off for a transaction whose slots are
known to be distinct.

Data structures
===============

Two data structures are built on the engine.  Each provides two kinds of
operations:

- ``_prepare`` operations record into a transaction that the caller owns.
- ``_rcu`` operations carry out a single operation by themselves.

Readers walk the structures with ``_rcu`` iterators.  A writer that excludes
the other writers may use the same iterators outside of an RCU read-side
critical section.  It then passes them a lockdep condition for that
exclusion, such as ``lockdep_is_held()`` of its lock.  This works as with
``list_for_each_entry_rcu()``.

``include/linux/rcu_txn_hlist.h``
	A list with a single-pointer head, like ``hlist``.  Its ``_prepare``
	operations compose.  Several nodes can be added to or removed from one
	or several lists atomically.  An ``_rcu`` operation re-points a
	single slot.  So it needs no transaction and allocates nothing.  From
	the commit on, a removed node is unhashed: its ``pprev`` is NULL.
	``rcu_txn_hlist_unhashed()`` reports this.

``include/linux/rcu_txn_list.h``
	A circular doubly-linked list.  Its forward and backward chains
	switch together, so readers can walk it in both directions.  An insertion or a removal switches a forward and a
	backward edge.  A move switches six edges, including the moved node's
	own two.  Each of these operations allocates a descriptor.  A reader
	standing on a moved node goes with it, and may skip or revisit other
	nodes.  For a walk that must see every node once, delete the node,
	wait for a grace period, then add it back.

Debugging
=========

``CONFIG_DEBUG_RCU_TXN`` enables the checks that cost more than a test of the
handle's state:

- It validates the tags.
- It reports a slot recorded twice.
- It validates the single-writer contract.  It warns when a task other than
  the one that initialized a handle drives it.  It also warns when a slot
  that a transaction records, installs to or settles was changed by another
  writer in the meantime.

A task may take over a handle that another task got ready, with nothing
recorded yet.  It then calls ``rcu_txn_adopt()`` first.  This makes it the
task that the check expects.

Misuse of a handle, such as recording into one that was already committed,
warns in every build.

Testing
=======

``CONFIG_RCU_TXN_KUNIT_TEST`` builds the KUnit suite ``rcu_txn``, which covers
the engine, both data structures and the debug checks::

	./tools/testing/kunit/kunit.py run --arch=x86_64 \
		--kconfig_add CONFIG_RCU_TXN_KUNIT_TEST=y \
		--kconfig_add CONFIG_DEBUG_KERNEL=y \
		--kconfig_add CONFIG_DEBUG_RCU_TXN=y \
		'rcu_txn*'

The litmus tests ``Documentation/litmus-tests/rcu/RCU-txn+*.litmus`` model the
memory ordering of the engine and of ``rcu_txn_list`` with the Linux kernel
memory model.  ``Documentation/litmus-tests/README`` describes them and their
error-injection variants.  Install herd7 as ``tools/memory-model/README``
says, then run::

	cd tools/memory-model
	scripts/checktheselitmus.sh --timeout 1h -- \
		../../Documentation/litmus-tests/rcu/RCU-txn+*.litmus

API
===

.. kernel-doc:: include/linux/rcu_txn.h

.. kernel-doc:: include/linux/rcu_txn_hlist.h

.. kernel-doc:: include/linux/rcu_txn_list.h
