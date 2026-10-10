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
#include <linux/sched/task.h>
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

static void __rcu *bloom_slots[4096];

/* An empty filter answers absent for every slot. */
static void rcu_txn_test_bloom_empty(struct kunit *test)
{
	u64 bloom[RCU_TXN_BLOOM_WORDS] = {};
	unsigned int i, present = 0;

	for (i = 0; i < ARRAY_SIZE(bloom_slots); i++)
		present += rcu_txn_bloom_test(bloom, &bloom_slots[i]);
	KUNIT_EXPECT_EQ(test, present, 0);
}

/*
 * No false negatives: every slot ever set still reads present, whatever is set
 * after it.  This is the property the engine's correctness rests on.
 */
static void rcu_txn_test_bloom_no_false_negative(struct kunit *test)
{
	u64 bloom[RCU_TXN_BLOOM_WORDS] = {};
	unsigned int i, j, missing = 0;

	for (i = 0; i < ARRAY_SIZE(bloom_slots); i++) {
		rcu_txn_bloom_set(bloom, &bloom_slots[i]);
		/* Testing all i + 1 each round is O(n^2): sample. */
		for (j = 0; j <= i; j += i / 16 + 1)
			missing += !rcu_txn_bloom_test(bloom, &bloom_slots[j]);
	}
	for (i = 0; i < ARRAY_SIZE(bloom_slots); i++)
		missing += !rcu_txn_bloom_test(bloom, &bloom_slots[i]);
	KUNIT_EXPECT_EQ(test, missing, 0);
}

/*
 * The filter still discriminates past its design fill: with an eighth of the
 * slots set, not every absent slot reads present.  The bound only has to catch
 * a degenerate hash.
 */
static void rcu_txn_test_bloom_discriminates(struct kunit *test)
{
	const unsigned int nr = ARRAY_SIZE(bloom_slots);
	u64 bloom[RCU_TXN_BLOOM_WORDS] = {};
	unsigned int i, false_positives = 0;

	for (i = 0; i < nr / 8; i++)
		rcu_txn_bloom_set(bloom, &bloom_slots[i]);
	for (i = nr / 8; i < nr; i++)
		false_positives += rcu_txn_bloom_test(bloom, &bloom_slots[i]);
	KUNIT_EXPECT_LT(test, false_positives, nr - nr / 8);
}

/*
 * The filter does not depend on insertion order.  Arming rebuilds it from the
 * records so far, so a rebuilt filter must match one grown incrementally.
 */
static void rcu_txn_test_bloom_rebuild(struct kunit *test)
{
	u64 bloom[RCU_TXN_BLOOM_WORDS] = {}, rebuilt[RCU_TXN_BLOOM_WORDS] = {};
	unsigned int i;

	for (i = 0; i < 64; i++)
		rcu_txn_bloom_set(bloom, &bloom_slots[i]);
	for (i = 64; i-- > 0; )
		rcu_txn_bloom_set(rebuilt, &bloom_slots[i]);
	KUNIT_EXPECT_MEMEQ(test, bloom, rebuilt, sizeof(bloom));
}

/*
 * The engine arms the filter at the first lookup that faces RCU_TXN_BLOOM_MIN
 * records, and keeps it current from then on: slots recorded before and after
 * the arming are both found.
 */
static void rcu_txn_test_bloom_arming(struct kunit *test)
{
	enum { NR = 2 * RCU_TXN_BLOOM_MIN };
	void __rcu *slots[NR];
	struct rcu_txn txn;
	unsigned int i;

	for (i = 0; i < NR; i++)
		RCU_INIT_POINTER(slots[i], T(i));
	rcu_txn_init(&txn, GFP_KERNEL);
	for (i = 0; i < NR; i++) {
		void *v = rcu_txn_load(&txn, &slots[i], TAG);

		/* The lookup that faces RCU_TXN_BLOOM_MIN records arms it. */
		KUNIT_EXPECT_EQ(test, txn.bloom_live, i >= RCU_TXN_BLOOM_MIN);
		rcu_txn_record_chain(&txn, &slots[i], v, T(20 + i), TAG);
	}
	for (i = 0; i < NR; i++) {
		void *v = rcu_txn_load(&txn, &slots[i], TAG);

		KUNIT_EXPECT_PTR_EQ(test, v, T(20 + i));
		rcu_txn_record_chain(&txn, &slots[i], v, T(100 + i), TAG);
	}
	KUNIT_EXPECT_EQ(test, txn.nr, NR);		/* fused */
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	KUNIT_EXPECT_TRUE(test, all_settled(slots, NR));

	/* A handle declared disjoint never looks up, and never arms. */
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_declare_disjoint(&txn);
	for (i = 0; i < NR; i++) {
		void *v = rcu_txn_load(&txn, &slots[i], TAG);

		rcu_txn_record_chain(&txn, &slots[i], v, T(i), TAG);
	}
	KUNIT_EXPECT_FALSE(test, txn.bloom_live);
	rcu_txn_cancel(&txn);
}

