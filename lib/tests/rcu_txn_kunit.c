// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * KUnit tests of the single-writer RCU pseudo-transactions
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <kunit/test.h>
#include <linux/bug.h>
#include <linux/rcu_txn.h>
#include <linux/rcu_txn_hlist.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>
#include <linux/string.h>

#include "../rcu_txn_internal.h"	/* the steps of a commit */

/* Bit 0 of the proxy pointer: no value below carries it. */
#define TAG	1UL

/* Opaque slot values with the tag bit clear. */
#define T(n)	((void *)(((unsigned long)(n) + 1) << 8))

/*
 * Without __WARN_FLAGS, a WARN_ON_ONCE() of the engine fires once per boot.
 * A later run of the suite then sees none, so check for at most one.  With
 * __WARN_FLAGS, the once state is in the bug table, and KUnit counts every
 * suppressed warning.
 */
#ifdef __WARN_FLAGS
# define EXPECT_ONE_WARNING(test)					\
	KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1)
#else
# define EXPECT_ONE_WARNING(test)					\
	KUNIT_EXPECT_LE(test, KUNIT_SUPPRESSED_WARNING_COUNT(), 1)
#endif

/*
 * Each test is the only writer of the structures it builds.  It reads them
 * as a writer, outside of any RCU read-side critical section.  It uses
 * rcu_access_pointer(), or a form that takes a lockdep condition, with true.
 *
 * Some tests read as a reader instead, through read_slot() below.  They check
 * what they read only after rcu_read_unlock(), because a failed
 * KUNIT_EXPECT_*() may sleep: it allocates with GFP_KERNEL.
 */

/* Load @slot and resolve it, as a reader does. */
static void *read_slot(void __rcu **slot)
{
	void *v;

	rcu_read_lock();
	v = rcu_txn_resolve(rcu_dereference(*slot), TAG);
	rcu_read_unlock();
	return v;		/* a test value: never dereferenced */
}

/* Load @slot with a load-acquire and resolve it, as a reader does. */
static void *read_slot_acquire(void __rcu **slot)
{
	void *v;

	rcu_read_lock();
	v = rcu_txn_dereference_acquire(*slot, TAG);
	rcu_read_unlock();
	return v;		/* a test value: never dereferenced */
}

/* One record: the commit stores the new target directly, with no proxy. */
static void rcu_txn_test_single_record(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL);
	KUNIT_EXPECT_TRUE(test, rcu_txn_reserve(&txn, 4));
	rcu_txn_record(&txn, &slot, T(0), T(1), TAG);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(slot), T(1));
	KUNIT_EXPECT_FALSE(test,
			   rcu_txn_is_proxy(rcu_access_pointer(slot), TAG));
}

/* Two records or more: the commit uses proxies.  Check the settled values. */
static void rcu_txn_test_multi_record(struct kunit *test)
{
	void __rcu *s1 = RCU_INITIALIZER(T(0));
	void __rcu *s2 = RCU_INITIALIZER(T(1));
	void __rcu *s3 = RCU_INITIALIZER(T(2));
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_reserve(&txn, 4);
	rcu_txn_record(&txn, &s1, T(0), T(10), TAG);
	rcu_txn_record(&txn, &s2, T(1), T(11), TAG);
	rcu_txn_record(&txn, &s3, T(2), T(12), TAG);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s1), T(10));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s2), T(11));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s3), T(12));
}

/*
 * Run the three steps of a commit by hand.  Check what a reader resolves
 * after each one.
 */
static void rcu_txn_test_commit_steps(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn_group group;
	struct rcu_txn_record rec;

	__rcu_txn_group_init(&group);
	__rcu_txn_record_set(&rec, &slot, T(0), T(1), TAG);
	__rcu_txn_record_install(&group, &rec);
	KUNIT_EXPECT_TRUE(test,
			  rcu_txn_is_proxy(rcu_access_pointer(slot), TAG));
	KUNIT_EXPECT_PTR_EQ(test, read_slot(&slot), T(0));
	KUNIT_EXPECT_PTR_EQ(test, read_slot_acquire(&slot), T(0));
	__rcu_txn_group_commit(&group);
	KUNIT_EXPECT_TRUE(test,
			  rcu_txn_is_proxy(rcu_access_pointer(slot), TAG));
	KUNIT_EXPECT_PTR_EQ(test, read_slot(&slot), T(1));
	KUNIT_EXPECT_PTR_EQ(test, read_slot_acquire(&slot), T(1));
	__rcu_txn_record_settle(&rec);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(slot), T(1));
	KUNIT_EXPECT_PTR_EQ(test, read_slot_acquire(&slot), T(1));
}

struct item {
	struct item __rcu *next;
	int key;
};

/*
 * Use a typed __rcu pointer as a slot.  rcu_txn_slot() hands its address to
 * the engine.  rcu_txn_dereference_check() reads it back with its type,
 * through a proxy or not.
 */
