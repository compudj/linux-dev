.. SPDX-License-Identifier: GPL-2.0

=====================================
Single-writer RCU pseudo-transactions
=====================================

An RCU pseudo-transaction switches a *set* of RCU-protected pointers from
their old values to their new values with a single store, as observed by
concurrent RCU readers: at any instant, every pointer of the set reads old, or
every pointer reads new.  A reader that follows several of them, each load
depending on the previous one, sees old values, then new ones, and never an
old value after a new one.

The implementation is in ``include/linux/rcu_txn.h`` and ``lib/rcu_txn.c``.
Code that uses it selects ``CONFIG_RCU_TXN``.

When to use it
==============

``rcu_assign_pointer()`` publishes one pointer atomically.  An update that
re-points several pointers one at a time exposes its intermediate states: a
reader that loads two of them may get the new value of one and then the old
value of the other.  Moving an object between two hash buckets is such an
update, and so is every insertion into or removal from a doubly-linked list.

A pseudo-transaction removes the intermediate states.  It does not replace the
update-side lock: the writers of a slot are excluded by the caller, and the
transaction neither detects nor resolves conflicts between them.  It is
"pseudo" in that sense: there is no read set and no abort, and readers get no
snapshot.

A pseudo-transaction is not free, on the update side above all.  The writer
allocates a descriptor for each transaction, with the ``gfp_t`` given to
``rcu_txn_init()``, and a transaction that switches two pointers or more frees
it with ``kfree_rcu()``, a grace period after its commit, as readers may still
be reading it: an allocation, which can fail, and a deferred free for every
update, where ``rcu_assign_pointer()`` needs neither.  The reader pays a test of
the low bits of every pointer it loads, which is nearly free: only while a
commit is in flight on that pointer does the reader also read the descriptor,
with three more loads, one of them an acquire.

How it works
============

The pointers a transaction switches are called *slots*: they are the *edges* of
the data structure that the update re-points.  The writer builds a *record* per
slot, holding the slot's old and new targets, and commits in three steps:

1. *Install.*  Each slot is made to point at its record, with tag bits set that
   mark the value as a *proxy* rather than a target.  A reader that finds a
   proxy resolves it through the record, which still answers with the old
   target.

2. *Commit.*  The records of a transaction share one word, the *selector* of
   their *flip group*.  The writer stores 1 to it.  From then on every proxy of
   the transaction resolves to its new target.

3. *Settle.*  Each slot is overwritten with its new target, and the records
   are freed after a grace period.

The selector is written once, from 0 to 1, so a reader that resolves several
slots of a transaction over time sees old targets, then new targets, and never
an old target after a new one, provided that each of its loads depends on the
previous one.  The header describes the memory ordering in detail.

A transaction of a single record installs no proxy: its commit is one
``rcu_assign_pointer()``.

Tags
====

The embedder chooses the tag that marks a proxy in its slots: typically bit 0,
or a reserved value of a type code that its pointers already carry.  The tag
must be within ``RCU_TXN_TAG_MASK``, the low bits that the alignment of a
record leaves clear, and no value that the embedder stores in a slot may carry
all of the tag's bits.

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

Here ``child`` moves from ``a->left`` to an empty ``b->right``: at any instant
it is in exactly one of the two places.

Slots are ``__rcu`` pointers.  ``rcu_txn_slot()`` converts the address of one,
of any type, to the ``void __rcu **`` the engine takes, and lets sparse check
the annotation.

A handle ends in ``rcu_txn_commit()`` or in ``rcu_txn_cancel()``.  Either
consumes it, and it must be initialized again before it is reused.

Allocation
----------

A transaction allocates a descriptor, with the ``gfp_t`` given to
``rcu_txn_init()``.  ``rcu_txn_reserve()`` allocates it for a given number of
records; otherwise the first ``rcu_txn_record()`` does, even for a single
record, and a later one may grow it.

A failed allocation is sticky: later records do nothing, and
``rcu_txn_commit()`` publishes nothing and returns ``-ENOMEM``.  A writer may
therefore ignore the return values of ``rcu_txn_reserve()`` and
``rcu_txn_record()`` and check that of ``rcu_txn_commit()`` only, as in the
example above.

