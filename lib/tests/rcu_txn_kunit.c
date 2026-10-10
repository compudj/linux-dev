// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * KUnit tests of the single-writer RCU pseudo-transactions
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <kunit/test.h>
#include <linux/bug.h>
#include <linux/rcu_txn.h>
#include <linux/rcupdate.h>
#include <linux/string.h>

#include "../rcu_txn_internal.h"	/* the steps of a commit */

/* Bit 0 of the proxy pointer: no value below carries it. */
#define TAG	1UL

/* Opaque slot values with the tag bit clear. */
#define T(n)	((void *)(((unsigned long)(n) + 1) << 8))

/*
 * A WARN_ON_ONCE() of the engine reports once per boot, so a later run of the
 * suite sees none.  Where the architecture keeps the once state in its bug
 * table, KUnit counts every suppressed occurrence instead.
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
 * as a writer, through the forms that take a lockdep condition, passing true,
 * and outside of any RCU read-side critical section.
 *
 * A test that reads as a reader instead, as read_slot() below does, checks
 * what it read only after rcu_read_unlock(): a failed KUNIT_EXPECT_*()
 * allocates with GFP_KERNEL, which may sleep.
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

/*
 * One record: the commit stores the new target directly.  No proxy is ever
 * installed, so the descriptor is freed at once.
 */
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

/*
 * Two records or more: the commit installs every proxy, flips the group and
 * settles every slot to new.
 */
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
 * The three steps of a commit, by hand: an installed proxy resolves to old,
 * the flip makes it resolve to new, and the settle leaves the new target in the
 * slot.
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
	__rcu_txn_group_commit(&group);
	KUNIT_EXPECT_TRUE(test,
			  rcu_txn_is_proxy(rcu_access_pointer(slot), TAG));
	KUNIT_EXPECT_PTR_EQ(test, read_slot(&slot), T(1));
	__rcu_txn_record_settle(&rec);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(slot), T(1));
}

struct item {
	struct item __rcu *next;
	int key;
};

/*
 * A typed __rcu pointer as a slot: rcu_txn_slot() hands its address to the
 * engine and rcu_txn_dereference_check() reads it back with its type, through a
 * proxy or not.
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

/* Record @nr edges on distinct slots.  Returns false if a record() failed. */
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
 * The sticky OOM state, set by hand: no allocation of this size fails on
 * demand.  Later records do nothing, and commit() and cancel() both release
 * the handle without publishing.
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
	KUNIT_CASE(rcu_txn_test_ryw_chain),
	KUNIT_CASE(rcu_txn_test_disjoint),
	KUNIT_CASE(rcu_txn_test_consumed_handle),
	{}
};

static struct kunit_suite rcu_txn_test_suite = {
	.name = "rcu_txn",
	.test_cases = rcu_txn_test_cases,
};

kunit_test_suite(rcu_txn_test_suite);

MODULE_DESCRIPTION("KUnit tests of the single-writer RCU pseudo-transactions");
MODULE_LICENSE("GPL");