static void rcu_txn_test_typed_slot(struct kunit *test)
{
	struct item a = { .key = 1 }, b = { .key = 2 }, c = { .key = 3 };
	struct item __rcu *head = RCU_INITIALIZER(&a);
	struct rcu_txn_group group;
	struct rcu_txn_record rec;
	struct rcu_txn txn;
	struct item *it;

	/* Through an installed proxy, before and after the flip. */
	__rcu_txn_group_init(&group);
	__rcu_txn_record_set(&rec, rcu_txn_slot(&head), &a, &b, TAG);
	__rcu_txn_record_install(&group, &rec);
	it = rcu_txn_dereference_check(head, TAG, true);
	KUNIT_EXPECT_PTR_EQ(test, it, &a);
	__rcu_txn_group_commit(&group);
	it = rcu_txn_dereference_check(head, TAG, true);
	KUNIT_EXPECT_PTR_EQ(test, it, &b);
	__rcu_txn_record_settle(&rec);

	/* Two typed slots in one transaction: head -> c -> b. */
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_record(&txn, rcu_txn_slot(&head), &b, &c, TAG);
	rcu_txn_record(&txn, rcu_txn_slot(&c.next), NULL, &b, TAG);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	it = rcu_txn_dereference_check(head, TAG, true);
	KUNIT_EXPECT_EQ(test, it->key, 3);
	it = rcu_txn_dereference_check(it->next, TAG, true);
	KUNIT_EXPECT_EQ(test, it->key, 2);
	KUNIT_EXPECT_NULL(test, rcu_txn_dereference_check(it->next, TAG, true));
}

/* Record @nr edges on distinct slots.  Return false if a record() failed. */
static bool record_all(struct rcu_txn *txn, void __rcu **slots, unsigned int nr)
{
	bool all_recorded = true;
	unsigned int i;

	for (i = 0; i < nr; i++) {
		RCU_INIT_POINTER(slots[i], T(i));
		if (!rcu_txn_record(txn, &slots[i], T(i), T(100 + i), TAG))
			all_recorded = false;
	}
	return all_recorded;
}

static bool all_settled(void __rcu **slots, unsigned int nr)
{
	unsigned int i;

	for (i = 0; i < nr; i++)
		if (rcu_access_pointer(slots[i]) != T(100 + i))
			return false;
	return true;
}

/*
 * Record more edges than the first capacity without a reserve(): the
 * descriptor grows, and the commit settles the whole set.
 */
static void rcu_txn_test_grow(struct kunit *test)
{
	enum { NR = 3 * RCU_TXN_CAP + 1 };
	void __rcu *slots[NR];
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL);
	KUNIT_EXPECT_TRUE(test, record_all(&txn, slots, 1));
	KUNIT_EXPECT_GE(test, txn.cap, RCU_TXN_CAP);
	rcu_txn_cancel(&txn);

	rcu_txn_init(&txn, GFP_KERNEL);
	KUNIT_EXPECT_TRUE(test, record_all(&txn, slots, NR));
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	KUNIT_EXPECT_TRUE(test, all_settled(slots, NR));
}

/*
 * reserve() gives the handle room for its bound, and recording up to the bound
 * does not move the records.  Recording past it grows the descriptor.
 */
static void rcu_txn_test_reserve(struct kunit *test)
{
	enum { NR = 2 * RCU_TXN_CAP + 1 };
	struct rcu_txn_record *records;
	void __rcu *slots[NR];
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL);
	KUNIT_EXPECT_TRUE(test, rcu_txn_reserve(&txn, 2));
	KUNIT_EXPECT_GE(test, txn.cap, 2);
	records = txn.records;
	KUNIT_EXPECT_TRUE(test, record_all(&txn, slots, 2));
	KUNIT_EXPECT_PTR_EQ(test, txn.records, records);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	KUNIT_EXPECT_TRUE(test, all_settled(slots, 2));

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_reserve(&txn, 2);
	KUNIT_EXPECT_TRUE(test, record_all(&txn, slots, NR));
	KUNIT_EXPECT_GE(test, txn.cap, NR);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	KUNIT_EXPECT_TRUE(test, all_settled(slots, NR));
}

/* A single-record transaction installs no proxy, and may use a zero tag. */
static void rcu_txn_test_zero_tag(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL);
	kunit_warning_suppress(test) {
		rcu_txn_record(&txn, &slot, T(0), T(1), 0);
		KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 0);
	}
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(slot), T(1));
}

/*
 * A cancelled transaction publishes nothing and consumes the handle, which
 * commits as usual once initialized again.
 */
static void rcu_txn_test_cancel(struct kunit *test)
{
	void __rcu *s1 = RCU_INITIALIZER(T(0));
	void __rcu *s2 = RCU_INITIALIZER(T(1));
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_record(&txn, &s1, T(0), T(10), TAG);
	rcu_txn_record(&txn, &s2, T(1), T(11), TAG);
	rcu_txn_cancel(&txn);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s1), T(0));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s2), T(1));
	KUNIT_EXPECT_NULL(test, txn.desc);
	KUNIT_EXPECT_NULL(test, txn.records);
	KUNIT_EXPECT_EQ(test, txn.state, RCU_TXN_DONE);

	/* A handle that recorded nothing. */
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_cancel(&txn);
	KUNIT_EXPECT_EQ(test, txn.state, RCU_TXN_DONE);

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_record(&txn, &s1, T(0), T(10), TAG);
	rcu_txn_record(&txn, &s2, T(1), T(11), TAG);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s1), T(10));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s2), T(11));
}

/*
 * Set the sticky OOM state by hand, since no allocation of this size fails on
 * demand.  Later records do nothing.  commit() and cancel() both release the
 * handle without publishing.
 */