/*
 * An armed filter moves with the records when the transaction outgrows its
 * descriptor: every slot recorded before and after the move is still found.
 */
static void rcu_txn_test_bloom_grow(struct kunit *test)
{
	enum { NR = 8 * RCU_TXN_BLOOM_MIN };
	void __rcu *slots[NR];
	unsigned int i, cap = 0;
	struct rcu_txn txn;

	for (i = 0; i < NR; i++)
		RCU_INIT_POINTER(slots[i], T(i));
	rcu_txn_init(&txn, GFP_KERNEL);
	for (i = 0; i < NR; i++) {
		void *v = rcu_txn_load(&txn, &slots[i], TAG);

		if (txn.bloom_live && !cap)
			cap = txn.cap;		/* armed in this descriptor */
		rcu_txn_record_chain(&txn, &slots[i], v, T(100 + i), TAG);
	}
	KUNIT_ASSERT_NE(test, cap, 0);
	KUNIT_EXPECT_GT(test, txn.cap, cap);	/* grown since */
	for (i = 0; i < NR; i++)
		KUNIT_EXPECT_PTR_EQ(test, rcu_txn_load(&txn, &slots[i], TAG),
				    T(100 + i));
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	KUNIT_EXPECT_TRUE(test, all_settled(slots, NR));
}

/*
 * CONFIG_DEBUG_RCU_TXN
 *
 * Every case below is an embedder bug that the debug checks report.  The
 * writer/writer races are staged deterministically: the other writer's proxy is
 * installed in the slot by hand, as a real race would leave it.
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
 * and its flip leaves it.  commit() runs install, flip and settle back to back,
 * so one task cannot stop a transaction there.
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
 * A handle got ready by one task and adopted by the one that drives it.  Not
 * with records in it: that is a transaction changing hands mid-flight.
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

/*
 * A second writer records a slot where the first has already installed a
 * proxy.  This is the interleaving where the violation is visible: the first
 * writer is between its install and its flip, so its proxy is sitting in the
 * slot.
 */
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
 * A second writer installs onto a slot where the first has already installed a
 * proxy.  It recorded the slot while it was still plain, so record() saw
 * nothing wrong; the overlap only becomes visible at its own install, which
 * finds a proxy there and not the recorded old.
 */
static void rcu_txn_test_debug_install_over_proxy(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	struct rcu_txn_record rec, other;
	struct rcu_txn_group group;

	need_debug(test);
	/* Recorded while the slot is still plain. */
	__rcu_txn_record_set(&rec, &slot, T(0), T(2), TAG);
	install_by_hand(&group, &other, &slot, T(0), T(1));
	kunit_warning_suppress(test) {
		__rcu_txn_excl_slot_installable(&rec);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 2);
	}
}

/*
 * A peer ran a complete transaction on the slot between our record() and our
 * commit().  It leaves a plain value behind, so the slot looks free; the
 * witness is that it no longer holds the recorded old.  The peer's committed
 * write is staged by a plain store, which is what it leaves behind.
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
 * The single-record commit stores blind, with no proxy and no install, so its
 * only witness is the slot's value: it must still hold the recorded old.
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

/*
 * One slot recorded twice, not adjacently: the commit would install both
 * records and settle them in record order, dropping the earlier edit.
 */
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

/*
 * No value stored in a transacted slot may carry the slot's tag: a reader
 * would take it for a proxy.  Caught at the record that introduces it, on the
 * new value and on the old.
 */
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
 * On a handle declared disjoint, a load or a record of a slot already recorded:
 * the load would miss the pending value, and the record would put a second
 * proxy on the slot.
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

/*
 * record_chain() given an old value that is not the pending one: its caller
 * computed the new value from a stale read.
 */
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
	KUNIT_CASE(rcu_txn_test_bloom_empty),
	KUNIT_CASE(rcu_txn_test_bloom_no_false_negative),
	KUNIT_CASE(rcu_txn_test_bloom_discriminates),
	KUNIT_CASE(rcu_txn_test_bloom_rebuild),
	KUNIT_CASE(rcu_txn_test_bloom_arming),
	KUNIT_CASE(rcu_txn_test_bloom_grow),
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
	{}
};

static struct kunit_suite rcu_txn_test_suite = {
	.name = "rcu_txn",
	.test_cases = rcu_txn_test_cases,
};

kunit_test_suite(rcu_txn_test_suite);

MODULE_DESCRIPTION("KUnit tests of the single-writer RCU pseudo-transactions");
MODULE_LICENSE("GPL");