A writer that records under a spinlock reserves its record bound before it
takes the lock, as above, or initializes the handle with a non-sleeping
``gfp_t``.  Under a raw or a bit spinlock only the reserve will do, since
``PREEMPT_RT`` does not allow the allocator there.  ``rcu_txn_commit()`` and
``rcu_txn_cancel()`` allocate nothing, do not sleep, and may be called under
such a lock.

An update that re-points a single slot needs no transaction.  Every
transaction settles its slots before its commit returns, and the writers of the
slot are excluded, so the slot holds no proxy: ``rcu_assign_pointer()``
publishes the update, as the ``_rcu`` operations of ``rcu_txn_hlist`` do.

Writing a slot twice
--------------------

``rcu_txn_record()`` requires the records of a transaction to target distinct
slots.  A writer that may write the same slot twice in one transaction reads
the slot with ``rcu_txn_load()``, which returns the transaction's pending value
for it, and records with ``rcu_txn_record_chain()``, which updates the record
the slot already has::

	old = rcu_txn_load(&txn, slot, FOO_TAG);
	rcu_txn_record_chain(&txn, slot, old, new, FOO_TAG);

This is what allows several operations on one data structure to be composed in
one transaction.  The lookup among the transaction's records is a linear scan,
behind a Bloom filter once the transaction holds enough of them.
``rcu_txn_declare_disjoint()`` turns the lookup off for a transaction whose
slots are known to be distinct.

Data structures
===============

Two data structures are built on the engine.  Each provides ``_prepare``
operations, which record into a transaction owned by the caller, and ``_rcu``
operations, which carry out a single operation by themselves.
Readers walk them with ``_rcu`` iterators.  A writer that excludes the others
walks them with the same iterators outside of an RCU read-side critical
section, if it passes them a lockdep condition for that exclusion, such as
``lockdep_is_held()`` of its lock, as with ``list_for_each_entry_rcu()``.

``include/linux/rcu_txn_hlist.h``
	A list with a single-pointer head, like ``hlist``.  Its ``_prepare``
	operations compose: several nodes can be added to or removed from one
	or several lists atomically.  An ``_rcu`` operation publishes its
	update with one ``rcu_assign_pointer()`` and allocates nothing.

``include/linux/rcu_txn_list.h``
	A circular doubly-linked list whose forward and backward chains are
	mutually coherent at every instant, which allows readers to walk it in
	both directions.  An insertion or a removal switches a forward and a
	backward edge, and a move six, the moved node's own included: each
	allocates a descriptor.

Debugging
=========

``CONFIG_DEBUG_RCU_TXN`` enables the checks that cost more than a test of the
handle's state.  It validates the tags, reports a slot recorded twice, and
validates the single-writer contract: it warns when a handle is driven by a
task other than the one that initialized it, and when a slot that a transaction
records, installs to or settles was changed by another writer in the meantime.
A task that takes over a handle which another got ready, with nothing recorded
yet, calls ``rcu_txn_adopt()`` first, which makes it the task the check
expects.

Misuse of a handle, such as recording into one that was already committed,
warns in every build.

Testing
=======

``CONFIG_RCU_TXN_KUNIT_TEST`` builds the KUnit suite ``rcu_txn``, which covers
the engine, both data structures and the debug checks, and runs one writer
against concurrent readers of a list::

	./tools/testing/kunit/kunit.py run --arch=x86_64 \
		--qemu_args="-smp 4" \
		--kconfig_add CONFIG_RCU_TXN_KUNIT_TEST=y \
		--kconfig_add CONFIG_DEBUG_KERNEL=y \
		--kconfig_add CONFIG_DEBUG_RCU_TXN=y \
		'rcu_txn*'

The litmus tests ``Documentation/litmus-tests/rcu/RCU-txn+*.litmus`` model the
memory ordering of the engine and of ``rcu_txn_list`` with the Linux kernel
memory model.  Each test checks an outcome that the code forbids, and herd7
finds that it never happens.  Error-injection variants validate that the model
catches a missing ordering: each drops one ordering that its test relies on, and
herd7 then finds the outcome possible.  With herd7 installed as
``tools/memory-model/README`` says::

	cd tools/memory-model
	scripts/checktheselitmus.sh --timeout 1h -- \
		../../Documentation/litmus-tests/rcu/RCU-txn+*.litmus

API
===

.. kernel-doc:: include/linux/rcu_txn.h

.. kernel-doc:: include/linux/rcu_txn_hlist.h

.. kernel-doc:: include/linux/rcu_txn_list.h