static void rcu_txn_test_oom(struct kunit *test)
{
	void __rcu *s1 = RCU_INITIALIZER(T(0));
	void __rcu *s2 = RCU_INITIALIZER(T(1));
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_record(&txn, &s1, T(0), T(10), TAG);
	txn.state = RCU_TXN_OOM;
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_FALSE(test, rcu_txn_reserve(&txn, 8));
		KUNIT_EXPECT_FALSE(test,
				   rcu_txn_record(&txn, &s2, T(1), T(11), TAG));
		KUNIT_EXPECT_FALSE(test,
				   rcu_txn_record_chain(&txn, &s2, T(1), T(11),
							TAG));
		KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), -ENOMEM);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 0);
	}
	KUNIT_EXPECT_NULL(test, txn.desc);
	KUNIT_EXPECT_EQ(test, txn.state, RCU_TXN_DONE);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s1), T(0));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s2), T(1));

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_record(&txn, &s1, T(0), T(10), TAG);
	rcu_txn_record(&txn, &s2, T(1), T(11), TAG);
	txn.state = RCU_TXN_OOM;
	rcu_txn_cancel(&txn);
	KUNIT_EXPECT_NULL(test, txn.desc);
	KUNIT_EXPECT_EQ(test, txn.state, RCU_TXN_DONE);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s1), T(0));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s2), T(1));

	/* declare_disjoint() leaves a handle whose reserve() failed as it is. */
	rcu_txn_init(&txn, GFP_KERNEL);
	txn.state = RCU_TXN_OOM;
	kunit_warning_suppress(test) {
		rcu_txn_declare_disjoint(&txn);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 0);
	}
	KUNIT_EXPECT_FALSE(test, txn.disjoint);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), -ENOMEM);
}

/*
 * A reserve too large for the allocator fails for real.  The failure is
 * sticky, and the commit publishes nothing.
 */
static void rcu_txn_test_alloc_fail(struct kunit *test)
{
	void __rcu *s1 = RCU_INITIALIZER(T(0));
	void __rcu *s2 = RCU_INITIALIZER(T(1));
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL | __GFP_NOWARN);
	KUNIT_EXPECT_TRUE(test, rcu_txn_record(&txn, &s1, T(0), T(10), TAG));
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_FALSE(test, rcu_txn_reserve(&txn, 1U << 30));
		KUNIT_EXPECT_EQ(test, txn.state, RCU_TXN_OOM);
		KUNIT_EXPECT_FALSE(test,
				   rcu_txn_record(&txn, &s2, T(1), T(11), TAG));
		KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), -ENOMEM);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 0);
	}
	KUNIT_EXPECT_NULL(test, txn.desc);
	KUNIT_EXPECT_EQ(test, txn.state, RCU_TXN_DONE);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s1), T(0));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s2), T(1));
}

/*
 * load() returns the transaction's pending value of a recorded slot, and
 * record_chain() fuses a second store to it into the same record.
 */
static void rcu_txn_test_ryw_chain(struct kunit *test)
{
	enum { NR = 6 };
	void __rcu *other = RCU_INITIALIZER(T(50));
	void __rcu *slots[NR];
	struct rcu_txn txn;
	unsigned int i;

	for (i = 0; i < NR; i++)
		RCU_INIT_POINTER(slots[i], T(i));
	rcu_txn_init(&txn, GFP_KERNEL);
	for (i = 0; i < NR; i++) {
		void *v = rcu_txn_load(&txn, &slots[i], TAG);

		KUNIT_EXPECT_PTR_EQ(test, v, T(i));
		rcu_txn_record_chain(&txn, &slots[i], v, T(20 + i), TAG);
	}
	/* Every slot is recorded: loads return the pending values. */
	for (i = 0; i < NR; i++) {
		void *v = rcu_txn_load(&txn, &slots[i], TAG);

		KUNIT_EXPECT_PTR_EQ(test, v, T(20 + i));
		rcu_txn_record_chain(&txn, &slots[i], v, T(100 + i), TAG);
	}
	KUNIT_EXPECT_EQ(test, txn.nr, NR);		/* fused */
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_load(&txn, &other, TAG), T(50));
	for (i = 0; i < NR; i++)		/* nothing is published yet */
		KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(slots[i]), T(i));
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	KUNIT_EXPECT_TRUE(test, all_settled(slots, NR));
}

/*
 * A handle declared disjoint skips the lookup: record_chain() appends, and
 * load() returns the committed value.
 */
static void rcu_txn_test_disjoint(struct kunit *test)
{
	void __rcu *a = RCU_INITIALIZER(T(0));
	void __rcu *b = RCU_INITIALIZER(T(1));
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_declare_disjoint(&txn);
	rcu_txn_record_chain(&txn, &a, rcu_txn_load(&txn, &a, TAG), T(10), TAG);
	rcu_txn_record_chain(&txn, &b, rcu_txn_load(&txn, &b, TAG), T(11), TAG);
	KUNIT_EXPECT_EQ(test, txn.nr, 2);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(a), T(10));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(b), T(11));
}

/*
 * A handle that commit() consumed must be initialized again.  Each entry
 * point warns and does nothing.
 */
static void rcu_txn_test_consumed_handle(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_record(&txn, &slot, T(0), T(1), TAG);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);

	kunit_warning_suppress(test) {
		KUNIT_EXPECT_FALSE(test, rcu_txn_reserve(&txn, 4));
		EXPECT_ONE_WARNING(test);
	}
	kunit_warning_suppress(test) {
		bool recorded = rcu_txn_record(&txn, &slot, T(1), T(2), TAG);

		KUNIT_EXPECT_FALSE(test, recorded);
		EXPECT_ONE_WARNING(test);
	}
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_FALSE(test,
				   rcu_txn_record_chain(&txn, &slot, T(1), T(2),
							TAG));
		EXPECT_ONE_WARNING(test);
	}
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_PTR_EQ(test, rcu_txn_load(&txn, &slot, TAG), T(1));
		EXPECT_ONE_WARNING(test);
	}
	kunit_warning_suppress(test) {
		rcu_txn_declare_disjoint(&txn);
		KUNIT_EXPECT_FALSE(test, txn.disjoint);
		EXPECT_ONE_WARNING(test);
	}
	kunit_warning_suppress(test) {
		rcu_txn_cancel(&txn);
		EXPECT_ONE_WARNING(test);
	}
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), -EINVAL);
		EXPECT_ONE_WARNING(test);
	}
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(slot), T(1));
}

/*
 * CONFIG_DEBUG_RCU_TXN
 *
 * The cases below check that the debug checks report each way to break the
 * contract.  They also check that a correct transaction and a correct adopt
 * do not warn.
 *
 * The writer/writer races are staged in one task.  The test stores the other
 * writer's proxy or committed value in the slot by hand, as a real race would
 * leave it.
 */

static void need_debug(struct kunit *test)
{
	if (!IS_ENABLED(CONFIG_DEBUG_RCU_TXN))
		kunit_skip(test, "CONFIG_DEBUG_RCU_TXN is off");
}

#ifdef CONFIG_DEBUG_RCU_TXN
static void set_owner(struct rcu_txn *t, struct task_struct *task)
{
	t->owner = task;
}
#else
static void set_owner(struct rcu_txn *t, struct task_struct *task)
{
}
#endif

/*
 * Install a proxy into @slot by hand, as a writer stopped between its install
 * and its flip leaves it.
 */
static void install_by_hand(struct rcu_txn_group *group,
			    struct rcu_txn_record *rec, void __rcu **slot,
			    void *old_ptr, void *new_ptr)
{
	__rcu_txn_group_init(group);
	__rcu_txn_record_set(rec, slot, old_ptr, new_ptr, TAG);
	__rcu_txn_record_install(group, rec);
}

/* A well-formed single-writer transaction trips nothing. */
static void rcu_txn_test_debug_control(struct kunit *test)
{
	void __rcu *a = RCU_INITIALIZER(T(0));
	void __rcu *b = RCU_INITIALIZER(T(0));
	struct rcu_txn txn;
	unsigned int i;

	need_debug(test);
	kunit_warning_suppress(test) {
		for (i = 0; i < 100; i++) {
			void *to = T(1 + i), *from = T(i);

			rcu_txn_init(&txn, GFP_KERNEL);
			rcu_txn_record(&txn, &a, from, to, TAG);
			rcu_txn_record_chain(&txn, &b,
					     rcu_txn_load(&txn, &b, TAG), to,
					     TAG);
			KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
		}
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 0);
	}
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(a), T(100));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(b), T(100));
}

/* A handle driven by a task other than the one that initialized it. */
static void rcu_txn_test_debug_owner(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn txn;

	need_debug(test);
	rcu_txn_init(&txn, GFP_KERNEL);
	set_owner(&txn, &init_task);	/* as if another task initialized it */
	kunit_warning_suppress(test) {
		rcu_txn_record(&txn, &slot, T(0), T(1), TAG);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	set_owner(&txn, current);
	rcu_txn_cancel(&txn);
}

/*
 * adopt() makes the calling task the owner of a handle that another task
 * initialized.  It warns if the handle already holds records.
 */
static void rcu_txn_test_debug_adopt(struct kunit *test)
{
	void __rcu *a = RCU_INITIALIZER(T(0));
	void __rcu *b = RCU_INITIALIZER(T(0));
	struct rcu_txn txn;

	need_debug(test);
	rcu_txn_init(&txn, GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, rcu_txn_reserve(&txn, 2));
	set_owner(&txn, &init_task);	/* as if another task got it ready */
	kunit_warning_suppress(test) {
		rcu_txn_adopt(&txn);
		rcu_txn_record(&txn, &a, T(0), T(1), TAG);
		rcu_txn_record(&txn, &b, T(0), T(1), TAG);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 0);
		rcu_txn_adopt(&txn);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
		KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	}
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(a), T(1));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(b), T(1));
}

/* A second writer records a slot that holds the first writer's proxy. */
static void rcu_txn_test_debug_record_over_proxy(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn_group group;
	struct rcu_txn_record rec;
	struct rcu_txn txn;

	need_debug(test);
	install_by_hand(&group, &rec, &slot, T(0), T(1));
	rcu_txn_init(&txn, GFP_KERNEL);
	kunit_warning_suppress(test) {
		rcu_txn_record(&txn, &slot, T(1), T(2), TAG);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	rcu_txn_cancel(&txn);
}

/*
 * A second writer installs onto a slot that holds the first writer's proxy.
 * It recorded the slot before that proxy was installed.  Both install checks
 * warn: the slot holds a proxy, and not the recorded old value.
 */
static void rcu_txn_test_debug_install_over_proxy(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn_record rec, other;
	struct rcu_txn_group group;

	need_debug(test);
	__rcu_txn_record_set(&rec, &slot, T(0), T(2), TAG);
	install_by_hand(&group, &other, &slot, T(0), T(1));
	kunit_warning_suppress(test) {
		__rcu_txn_excl_slot_installable(&rec);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 2);
	}
}

/*
 * Another writer committed on the slot between our record() and our install:
 * see __rcu_txn_excl_slot_installable().
 */
static void rcu_txn_test_debug_install_over_settled(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn_record rec;

	need_debug(test);
	__rcu_txn_record_set(&rec, &slot, T(0), T(1), TAG);
	rcu_assign_pointer(slot, T(2));		/* a peer committed 0 -> 2 */
	kunit_warning_suppress(test) {
		__rcu_txn_excl_slot_installable(&rec);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
}

/* Our proxy is gone by the time we settle: someone overwrote it. */
static void rcu_txn_test_debug_settle_clobbered(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn_group group;
	struct rcu_txn_record rec;

	need_debug(test);
	/* Stands for our install. */
	install_by_hand(&group, &rec, &slot, T(0), T(1));
	rcu_assign_pointer(slot, T(2));		/* a peer overwrites it */
	kunit_warning_suppress(test) {
		__rcu_txn_excl_slot_ours(&rec);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
}

/*
 * Another writer changed the slot between our record() and our single-record
 * commit: see __rcu_txn_excl_slot_unchanged().
 */
static void rcu_txn_test_debug_single_record_changed(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn txn;

	need_debug(test);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_record(&txn, &slot, T(0), T(1), TAG);
	rcu_assign_pointer(slot, T(2));		/* a peer changed it under us */
	kunit_warning_suppress(test) {
		rcu_txn_commit(&txn);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
}

/* One slot recorded twice, not adjacently: see rcu_txn_record(). */
static void rcu_txn_test_debug_duplicate_slot(struct kunit *test)
{
	void __rcu *a = RCU_INITIALIZER(T(0));
	void __rcu *b = RCU_INITIALIZER(T(1));
	struct rcu_txn_record recs[3];

	need_debug(test);
	__rcu_txn_record_set(&recs[0], &a, T(0), T(10), TAG);
	__rcu_txn_record_set(&recs[1], &b, T(1), T(11), TAG);
	__rcu_txn_record_set(&recs[2], &a, T(10), T(12), TAG);
	kunit_warning_suppress(test) {
		__rcu_txn_assert_distinct_slots(recs, ARRAY_SIZE(recs));
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
}

/* record() warns about an old or a new value that carries the slot's tag. */
static void rcu_txn_test_debug_tagged_value(struct kunit *test)
{
	void *tagged = (void *)((unsigned long)T(1) | TAG);
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn txn;

	need_debug(test);
	rcu_txn_init(&txn, GFP_KERNEL);
	kunit_warning_suppress(test) {
		rcu_txn_record(&txn, &slot, T(0), tagged, TAG);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	rcu_txn_cancel(&txn);

	rcu_txn_init(&txn, GFP_KERNEL);
	kunit_warning_suppress(test) {
		rcu_txn_record(&txn, &slot, tagged, T(2), TAG);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	rcu_txn_cancel(&txn);
}

/* A proxy cannot be installed under a zero tag: nothing would mark it. */
static void rcu_txn_test_debug_install_zero_tag(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn_group group;
	struct rcu_txn_record rec;

	need_debug(test);
	__rcu_txn_group_init(&group);
	__rcu_txn_record_set(&rec, &slot, T(0), T(1), 0);
	kunit_warning_suppress(test) {
		__rcu_txn_record_install(&group, &rec);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
}

/*
 * On a handle declared disjoint, load and record a slot that is already
 * recorded.  Both warn: see __rcu_txn_disjoint_check().
 */
static void rcu_txn_test_debug_disjoint(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn txn;

	need_debug(test);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_declare_disjoint(&txn);
	rcu_txn_record_chain(&txn, &slot, T(0), T(1), TAG);
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_PTR_EQ(test, rcu_txn_load(&txn, &slot, TAG), T(0));
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	kunit_warning_suppress(test) {
		rcu_txn_record_chain(&txn, &slot, T(0), T(2), TAG);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	rcu_txn_cancel(&txn);
}

/* record_chain() warns about an old value that is not the pending one. */
static void rcu_txn_test_debug_stale_chain(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn txn;

	need_debug(test);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_record_chain(&txn, &slot, T(0), T(1), TAG);
	kunit_warning_suppress(test) {
		rcu_txn_record_chain(&txn, &slot, T(0), T(2), TAG);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	rcu_txn_cancel(&txn);
}

/* load() finds another writer's proxy in a slot it has not recorded. */
static void rcu_txn_test_debug_load_proxy(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn_group group;
	struct rcu_txn_record rec;
	struct rcu_txn txn;

	need_debug(test);
	install_by_hand(&group, &rec, &slot, T(0), T(1));
	rcu_txn_init(&txn, GFP_KERNEL);
	kunit_warning_suppress(test) {
		rcu_txn_load(&txn, &slot, TAG);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	rcu_txn_cancel(&txn);
}

/*
 * <linux/rcu_txn_hlist.h>
 */

struct hn {
	struct rcu_txn_hlist_node node;
	int key;
};

/* Freed with the test: a removed node stays valid for any reader. */
static struct rcu_txn_hlist_node *hn_new(struct kunit *test, int key)
{
	struct hn *n = kunit_kzalloc(test, sizeof(*n), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, n);
	n->key = key;
	rcu_txn_hlist_node_init(&n->node);
	return &n->node;
}

/* Build the list @keys by prepending in reverse. */
static void hlist_build(struct kunit *test, struct rcu_txn_hlist_head *head,
			const int *keys, int nr)
{
	int i;

	for (i = nr - 1; i >= 0; i--)
		rcu_txn_hlist_add_head_rcu(hn_new(test, keys[i]), head);
}

/* The test is the chain's only writer, and owns every node until it ends. */
static struct rcu_txn_hlist_node *hlist_find(struct rcu_txn_hlist_head *head,
					     int key)
{
	struct hn *n;

	rcu_txn_hlist_for_each_entry_rcu(n, head, node, true)
		if (n->key == key)
			return &n->node;
	return NULL;
}

/*
 * Check that the forward walk gives @want.  Also check that each node's pprev
 * points to the slot that points to the node.  That slot is &head->first for
 * the first node, and &prev->next for the others.  A torn pprev is invisible
 * to the forward walk but breaks the next edit, hence the separate check.
 */
static void hlist_expect(struct kunit *test, struct rcu_txn_hlist_head *head,
			 const int *want, int nr)
{
	struct rcu_txn_hlist_node __rcu **slot = &head->first;
	struct rcu_txn_hlist_node *p;
	int i = 0;

	rcu_txn_hlist_for_each_rcu(p, head, true) {
		if (i < nr)
			KUNIT_EXPECT_EQ(test,
					container_of(p, struct hn, node)->key,
					want[i]);
		KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(p->pprev), slot);
		slot = &p->next;
		i++;
	}
	KUNIT_EXPECT_EQ(test, i, nr);
}

static void rcu_txn_test_hlist_add(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3 };
	static const int behind[] = { 1, 2, 9, 3 };
	static const int before[] = { 1, 8, 2, 9, 3 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *fresh = hn_new(test, 0);

	KUNIT_EXPECT_TRUE(test, rcu_txn_hlist_empty(&head, true));
	KUNIT_EXPECT_TRUE(test, rcu_txn_hlist_unhashed(fresh, true));
	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	hlist_expect(test, &head, keys, ARRAY_SIZE(keys));
	fresh = hlist_find(&head, 2);
	KUNIT_EXPECT_FALSE(test, rcu_txn_hlist_unhashed(fresh, true));

	rcu_txn_hlist_add_behind_rcu(hn_new(test, 9), hlist_find(&head, 2));
	hlist_expect(test, &head, behind, ARRAY_SIZE(behind));

	rcu_txn_hlist_add_before_rcu(hn_new(test, 8), hlist_find(&head, 2));
	hlist_expect(test, &head, before, ARRAY_SIZE(before));
}

static void rcu_txn_test_hlist_del(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3, 4, 5 };
	static const int middle[] = { 1, 2, 4, 5 };
	static const int ends[] = { 2, 4 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *removed;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	removed = hlist_find(&head, 3);
	rcu_txn_hlist_del_rcu(removed);
	hlist_expect(test, &head, middle, ARRAY_SIZE(middle));
	/* A reader standing on the removed node still reaches the chain. */
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_hlist_next_check(removed, true),
			    hlist_find(&head, 4));
	KUNIT_EXPECT_TRUE(test, rcu_txn_hlist_unhashed(removed, true));

	rcu_txn_hlist_del_rcu(hlist_find(&head, 1));	/* first */
	rcu_txn_hlist_del_rcu(hlist_find(&head, 5));	/* last */
	hlist_expect(test, &head, ends, ARRAY_SIZE(ends));
}

static void rcu_txn_test_hlist_replace(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3 };
	static const int want[] = { 1, 22, 3 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *old;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	old = hlist_find(&head, 2);
	rcu_txn_hlist_replace_rcu(old, hn_new(test, 22));
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
	KUNIT_EXPECT_TRUE(test, rcu_txn_hlist_unhashed(old, true));
}

/*
 * Compose edits on one bucket whose neighbourhoods touch.  This is why the
 * _prepare forms read their own writes.
 */

/* Delete the adjacent 2 and 3 from 1..5 in one transaction. */
static void rcu_txn_test_hlist_compose_adjacent_del(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3, 4, 5 };
	static const int want[] = { 1, 4, 5 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *e2, *e3;
	struct rcu_txn txn;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	e2 = hlist_find(&head, 2);
	e3 = hlist_find(&head, 3);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_hlist_del_prepare(&txn, e2);
	rcu_txn_hlist_del_prepare(&txn, e3);	/* the neighbour of e2 */
	/* The records: &1->next (chained), and the pprev of 2, 3 and 4. */
	KUNIT_EXPECT_EQ(test, txn.nr, 4);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
}

/* Delete 2 and insert 9 into the same gap, behind 1, in one transaction. */
static void rcu_txn_test_hlist_compose_del_and_add(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3 };
	static const int want[] = { 1, 9, 3 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *e1, *e2;
	struct rcu_txn txn;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	e1 = hlist_find(&head, 1);
	e2 = hlist_find(&head, 2);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_hlist_del_prepare(&txn, e2);
	rcu_txn_hlist_add_behind_prepare(&txn, hn_new(test, 9), e1);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
}

/* Three consecutive deletes in one transaction: &1->next is chained twice. */
static void rcu_txn_test_hlist_compose_triple_del(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3, 4, 5 };
	static const int want[] = { 1, 5 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *e[3];
	struct rcu_txn txn;
	int i;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	for (i = 0; i < ARRAY_SIZE(e); i++)
		e[i] = hlist_find(&head, i + 2);	/* 2, 3, 4 */
	rcu_txn_init(&txn, GFP_KERNEL);
	for (i = 0; i < ARRAY_SIZE(e); i++)
		rcu_txn_hlist_del_prepare(&txn, e[i]);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * Insert 9 behind 1, then delete 1.  The fresh node inherits &1->next as the
 * slot that points to it.  The next operation removes the node that holds
 * that slot.  add_behind must have recorded &9->next as 2's pprev.  del(1)
 * must then re-point head->first to the pending 9, not to the committed 2.
 * It must also record &head->first as 9's pprev.
 *
 * A wrong pprev for 2 leaves the forward order intact here.  Only the pprev
 * check catches it.
 */
static void rcu_txn_test_hlist_compose_add_then_del_host(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3 };
	static const int want[] = { 9, 2, 3 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *e1;
	struct rcu_txn txn;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	e1 = hlist_find(&head, 1);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_hlist_add_behind_prepare(&txn, hn_new(test, 9), e1);
	rcu_txn_hlist_del_prepare(&txn, e1);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * Delete the first node, then insert before its successor.  add_before loads
 * 2's pprev as the transaction leaves it, &head->first, which del(1) recorded.
 * Reading 2's committed pprev, &1->next, instead would link 9 behind the
 * removed node 1.  The bucket would then hold 2, 3, without 9.
 */
static void rcu_txn_test_hlist_compose_del_then_add_before(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3 };
	static const int want[] = { 9, 2, 3 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *e1, *e2;
	struct rcu_txn txn;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	e1 = hlist_find(&head, 1);
	e2 = hlist_find(&head, 2);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_hlist_del_prepare(&txn, e1);
	rcu_txn_hlist_add_before_prepare(&txn, hn_new(test, 9), e2);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * Two buckets in one transaction: a node leaves one and its replacement
 * enters the other.  The edges are installed as proxies, so that at any instant
 * the key is in exactly one bucket.
 */
static void rcu_txn_test_hlist_move(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3 };
	static const int from[] = { 1, 3 };
	static const int to[] = { 2 };
	struct rcu_txn_hlist_head a = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_head b = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn txn;

	hlist_build(test, &a, keys, ARRAY_SIZE(keys));
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_hlist_del_prepare(&txn, hlist_find(&a, 2));
	rcu_txn_hlist_add_head_prepare(&txn, hn_new(test, 2), &b);
	/* The two forward edges, and the pprev of both 2s and of 3. */
	KUNIT_EXPECT_EQ(test, txn.nr, 5);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	hlist_expect(test, &a, from, ARRAY_SIZE(from));
	hlist_expect(test, &b, to, ARRAY_SIZE(to));
}

/*
 * One _prepare alone.  A delete records three edges: its forward edge, the
 * successor's pprev and its own pprev.  It commits through the descriptor.
 * The removed node is unhashed from the commit on, not before.
 */
static void rcu_txn_test_hlist_prepare_alone(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3 };
	static const int want[] = { 1, 3 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *e2;
	struct rcu_txn txn;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	e2 = hlist_find(&head, 2);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_hlist_del_prepare(&txn, e2);
	KUNIT_EXPECT_EQ(test, txn.nr, 3);
	KUNIT_EXPECT_FALSE(test, rcu_txn_hlist_unhashed(e2, true));
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
	KUNIT_EXPECT_TRUE(test, rcu_txn_hlist_unhashed(e2, true));
}

/*
 * Make every kind of edit in one transaction over 1..5:
 * del(2), add_behind(9, 1), replace(4, 44) and add_before(8, 5).
 * The last one re-points the slot that the replace gave to 5.  That slot is
 * inside the fresh node 44.  So add_before loads 5's pending pprev to find it.
 */
static void hlist_prepare_mixed(struct kunit *test, struct rcu_txn *txn,
				struct rcu_txn_hlist_head *head)
{
	struct rcu_txn_hlist_node *e1 = hlist_find(head, 1);
	struct rcu_txn_hlist_node *e2 = hlist_find(head, 2);
	struct rcu_txn_hlist_node *e4 = hlist_find(head, 4);
	struct rcu_txn_hlist_node *e5 = hlist_find(head, 5);

	rcu_txn_hlist_del_prepare(txn, e2);
	rcu_txn_hlist_add_behind_prepare(txn, hn_new(test, 9), e1);
	rcu_txn_hlist_replace_prepare(txn, e4, hn_new(test, 44));
	rcu_txn_hlist_add_before_prepare(txn, hn_new(test, 8), e5);
}

static void rcu_txn_test_hlist_compose_mixed(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3, 4, 5 };
	static const int want[] = { 1, 9, 3, 44, 8, 5 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn txn;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	rcu_txn_init(&txn, GFP_KERNEL);
	hlist_prepare_mixed(test, &txn, &head);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * Cancel the same transaction.  Nothing was written outside it.  So the chain
 * and every pprev are as they were.  A later single update then finds the
 * pprev it relies on.  The nodes that the adds and the replace would have
 * hashed are still unhashed.
 */
static void rcu_txn_test_hlist_cancel(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3, 4, 5 };
	static const int want[] = { 1, 2, 4, 5 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *n7, *n8, *n44;
	struct rcu_txn txn;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	rcu_txn_init(&txn, GFP_KERNEL);
	hlist_prepare_mixed(test, &txn, &head);
	rcu_txn_cancel(&txn);
	hlist_expect(test, &head, keys, ARRAY_SIZE(keys));

	n7 = hn_new(test, 7);
	n8 = hn_new(test, 8);
	n44 = hn_new(test, 44);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_hlist_add_head_prepare(&txn, n7, &head);
	rcu_txn_hlist_add_before_prepare(&txn, n8, hlist_find(&head, 5));
	rcu_txn_hlist_replace_prepare(&txn, hlist_find(&head, 4), n44);
	rcu_txn_cancel(&txn);
	KUNIT_EXPECT_TRUE(test, rcu_txn_hlist_unhashed(n7, true));
	KUNIT_EXPECT_TRUE(test, rcu_txn_hlist_unhashed(n8, true));
	KUNIT_EXPECT_TRUE(test, rcu_txn_hlist_unhashed(n44, true));
	hlist_expect(test, &head, keys, ARRAY_SIZE(keys));

	rcu_txn_hlist_del_rcu(hlist_find(&head, 3));
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * Run out of memory after two edits.  The test sets the sticky state by
 * hand, as a failed record() leaves it.  commit() reports it and publishes
 * nothing.  The edits recorded before the failure leave no pprev behind
 * either.
 */
static void rcu_txn_test_hlist_oom(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3, 4, 5 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *e1, *e2, *e4;
	struct rcu_txn txn;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	e1 = hlist_find(&head, 1);
	e2 = hlist_find(&head, 2);
	e4 = hlist_find(&head, 4);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_hlist_del_prepare(&txn, e2);
	rcu_txn_hlist_add_behind_prepare(&txn, hn_new(test, 9), e1);
	txn.state = RCU_TXN_OOM;
	rcu_txn_hlist_replace_prepare(&txn, e4, hn_new(test, 44));
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), -ENOMEM);
	hlist_expect(test, &head, keys, ARRAY_SIZE(keys));
}

/*
 * An add_behind anchored on a node that the same transaction deleted.  The
 * anchor is unhashed as the transaction leaves it.  The add records the
 * successor's pprev from a slot that the delete already moved it away from.
 */
static void rcu_txn_test_hlist_debug_ghost_anchor(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn_hlist_node *e2;
	struct rcu_txn txn;

	need_debug(test);
	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	e2 = hlist_find(&head, 2);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_hlist_del_prepare(&txn, e2);
	kunit_warning_suppress(test) {
		rcu_txn_hlist_add_behind_prepare(&txn, hn_new(test, 9), e2);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 2);
	}
	rcu_txn_cancel(&txn);
	hlist_expect(test, &head, keys, ARRAY_SIZE(keys));
}

static struct kunit_case rcu_txn_test_cases[] = {
	KUNIT_CASE(rcu_txn_test_single_record),
	KUNIT_CASE(rcu_txn_test_multi_record),
	KUNIT_CASE(rcu_txn_test_commit_steps),
	KUNIT_CASE(rcu_txn_test_typed_slot),
	KUNIT_CASE(rcu_txn_test_grow),
	KUNIT_CASE(rcu_txn_test_reserve),
	KUNIT_CASE(rcu_txn_test_zero_tag),
	KUNIT_CASE(rcu_txn_test_cancel),
	KUNIT_CASE(rcu_txn_test_oom),
	KUNIT_CASE(rcu_txn_test_alloc_fail),
	KUNIT_CASE(rcu_txn_test_ryw_chain),
	KUNIT_CASE(rcu_txn_test_disjoint),
	KUNIT_CASE(rcu_txn_test_consumed_handle),
	KUNIT_CASE(rcu_txn_test_debug_control),
	KUNIT_CASE(rcu_txn_test_debug_owner),
	KUNIT_CASE(rcu_txn_test_debug_adopt),
	KUNIT_CASE(rcu_txn_test_debug_record_over_proxy),
	KUNIT_CASE(rcu_txn_test_debug_install_over_proxy),
	KUNIT_CASE(rcu_txn_test_debug_install_over_settled),
	KUNIT_CASE(rcu_txn_test_debug_settle_clobbered),
	KUNIT_CASE(rcu_txn_test_debug_single_record_changed),
	KUNIT_CASE(rcu_txn_test_debug_duplicate_slot),
	KUNIT_CASE(rcu_txn_test_debug_tagged_value),
	KUNIT_CASE(rcu_txn_test_debug_install_zero_tag),
	KUNIT_CASE(rcu_txn_test_debug_disjoint),
	KUNIT_CASE(rcu_txn_test_debug_stale_chain),
	KUNIT_CASE(rcu_txn_test_debug_load_proxy),
	KUNIT_CASE(rcu_txn_test_hlist_add),
	KUNIT_CASE(rcu_txn_test_hlist_del),
	KUNIT_CASE(rcu_txn_test_hlist_replace),
	KUNIT_CASE(rcu_txn_test_hlist_compose_adjacent_del),
	KUNIT_CASE(rcu_txn_test_hlist_compose_del_and_add),
	KUNIT_CASE(rcu_txn_test_hlist_compose_triple_del),
	KUNIT_CASE(rcu_txn_test_hlist_compose_add_then_del_host),
	KUNIT_CASE(rcu_txn_test_hlist_compose_del_then_add_before),
	KUNIT_CASE(rcu_txn_test_hlist_move),
	KUNIT_CASE(rcu_txn_test_hlist_prepare_alone),
	KUNIT_CASE(rcu_txn_test_hlist_compose_mixed),
	KUNIT_CASE(rcu_txn_test_hlist_cancel),
	KUNIT_CASE(rcu_txn_test_hlist_oom),
	KUNIT_CASE(rcu_txn_test_hlist_debug_ghost_anchor),
	{}
};

static struct kunit_suite rcu_txn_test_suite = {
	.name = "rcu_txn",
	.test_cases = rcu_txn_test_cases,
};

kunit_test_suite(rcu_txn_test_suite);

MODULE_DESCRIPTION("KUnit tests of the single-writer RCU pseudo-transactions");
MODULE_LICENSE("GPL");
