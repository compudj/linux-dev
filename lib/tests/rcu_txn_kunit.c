// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * KUnit tests of the single-writer RCU pseudo-transactions
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <kunit/test.h>
#include <linux/bug.h>
#include <linux/cpumask.h>
#include <linux/debug_locks.h>
#include <linux/delay.h>
#include <linux/irqflags.h>
#include <linux/kthread.h>
#include <linux/list_bl.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/panic.h>
#include <linux/rcu_txn.h>
#include <linux/rcu_txn_hlist.h>
#include <linux/rcu_txn_list.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
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
	v = rcu_txn_resolve(slot, rcu_dereference(*slot), TAG);
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
 * The forward walk gives @want, and each node's pprev points to the slot that
 * points to it: &head->first for the first node, &prev->next thereafter.  A
 * torn pprev is invisible to the forward walk but breaks the next edit, hence
 * the separate check.
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

	KUNIT_EXPECT_TRUE(test, rcu_txn_hlist_empty(&head, true));
	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	hlist_expect(test, &head, keys, ARRAY_SIZE(keys));

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

	rcu_txn_hlist_del_rcu(hlist_find(&head, 1));	/* first */
	rcu_txn_hlist_del_rcu(hlist_find(&head, 5));	/* last */
	hlist_expect(test, &head, ends, ARRAY_SIZE(ends));
}

static void rcu_txn_test_hlist_replace(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3 };
	static const int want[] = { 1, 22, 3 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	rcu_txn_hlist_replace_rcu(hlist_find(&head, 2), hn_new(test, 22));
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * Composition on one bucket, with neighbourhoods that touch: the case the
 * _prepare forms read their own writes for.
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
	/* &1->next, chained, and the pprev of 3 and of 4. */
	KUNIT_EXPECT_EQ(test, txn.nr, 3);
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
 * Insert 9 behind 1, then delete 1, in that order: the fresh node inherits
 * &1->next as the slot that points to it, and the next operation removes the
 * node that slot lives in.  add_behind must have recorded &9->next as 2's
 * pprev, and del(1) must then re-point head->first to the pending 9 rather than
 * to the committed 2, and record &head->first as 9's pprev.
 *
 * A wrong pprev for 2 leaves the forward order intact here: only the pprev
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
 * The committed &1->next would publish head->first == 1.
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
	/* The two forward edges, and the pprev of 3. */
	KUNIT_EXPECT_EQ(test, txn.nr, 3);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	hlist_expect(test, &a, from, ARRAY_SIZE(from));
	hlist_expect(test, &b, to, ARRAY_SIZE(to));
}

/*
 * One _prepare alone: an update with a successor records its forward edge and
 * the successor's pprev, and commits through the descriptor.
 */
static void rcu_txn_test_hlist_prepare_alone(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3 };
	static const int want[] = { 1, 3 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn txn;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_hlist_del_prepare(&txn, hlist_find(&head, 2));
	KUNIT_EXPECT_EQ(test, txn.nr, 2);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * Every kind of edit in one transaction over 1..5: del(2), add_behind(9, 1),
 * replace(4, 44), add_before(8, 5).  The last re-points the slot that the
 * replace made 5's, inside the fresh node 44, and so loads 5's pending pprev
 * to find it.
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
 * The same transaction, cancelled: nothing was written outside it, so the chain
 * and every pprev are as they were.  A later single update then finds the
 * pprev it relies on.
 */
static void rcu_txn_test_hlist_cancel(struct kunit *test)
{
	static const int keys[] = { 1, 2, 3, 4, 5 };
	static const int want[] = { 1, 2, 4, 5 };
	struct rcu_txn_hlist_head head = RCU_TXN_HLIST_HEAD_INIT;
	struct rcu_txn txn;

	hlist_build(test, &head, keys, ARRAY_SIZE(keys));
	rcu_txn_init(&txn, GFP_KERNEL);
	hlist_prepare_mixed(test, &txn, &head);
	rcu_txn_cancel(&txn);
	hlist_expect(test, &head, keys, ARRAY_SIZE(keys));

	rcu_txn_hlist_del_rcu(hlist_find(&head, 3));
	hlist_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * Out of memory after two edits, the sticky state set by hand as a failed
 * record() leaves it: commit() reports it and publishes nothing, and the edits
 * recorded before the failure leave no pprev behind either.
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
 * An add_behind anchored on a node that the same transaction deleted: it
 * records the successor's pprev from a slot that the delete already moved it
 * from.
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
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	rcu_txn_cancel(&txn);
	hlist_expect(test, &head, keys, ARRAY_SIZE(keys));
}

/*
 * <linux/rcu_txn_list.h>
 */

struct ln {
	struct rcu_txn_list_node node;
	int key;
};

/* Freed with the test: a removed node stays valid for any reader. */
static struct rcu_txn_list_node *ln_new(struct kunit *test, int key)
{
	struct ln *n = kunit_kzalloc(test, sizeof(*n), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, n);
	n->key = key;
	return &n->node;
}

/* Build the list @keys by appending. */
static void list_build(struct kunit *test, struct rcu_txn_list_head *head,
		       const int *keys, int nr)
{
	int i;

	for (i = 0; i < nr; i++)
		rcu_txn_list_add_tail_rcu(ln_new(test, keys[i]), head);
}

/* The test is the list's only writer, and owns every node until it ends. */
static struct rcu_txn_list_node *list_find(struct rcu_txn_list_head *head,
					   int key)
{
	struct ln *n;

	rcu_txn_list_for_each_entry_rcu(n, head, node, true)
		if (n->key == key)
			return &n->node;
	return NULL;
}

/*
 * The forward walk gives @want, the backward walk gives its mirror, and next
 * and prev are mutual inverses at every edge of the ring, sentinel included.
 */
static void list_expect(struct kunit *test, struct rcu_txn_list_head *head,
			const int *want, int nr)
{
	struct rcu_txn_list_node *p, *q;
	struct ln *n;
	int i = 0;

	rcu_txn_list_for_each_entry_rcu(n, head, node, true) {
		if (i < nr)
			KUNIT_EXPECT_EQ(test, n->key, want[i]);
		i++;
	}
	KUNIT_EXPECT_EQ(test, i, nr);
	rcu_txn_list_for_each_entry_reverse_rcu(n, head, node, true) {
		i--;
		if (i >= 0)
			KUNIT_EXPECT_EQ(test, n->key, want[i]);
	}
	KUNIT_EXPECT_EQ(test, i, 0);
	p = &head->node;
	do {
		q = rcu_txn_list_next_check(p, true);
		KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_prev_check(q, true), p);
		p = q;
	} while (p != &head->node);
}

static void rcu_txn_test_list_add(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	RCU_TXN_LIST_HEAD(tail);
	static const int at_head[] = { 3, 2, 1 };
	static const int keys[] = { 1, 2, 3 };
	static const int middle[] = { 1, 8, 2, 9, 3 };
	int i;

	KUNIT_EXPECT_TRUE(test, rcu_txn_list_empty(&head, true));
	list_expect(test, &head, NULL, 0);

	for (i = 1; i <= 3; i++)
		rcu_txn_list_add_rcu(ln_new(test, i), &head);
	list_expect(test, &head, at_head, ARRAY_SIZE(at_head));

	list_build(test, &tail, keys, ARRAY_SIZE(keys));
	list_expect(test, &tail, keys, ARRAY_SIZE(keys));

	rcu_txn_list_add_after_rcu(ln_new(test, 9), list_find(&tail, 2));
	rcu_txn_list_add_before_rcu(ln_new(test, 8), list_find(&tail, 2));
	list_expect(test, &tail, middle, ARRAY_SIZE(middle));
}

static void rcu_txn_test_list_del(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	static const int keys[] = { 1, 2, 3, 4, 5 };
	static const int middle[] = { 1, 2, 4, 5 };
	static const int ends[] = { 2, 4 };
	struct rcu_txn_list_node *removed;

	list_build(test, &head, keys, ARRAY_SIZE(keys));
	removed = list_find(&head, 3);
	rcu_txn_list_del_rcu(removed);
	list_expect(test, &head, middle, ARRAY_SIZE(middle));
	/* A reader standing on the removed node leaves in either direction. */
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_next_check(removed, true),
			    list_find(&head, 4));
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_prev_check(removed, true),
			    list_find(&head, 2));

	rcu_txn_list_del_rcu(list_find(&head, 1));	/* first */
	rcu_txn_list_del_rcu(list_find(&head, 5));	/* last */
	list_expect(test, &head, ends, ARRAY_SIZE(ends));

	rcu_txn_list_del_rcu(list_find(&head, 2));
	rcu_txn_list_del_rcu(list_find(&head, 4));
	list_expect(test, &head, NULL, 0);
}

static void rcu_txn_test_list_replace(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	static const int keys[] = { 1, 2, 3 };
	static const int want[] = { 1, 22, 3 };

	list_build(test, &head, keys, ARRAY_SIZE(keys));
	rcu_txn_list_replace_rcu(list_find(&head, 2), ln_new(test, 22));
	list_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * The two edges of a delete, by hand, as a reader sees them across install,
 * flip and settle: old in both directions until the single flip, new in both
 * directions after.
 *
 * Ring: head <-> A <-> B <-> head.  Delete A:
 *   forward edge:  head->next: A -> B
 *   backward edge: B->prev:    A -> head
 */
static void rcu_txn_test_list_proxy_phases(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	static const int keys[] = { 1, 2 };
	static const int want[] = { 2 };
	struct rcu_txn_list_node *a, *b;
	struct rcu_txn_record rec[2];
	struct rcu_txn_group group;

	list_build(test, &head, keys, ARRAY_SIZE(keys));
	a = list_find(&head, 1);
	b = list_find(&head, 2);

	__rcu_txn_group_init(&group);
	__rcu_txn_record_set(&rec[0], rcu_txn_slot(&head.node.next), a, b,
			     RCU_TXN_LIST_TAG);
	__rcu_txn_record_set(&rec[1], rcu_txn_slot(&b->prev), a, &head.node,
			     RCU_TXN_LIST_TAG);
	__rcu_txn_record_install(&group, &rec[0]);
	__rcu_txn_record_install(&group, &rec[1]);

	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_next_check(&head.node, true), a);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_prev_check(b, true), a);

	__rcu_txn_group_commit(&group);	/* one flip switches both edges */

	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_next_check(&head.node, true), b);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_prev_check(b, true), &head.node);

	/* No reader holds a proxy here: the records need no grace period. */
	__rcu_txn_record_settle(&rec[0]);
	__rcu_txn_record_settle(&rec[1]);
	list_expect(test, &head, want, ARRAY_SIZE(want));
}

/* Delete the adjacent 2 and 3 from 1..5 in one transaction. */
static void rcu_txn_test_list_compose_adjacent_del(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	static const int keys[] = { 1, 2, 3, 4, 5 };
	static const int want[] = { 1, 4, 5 };
	struct rcu_txn_list_node *e2, *e3;
	struct rcu_txn txn;

	list_build(test, &head, keys, ARRAY_SIZE(keys));
	e2 = list_find(&head, 2);
	e3 = list_find(&head, 3);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_list_del_prepare(&txn, e2);
	rcu_txn_list_del_prepare(&txn, e3);	/* the neighbour of e2 */
	/* &1->next is chained; &3->prev and &4->prev are one record each. */
	KUNIT_EXPECT_EQ(test, txn.nr, 3);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	list_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * Delete 2 and insert 9 into the same gap in one transaction: both edges of
 * the gap are written twice.
 */
static void rcu_txn_test_list_compose_del_and_add(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	static const int keys[] = { 1, 2, 3 };
	static const int want[] = { 1, 9, 3 };
	struct rcu_txn_list_node *e1, *e2;
	struct rcu_txn txn;

	list_build(test, &head, keys, ARRAY_SIZE(keys));
	e1 = list_find(&head, 1);
	e2 = list_find(&head, 2);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_list_del_prepare(&txn, e2);
	rcu_txn_list_add_after_prepare(&txn, ln_new(test, 9), e1);
	KUNIT_EXPECT_EQ(test, txn.nr, 2);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	list_expect(test, &head, want, ARRAY_SIZE(want));
}

/* Three consecutive deletes in one transaction: &1->next is chained twice. */
static void rcu_txn_test_list_compose_triple_del(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	static const int keys[] = { 1, 2, 3, 4, 5 };
	static const int want[] = { 1, 5 };
	struct rcu_txn_list_node *e[3];
	struct rcu_txn txn;
	int i;

	list_build(test, &head, keys, ARRAY_SIZE(keys));
	for (i = 0; i < ARRAY_SIZE(e); i++)
		e[i] = list_find(&head, i + 2);		/* 2, 3, 4 */
	rcu_txn_init(&txn, GFP_KERNEL);
	for (i = 0; i < ARRAY_SIZE(e); i++)
		rcu_txn_list_del_prepare(&txn, e[i]);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	list_expect(test, &head, want, ARRAY_SIZE(want));
}

/* The moves of a node, in a transaction of their own. */
static void rcu_txn_test_list_move(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	static const int keys[] = { 1, 2, 3, 4, 5 };
	static const int front[] = { 4, 1, 2, 3, 5 };
	static const int tail[] = { 1, 2, 3, 5, 4 };
	static const int past_next[] = { 1, 3, 2, 5, 4 };
	struct rcu_txn_list_node *e1, *e2, *e3, *e4;
	struct rcu_txn txn;

	list_build(test, &head, keys, ARRAY_SIZE(keys));
	e1 = list_find(&head, 1);
	e2 = list_find(&head, 2);
	e3 = list_find(&head, 3);
	e4 = list_find(&head, 4);

	rcu_txn_list_move_rcu(e4, &head);
	list_expect(test, &head, front, ARRAY_SIZE(front));
	rcu_txn_list_move_tail_rcu(e4, &head);
	list_expect(test, &head, tail, ARRAY_SIZE(tail));
	/* Past its next, then back before it: the neighbourhoods overlap. */
	rcu_txn_list_move_after_rcu(e2, e3);
	list_expect(test, &head, past_next, ARRAY_SIZE(past_next));
	rcu_txn_list_move_before_rcu(e2, e3);
	list_expect(test, &head, tail, ARRAY_SIZE(tail));

	/* Where it already is: nothing is recorded. */
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_list_move_after_prepare(&txn, e2, e1);
	rcu_txn_list_move_before_prepare(&txn, e1, e2);
	KUNIT_EXPECT_EQ(test, txn.nr, 0);
	rcu_txn_cancel(&txn);
}

/*
 * The six edges of a move, as a reader sees them across install, flip and
 * settle: the old list until the single flip, the new one after, the moved
 * node's own links included.
 *
 * Ring: head <-> A <-> B <-> C <-> D <-> head.  Move D after A.
 */
static void rcu_txn_test_list_move_phases(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	static const int keys[] = { 1, 2, 3, 4 };
	static const int want[] = { 1, 4, 2, 3 };
	struct rcu_txn_list_node *a, *b, *c, *d, *h = &head.node;
	struct rcu_txn txn;
	unsigned int i;

	list_build(test, &head, keys, ARRAY_SIZE(keys));
	a = list_find(&head, 1);
	b = list_find(&head, 2);
	c = list_find(&head, 3);
	d = list_find(&head, 4);

	rcu_txn_init(&txn, GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, rcu_txn_reserve(&txn, 6));
	rcu_txn_list_move_after_prepare(&txn, d, a);
	KUNIT_ASSERT_EQ(test, txn.nr, 6);
	for (i = 0; i < txn.nr; i++)
		__rcu_txn_record_install(&txn.desc->group, &txn.records[i]);

	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_next_check(a, true), b);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_prev_check(b, true), a);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_next_check(c, true), d);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_prev_check(h, true), d);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_next_check(d, true), h);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_prev_check(d, true), c);

	__rcu_txn_group_commit(&txn.desc->group);	/* one flip, six edges */

	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_next_check(a, true), d);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_prev_check(b, true), d);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_next_check(c, true), h);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_prev_check(h, true), c);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_next_check(d, true), b);
	KUNIT_EXPECT_PTR_EQ(test, rcu_txn_list_prev_check(d, true), a);

	/* No reader holds a proxy here: the records need no grace period. */
	for (i = 0; i < txn.nr; i++)
		__rcu_txn_record_settle(&txn.records[i]);
	__rcu_txn_free_desc(txn.desc);
	__rcu_txn_done(&txn);
	list_expect(test, &head, want, ARRAY_SIZE(want));
}

/* Two moves to the front in one transaction: their edges chain. */
static void rcu_txn_test_list_compose_moves(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	static const int keys[] = { 1, 2, 3, 4 };
	static const int want[] = { 4, 3, 1, 2 };
	struct rcu_txn txn;

	list_build(test, &head, keys, ARRAY_SIZE(keys));
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_list_move_after_prepare(&txn, list_find(&head, 3), &head.node);
	rcu_txn_list_move_after_prepare(&txn, list_find(&head, 4), &head.node);
	KUNIT_EXPECT_EQ(test, rcu_txn_commit(&txn), 0);
	list_expect(test, &head, want, ARRAY_SIZE(want));
}

/*
 * Declaring disjoint a transaction that composes edits turns off what makes
 * them compose: the second delete reads a link the first has already recorded.
 */
static void rcu_txn_test_list_debug_disjoint_compose(struct kunit *test)
{
	RCU_TXN_LIST_HEAD(head);
	static const int keys[] = { 1, 2, 3, 4 };
	struct rcu_txn_list_node *e2, *e3;
	struct rcu_txn txn;

	need_debug(test);
	list_build(test, &head, keys, ARRAY_SIZE(keys));
	e2 = list_find(&head, 2);
	e3 = list_find(&head, 3);
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_declare_disjoint(&txn);
	rcu_txn_list_del_prepare(&txn, e2);
	kunit_warning_suppress(test) {
		rcu_txn_list_del_prepare(&txn, e3);	/* loads &3->prev */
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
	}
	rcu_txn_cancel(&txn);
	list_expect(test, &head, keys, ARRAY_SIZE(keys));
}

/*
 * One writer mutating a key-sorted list while several readers iterate it
 * forward and backward.
 *
 * Two invariants, because the first one alone cannot see the property this
 * test exists for.
 *
 * (1) Monotonicity, per direction.  The writer keeps the list sorted by key at
 * all times, so a forward walk must see strictly increasing keys and a reverse
 * walk strictly decreasing ones.  Out-of-range keys and runaway walks are
 * caught here too.
 *
 * What invariant (1) cannot catch is a torn two-edge publish.  Take an insert
 * of X between P and Q publishing only its forward edge: the forward ring is
 * P -> X -> Q, still strictly increasing, and the reverse ring is Q -> P, still
 * strictly decreasing.  Every torn state of a two-edge operation is sorted in
 * each direction separately.
 *
 * (2) Cross-direction agreement, which is where a torn publish shows.  After
 * each forward step p -> q the reader also resolves the prev of q and requires
 * its key to be at least p's.  A forward-only torn insert yields the
 * predecessor that is not back-linked yet, whose key is smaller than p's.  A
 * reverse-only torn insert is not caught: the prev of q points to X, whose key
 * lies between p's and q's, as after a completed insert.  list_add_rcu()
 * publishes the forward edge first, which is the tear this check is for.
 *
 * Invariant (2) holds only in a phase without deletes.  Under concurrent
 * deletes it is simply false: delete p between the reader's two loads and the
 * prev of q legitimately becomes the node before p, whose key is smaller.  With
 * inserts alone the legal outcomes are p itself or a node inserted between p
 * and q, both with a key at least p's.
 *
 * The phase change is a handshake.  The writer numbers its phases, the odd ones
 * allowing deletes, and after publishing an odd phase waits for every reader
 * to acknowledge that number.  A reader samples the phase at the top of an
 * iteration and acknowledges an odd one there, so that no cross-checking walk
 * is in flight when the writer proceeds; an acknowledgment of an earlier
 * phase does not count.  Store-release and load-acquire order the walks and
 * the deletes against the handshake.
 */

#define CONC_KEY_MAX		100		/* keys are 1..CONC_KEY_MAX */
#define CONC_READERS		4
#define CONC_STEP_LIMIT		(CONC_KEY_MAX + 8)	/* runaway-walk guard */
#define CONC_INSERT_ROUNDS	20
#define CONC_MIXED_OPS		20000

struct cn {
	struct rcu_txn_list_node node;
	struct rcu_head rcu;
	int key;
};

struct conc {
	struct rcu_txn_list_head head;
	unsigned long phase;	/* odd: deletes as well as inserts */
	struct cn *live[CONC_KEY_MAX + 1];	/* the writer's: node per key */
};

struct conc_reader {
	struct conc *conc;
	struct task_struct *task;
	long walks;
	long violations;	/* invariant (1) */
	long cross_checks;
	long cross_violations;	/* invariant (2) */
	unsigned long ack;	/* last odd phase seen: no cross-check in flight */
};

static int conc_reader_fn(void *arg)
{
	struct conc_reader *r = arg;
	struct rcu_txn_list_head *head = &r->conc->head;

	while (!kthread_should_stop()) {
		/* Pairs with the writer's store-release of the phase. */
		unsigned long phase = smp_load_acquire(&r->conc->phase);
		bool insert_only = !(phase & 1);
		struct rcu_txn_list_node *p;
		int prev_key, steps;

		if (!insert_only) {
			/* Pairs with the writer's load-acquire of the ack. */
			smp_store_release(&r->ack, phase);
		}

		/* Forward: strictly increasing keys, bounded. */
		rcu_read_lock();
		prev_key = 0;
		steps = 0;
		rcu_txn_list_for_each_rcu(p, head) {
			int key = container_of(p, struct cn, node)->key;

			if (key < 1 || key > CONC_KEY_MAX || key <= prev_key ||
			    ++steps > CONC_STEP_LIMIT) {
				r->violations++;
				break;
			}
			/*
			 * Invariant (2).  Skipped for the first step, whose
			 * predecessor is the head.
			 */
			if (insert_only && prev_key) {
				struct rcu_txn_list_node *back =
						rcu_txn_list_prev_rcu(p);

				r->cross_checks++;
				if (back != &head->node &&
				    container_of(back, struct cn,
						 node)->key < prev_key)
					r->cross_violations++;
			}
			prev_key = key;
		}
		rcu_read_unlock();

		/* Reverse: strictly decreasing keys, bounded. */
		rcu_read_lock();
		prev_key = CONC_KEY_MAX + 1;
		steps = 0;
		rcu_txn_list_for_each_reverse_rcu(p, head) {
			int key = container_of(p, struct cn, node)->key;

			if (key < 1 || key > CONC_KEY_MAX || key >= prev_key ||
			    ++steps > CONC_STEP_LIMIT) {
				r->violations++;
				break;
			}
			prev_key = key;
		}
		rcu_read_unlock();

		r->walks += 2;
		cond_resched();
	}
	return 0;
}

/* Insert @key, keeping the list sorted.  Returns false if it cannot. */
static bool conc_insert(struct conc *c, int key)
{
	struct rcu_txn_list_node *succ = &c->head.node;	/* default: last */
	struct cn *n = kmalloc(sizeof(*n), GFP_KERNEL);
	int i;

	if (!n)
		return false;
	n->key = key;
	for (i = key + 1; i <= CONC_KEY_MAX; i++) {
		if (c->live[i]) {
			succ = &c->live[i]->node;
			break;
		}
	}
	rcu_txn_list_add_before_rcu(&n->node, succ);
	c->live[key] = n;
	return true;
}

static void conc_del(struct conc *c, int key)
{
	struct cn *n = c->live[key];

	rcu_txn_list_del_rcu(&n->node);
	c->live[key] = NULL;
	kfree_rcu(n, rcu);
}

/* Replace the node of @key by a new one of the same key: order is kept. */
static bool conc_replace(struct conc *c, int key)
{
	struct cn *old = c->live[key];
	struct cn *n = kmalloc(sizeof(*n), GFP_KERNEL);

	if (!n)
		return false;
	n->key = key;
	rcu_txn_list_replace_rcu(&old->node, &n->node);
	c->live[key] = n;
	kfree_rcu(old, rcu);
	return true;
}

static void rcu_txn_test_list_concurrent(struct kunit *test)
{
	struct conc_reader *readers;
	long walks = 0, violations = 0, cross_checks = 0, cross_violations = 0;
	unsigned long steals = __rcu_txn_local_steals();
	u32 rng = 2463534242u;
	bool ok = true;
	struct conc *c;
	int i, key, round;

	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	readers = kunit_kcalloc(test, CONC_READERS, sizeof(*readers),
				GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c);
	KUNIT_ASSERT_NOT_NULL(test, readers);
	rcu_txn_list_init(&c->head);

	for (i = 0; i < CONC_READERS; i++) {
		readers[i].conc = c;
		readers[i].task = kthread_run(conc_reader_fn, &readers[i],
					      "rcu_txn_reader/%d", i);
		if (IS_ERR(readers[i].task)) {
			readers[i].task = NULL;
			ok = false;
		}
	}

	/*
	 * An even phase, repeated: fill the list one key at a time, with
	 * inserts only, so that the readers cross-check against concurrent
	 * inserts.  Then hand over to an odd phase and empty it with deletes.
	 */
	for (round = 0; ok && round < CONC_INSERT_ROUNDS; round++) {
		int order[CONC_KEY_MAX], other;

		for (i = 0; i < CONC_KEY_MAX; i++)
			order[i] = i + 1;
		for (i = CONC_KEY_MAX - 1; i > 0; i--) {	/* shuffle */
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			other = rng % (i + 1);
			swap(order[i], order[other]);
		}

		/* An even phase, after the deletes of the previous one. */
		smp_store_release(&c->phase, (c->phase | 1) + 1);

		for (i = 0; ok && i < CONC_KEY_MAX; i++) {
			ok = conc_insert(c, order[i]);
			/*
			 * Let the readers walk in between.  On one cpu they
			 * only run in this phase if the writer sleeps in it.
			 */
			if (i == CONC_KEY_MAX / 2)
				schedule_timeout_uninterruptible(1);
			else
				cond_resched();
		}

		/* An odd phase: wait until no even phase walk is running. */
		smp_store_release(&c->phase, c->phase + 1);
		for (i = 0; i < CONC_READERS; i++) {
			if (!readers[i].task)
				continue;
			/* Pairs with the reader's store-release of its ack. */
			while (smp_load_acquire(&readers[i].ack) != c->phase)
				schedule_timeout_uninterruptible(1);
		}

		for (key = 1; ok && key <= CONC_KEY_MAX; key++) {
			if (c->live[key])
				conc_del(c, key);
			cond_resched();
		}
	}

	/* Then inserts, deletes and replaces, at random. */
	for (i = 0; ok && i < CONC_MIXED_OPS; i++) {
		rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
		key = 1 + rng % CONC_KEY_MAX;

		if (!c->live[key])
			ok = conc_insert(c, key);
		else if (!(rng & 0x300))
			ok = conc_replace(c, key);
		else
			conc_del(c, key);
		cond_resched();
	}

	for (i = 0; i < CONC_READERS; i++) {
		if (readers[i].task)
			kthread_stop(readers[i].task);
		walks += readers[i].walks;
		violations += readers[i].violations;
		cross_checks += readers[i].cross_checks;
		cross_violations += readers[i].cross_violations;
	}
	for (key = 1; key <= CONC_KEY_MAX; key++)
		if (c->live[key])
			conc_del(c, key);

	/*
	 * The writer runs more operations within a grace period than a pool
	 * holds, so that the readers also race descriptors stolen back.
	 */
	kunit_info(test, "%ld walks, %ld cross-direction checks, %lu steals\n",
		   walks, cross_checks, __rcu_txn_local_steals() - steals);
	KUNIT_EXPECT_TRUE(test, ok);
	KUNIT_EXPECT_GT(test, walks, 0);
	/* Forward and reverse walks stayed sorted. */
	KUNIT_EXPECT_EQ(test, violations, 0);
	/* No two-edge publish was ever seen torn. */
	KUNIT_EXPECT_GT(test, cross_checks, 0);
	KUNIT_EXPECT_EQ(test, cross_violations, 0);
}

/* Did the kernel report something since @tainted and @locks were sampled? */
static void expect_no_report(struct kunit *test, bool tainted, int locks)
{
	KUNIT_EXPECT_EQ(test, test_taint(TAINT_WARN), tainted);
	KUNIT_EXPECT_EQ(test, debug_locks, locks);
}

/*
 * A commit from atomic context: under a bit spinlock, which is how a hash
 * table with hlist_bl buckets excludes its writers, and under a raw spinlock.
 * On PREEMPT_RT neither may take a sleeping lock, which lockdep and the
 * might_sleep() checks would report.
 *
 * The transaction reserves its records before it takes the lock, so recording
 * and committing allocate nothing, and the descriptor of a commit that
 * installed proxies is handed to kfree_rcu().
 *
 * A descriptor that installed none is released under the lock as well, by a
 * commit of one record or by a cancel.  On PREEMPT_RT that has to be
 * kfree_rcu() too, but these checks did not report a kfree() there when
 * tried: those two cases only exercise the path.
 */
static void rcu_txn_test_commit_atomic(struct kunit *test)
{
	static DEFINE_RAW_SPINLOCK(raw_lock);
	void __rcu *s1 = RCU_INITIALIZER(T(0));
	void __rcu *s2 = RCU_INITIALIZER(T(1));
	bool tainted = test_taint(TAINT_WARN);
	struct hlist_bl_head bucket;
	int locks = debug_locks;
	unsigned long flags;
	struct rcu_txn txn;
	int err;

	INIT_HLIST_BL_HEAD(&bucket);

	/* Two records, under a bit spinlock. */
	rcu_txn_init(&txn, GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, rcu_txn_reserve(&txn, 2));
	hlist_bl_lock(&bucket);
	rcu_txn_record(&txn, &s1, T(0), T(10), TAG);
	rcu_txn_record(&txn, &s2, T(1), T(11), TAG);
	err = rcu_txn_commit(&txn);
	hlist_bl_unlock(&bucket);
	KUNIT_EXPECT_EQ(test, err, 0);
	expect_no_report(test, tainted, locks);

	/* Two records, under a raw spinlock with interrupts off. */
	rcu_txn_init(&txn, GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, rcu_txn_reserve(&txn, 2));
	raw_spin_lock_irqsave(&raw_lock, flags);
	rcu_txn_record(&txn, &s1, T(10), T(20), TAG);
	rcu_txn_record(&txn, &s2, T(11), T(21), TAG);
	err = rcu_txn_commit(&txn);
	raw_spin_unlock_irqrestore(&raw_lock, flags);
	KUNIT_EXPECT_EQ(test, err, 0);
	expect_no_report(test, tainted, locks);

	/* One record. */
	rcu_txn_init(&txn, GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, rcu_txn_reserve(&txn, 2));
	hlist_bl_lock(&bucket);
	rcu_txn_record(&txn, &s1, T(20), T(40), TAG);
	err = rcu_txn_commit(&txn);
	hlist_bl_unlock(&bucket);
	KUNIT_EXPECT_EQ(test, err, 0);
	expect_no_report(test, tainted, locks);

	/* A cancel of such a handle. */
	rcu_txn_init(&txn, GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, rcu_txn_reserve(&txn, 2));
	hlist_bl_lock(&bucket);
	rcu_txn_record(&txn, &s1, T(40), T(50), TAG);
	rcu_txn_cancel(&txn);
	hlist_bl_unlock(&bucket);
	expect_no_report(test, tainted, locks);

	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s1), T(40));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s2), T(21));
}

/*
 * Resolve sections
 * ================
 *
 * A reader resolves a proxy within a resolve section, and
 * __rcu_txn_synchronize_resolve() waits for the sections in flight, so that a
 * descriptor can be reused once it returns.  See lib/rcu_txn.c.
 */

/*
 * A section entered within another, as by an NMI, takes the next level, and a
 * resolve within it takes the one after.
 */
static void rcu_txn_test_resolve_nest(struct kunit *test)
{
	void __rcu *slot = RCU_INITIALIZER(T(0));
	unsigned int outer, inner;
	struct rcu_txn_group group;
	struct rcu_txn_record rec;
	unsigned long flags;
	void *v;

	__rcu_txn_group_init(&group);
	__rcu_txn_record_set(&rec, &slot, T(0), T(1), TAG);
	__rcu_txn_record_install(&group, &rec);

	local_irq_save(flags);
	outer = __rcu_txn_resolve_enter();
	inner = __rcu_txn_resolve_enter();
	v = rcu_txn_resolve(&slot, rcu_dereference_raw(slot), TAG);
	__rcu_txn_resolve_exit(inner);
	__rcu_txn_resolve_exit(outer);
	local_irq_restore(flags);

	KUNIT_EXPECT_EQ(test, outer, 0);
	KUNIT_EXPECT_EQ(test, inner, 1);
	KUNIT_EXPECT_PTR_EQ(test, v, T(0));

	__rcu_txn_group_commit(&group);
	__rcu_txn_record_settle(&rec);
	/* No section in flight here: the wait returns. */
	__rcu_txn_synchronize_resolve();
	KUNIT_EXPECT_PTR_EQ(test, read_slot(&slot), T(1));
}

/*
 * A kthread holds resolve sections open on another CPU, @levels of them nested
 * as by an NMI, each for PARK_HOLD_NS, innermost first.  Just before it leaves
 * the outermost, it sets @done.
 */
#define PARK_HOLD_NS	(20 * NSEC_PER_MSEC)

struct park {
	int levels;
	int entered;	/* every section is open */
	int done;
};

static int park_fn(void *arg)
{
	struct park *p = arg;
	unsigned int level[2];
	unsigned long flags;
	int i;

	local_irq_save(flags);
	for (i = 0; i < p->levels; i++)
		level[i] = __rcu_txn_resolve_enter();
	/* Pairs with the test's load-acquire. */
	smp_store_release(&p->entered, 1);
	for (i = p->levels - 1; i >= 0; i--) {
		u64 start = local_clock();

		while (local_clock() - start < PARK_HOLD_NS)
			cpu_relax();
		if (!i)
			WRITE_ONCE(p->done, 1);
		__rcu_txn_resolve_exit(level[i]);
	}
	local_irq_restore(flags);

	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

/*
 * The wait outlasts a section in flight: once it returns, the store that the
 * section makes last is visible.  With two nested sections, the wait outlasts
 * the outer one, which is still in flight once the inner one is over: a single
 * count per CPU would have looked idle then.
 */
static void rcu_txn_test_resolve_wait(struct kunit *test)
{
	struct task_struct *task;
	struct park p;
	int levels, done;
	unsigned int cpu;

	for (levels = 1; levels <= 2; levels++) {
		p = (struct park){ .levels = levels };

		/* The test spins on its CPU, the kthread on another. */
		migrate_disable();
		cpu = cpumask_any_but(cpu_online_mask, smp_processor_id());
		if (cpu >= nr_cpu_ids) {
			migrate_enable();
			kunit_skip(test, "needs two CPUs online");
		}
		task = kthread_create(park_fn, &p, "rcu_txn_park");
		if (IS_ERR(task)) {
			migrate_enable();
			KUNIT_FAIL(test, "cannot create the kthread");
			return;
		}
		kthread_bind(task, cpu);
		wake_up_process(task);
		/* Pairs with the store-release of park_fn(). */
		while (!smp_load_acquire(&p.entered))
			cpu_relax();
		__rcu_txn_synchronize_resolve();
		done = READ_ONCE(p.done);
		migrate_enable();
		kthread_stop(task);

		KUNIT_EXPECT_EQ_MSG(test, done, 1, "%d nested section(s)",
				    levels);
	}
}

/*
 * Reuse of records once the wait returns, against concurrent readers.
 *
 * The writer commits two slots, round after round, with the same two records
 * and the same flip group, as a descriptor reused at once would.  Once it has
 * settled both slots, it waits for the resolve sections in flight, then
 * poisons the records, as the next transaction to use the descriptor may.
 * Each round gives the slots the records the other way round, so that a stale
 * proxy, if one were resolved, would read the other slot's values.
 *
 * Slot i holds REUSE_V(i, k) after round k.  A reader checks every target it
 * resolves: never the poison, always a value of its slot, and never of a round
 * older than the last one it read there.  The writer holds each round's proxies
 * installed for a while before and after the flip, so that the readers resolve
 * many of them.
 */
#define REUSE_READERS	4
#define REUSE_ROUNDS	20000
#define REUSE_V(i, k)	T(2 * (k) + (i))
/* Bit 1: no T() value has it, and bit 0, the tag, stays clear. */
#define REUSE_POISON	((void *)0xdead02UL)

struct reuse_desc {
	struct rcu_txn_group group;
	struct rcu_txn_record rec[2];
};

struct reuse {
	void __rcu *slot[2];
	struct reuse_desc desc;
	struct rcu_txn_group poison_group;	/* selector 1: reads ptr[1] */
};

struct reuse_reader {
	struct reuse *reuse;
	struct task_struct *task;
	long reads;
	long proxies;		/* loads that found a proxy */
	long violations;
};

static int reuse_reader_fn(void *arg)
{
	struct reuse_reader *r = arg;
	struct reuse *ru = r->reuse;
	unsigned long last[2] = { 0, 0 };
	int i;

	while (!kthread_should_stop()) {
		for (i = 0; i < 2; i++) {
			unsigned long n;
			void *v, *t;

			rcu_read_lock();
			v = rcu_dereference(ru->slot[i]);
			if (rcu_txn_is_proxy(v, TAG))
				r->proxies++;
			t = rcu_txn_resolve(&ru->slot[i], v, TAG);
			rcu_read_unlock();

			n = ((unsigned long)t >> 8) - 1;	/* T(n) */
			if (t == REUSE_POISON || (n & 1) != i ||
			    n / 2 < last[i])
				r->violations++;
			else
				last[i] = n / 2;
			r->reads++;
		}
		if (!(r->reads & 1023))
			cond_resched();
	}
	return 0;
}

static void reuse_round(struct reuse *ru, unsigned long k)
{
	struct reuse_desc *d = &ru->desc;
	struct rcu_txn_record *r0 = &d->rec[k & 1];
	struct rcu_txn_record *r1 = &d->rec[!(k & 1)];
	int i;

	__rcu_txn_group_init(&d->group);
	__rcu_txn_record_set(r0, &ru->slot[0], REUSE_V(0, k - 1),
			     REUSE_V(0, k), TAG);
	__rcu_txn_record_set(r1, &ru->slot[1], REUSE_V(1, k - 1),
			     REUSE_V(1, k), TAG);
	__rcu_txn_record_install(&d->group, r0);
	__rcu_txn_record_install(&d->group, r1);
	udelay(1);
	__rcu_txn_group_commit(&d->group);
	udelay(1);
	__rcu_txn_record_settle(r0);
	__rcu_txn_record_settle(r1);

	__rcu_txn_synchronize_resolve();
	for (i = 0; i < 2; i++) {
		d->rec[i].ptr[0] = REUSE_POISON;
		d->rec[i].ptr[1] = REUSE_POISON;
		d->rec[i].group = &ru->poison_group;
	}
}

/*
 * Run the readers against @rounds rounds of @round, then check what they read.
 * Returns how many of their loads found a proxy.
 */
static long reuse_test(struct kunit *test,
		       void (*round)(struct reuse *ru, unsigned long k),
		       unsigned long rounds)
{
	struct reuse_reader *readers;
	long reads = 0, proxies = 0, violations = 0;
	struct reuse *ru;
	unsigned long k;
	int i;

	/* kmalloc() aligns the records as a descriptor's: see rcu_txn.c. */
	ru = kunit_kzalloc(test, sizeof(*ru), GFP_KERNEL);
	readers = kunit_kcalloc(test, REUSE_READERS, sizeof(*readers),
				GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ru);
	KUNIT_ASSERT_NOT_NULL(test, readers);
	RCU_INIT_POINTER(ru->slot[0], REUSE_V(0, 0));
	RCU_INIT_POINTER(ru->slot[1], REUSE_V(1, 0));
	ru->poison_group.selector = 1;

	for (i = 0; i < REUSE_READERS; i++) {
		readers[i].reuse = ru;
		readers[i].task = kthread_run(reuse_reader_fn, &readers[i],
					      "rcu_txn_reuse/%d", i);
		if (IS_ERR(readers[i].task))
			readers[i].task = NULL;
	}

	for (k = 1; k <= rounds; k++) {
		round(ru, k);
		if (!(k & 255))
			cond_resched();
	}

	for (i = 0; i < REUSE_READERS; i++) {
		if (readers[i].task)
			kthread_stop(readers[i].task);
		reads += readers[i].reads;
		proxies += readers[i].proxies;
		violations += readers[i].violations;
	}

	kunit_info(test, "%ld reads, %ld through a proxy\n", reads, proxies);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(ru->slot[0]),
			    REUSE_V(0, rounds));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(ru->slot[1]),
			    REUSE_V(1, rounds));
	/* No reader read a record once it was reused. */
	KUNIT_EXPECT_EQ(test, violations, 0);
	return proxies;
}

static void rcu_txn_test_resolve_reuse(struct kunit *test)
{
	if (num_online_cpus() < 2)
		kunit_skip(test, "needs two CPUs online");
	/* The readers raced the commits. */
	KUNIT_EXPECT_GT(test, reuse_test(test, reuse_round, REUSE_ROUNDS), 0);
}

/*
 * Local transactions
 * ==================
 *
 * A local handle takes its descriptor from a pool of its CPU's, a grace period
 * after its last use, or at once by stealing it back: see lib/rcu_txn.c.
 */

/* Commit slot i from REUSE_V(i, k - 1) to REUSE_V(i, k), in a local handle. */
static void local_commit(void __rcu **slot, unsigned long k)
{
	struct rcu_txn txn;
	int i;

	rcu_txn_init_local(&txn);
	/* Each round records the slots the other way round. */
	for (i = 0; i < 2; i++) {
		int j = (k & 1) ? 1 - i : i;

		rcu_txn_record(&txn, &slot[j], REUSE_V(j, k - 1),
			       REUSE_V(j, k), TAG);
	}
	rcu_txn_commit(&txn);
}

/*
 * Leave this CPU's pool with each descriptor free, or in a batch whose callback
 * ran.  Earlier tests may have left settled descriptors in no batch: a commit
 * queues them, with its own.  The caller keeps to its CPU.
 */
static void local_drain(void)
{
	void __rcu *d[2];

	RCU_INIT_POINTER(d[0], REUSE_V(0, 0));
	RCU_INIT_POINTER(d[1], REUSE_V(1, 0));
	rcu_barrier();
	local_commit(d, 1);
	rcu_barrier();
}

/*
 * A local handle runs with preemption disabled, and its commit or cancel
 * enables it again.  RCU_TXN_LOCAL_CAP records fit.
 */
static void rcu_txn_test_local_commit(struct kunit *test)
{
	void __rcu *slots[RCU_TXN_LOCAL_CAP];
	void __rcu *s[2];
	int count = preempt_count(), within[3];
	struct rcu_txn txn;
	bool all_recorded;
	int err[3];

	RCU_INIT_POINTER(s[0], REUSE_V(0, 0));
	RCU_INIT_POINTER(s[1], REUSE_V(1, 0));

	/* Two records: the descriptor waits for the readers of its proxies. */
	rcu_txn_init_local(&txn);
	within[0] = preempt_count();
	rcu_txn_record(&txn, &s[0], REUSE_V(0, 0), REUSE_V(0, 1), TAG);
	rcu_txn_record(&txn, &s[1], REUSE_V(1, 0), REUSE_V(1, 1), TAG);
	err[0] = rcu_txn_commit(&txn);
	KUNIT_EXPECT_EQ(test, preempt_count(), count);

	/* One record, then a cancel: the descriptor goes back at once. */
	rcu_txn_init_local(&txn);
	within[1] = preempt_count();
	rcu_txn_record(&txn, &s[0], REUSE_V(0, 1), REUSE_V(0, 2), TAG);
	err[1] = rcu_txn_commit(&txn);
	KUNIT_EXPECT_EQ(test, preempt_count(), count);
	rcu_txn_init_local(&txn);
	rcu_txn_record(&txn, &s[1], REUSE_V(1, 1), T(0), TAG);
	rcu_txn_cancel(&txn);
	KUNIT_EXPECT_EQ(test, preempt_count(), count);

	/* As many records as a local handle holds. */
	rcu_txn_init_local(&txn);
	within[2] = preempt_count();
	all_recorded = record_all(&txn, slots, RCU_TXN_LOCAL_CAP);
	err[2] = rcu_txn_commit(&txn);
	KUNIT_EXPECT_EQ(test, preempt_count(), count);

	KUNIT_EXPECT_EQ(test, err[0], 0);
	KUNIT_EXPECT_EQ(test, err[1], 0);
	KUNIT_EXPECT_EQ(test, err[2], 0);
	KUNIT_EXPECT_TRUE(test, all_recorded);
	KUNIT_EXPECT_TRUE(test, all_settled(slots, RCU_TXN_LOCAL_CAP));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s[0]), REUSE_V(0, 2));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s[1]), REUSE_V(1, 1));
	if (IS_ENABLED(CONFIG_PREEMPT_COUNT)) {
		KUNIT_EXPECT_EQ(test, within[0], count + 1);
		KUNIT_EXPECT_EQ(test, within[1], count + 1);
		KUNIT_EXPECT_EQ(test, within[2], count + 1);
	}
}

/*
 * A local handle does not grow: a record past RCU_TXN_LOCAL_CAP, or a reserve,
 * warns and makes it OOM, and its commit publishes nothing.
 */
static void rcu_txn_test_local_over_cap(struct kunit *test)
{
	void __rcu *slots[RCU_TXN_LOCAL_CAP + 1];
	int count = preempt_count(), err[2];
	struct rcu_txn txn;
	bool recorded, reserved;
	unsigned int i;

	kunit_warning_suppress(test) {
		rcu_txn_init_local(&txn);
		recorded = record_all(&txn, slots, RCU_TXN_LOCAL_CAP + 1);
		err[0] = rcu_txn_commit(&txn);
		EXPECT_ONE_WARNING(test);
	}
	KUNIT_EXPECT_FALSE(test, recorded);
	KUNIT_EXPECT_EQ(test, err[0], -ENOMEM);
	for (i = 0; i <= RCU_TXN_LOCAL_CAP; i++)
		KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(slots[i]), T(i));
	KUNIT_EXPECT_EQ(test, preempt_count(), count);

	kunit_warning_suppress(test) {
		rcu_txn_init_local(&txn);
		reserved = rcu_txn_reserve(&txn, RCU_TXN_LOCAL_CAP + 1);
		err[1] = rcu_txn_commit(&txn);
		EXPECT_ONE_WARNING(test);
	}
	KUNIT_EXPECT_FALSE(test, reserved);
	KUNIT_EXPECT_EQ(test, err[1], -ENOMEM);
	KUNIT_EXPECT_EQ(test, preempt_count(), count);
}

/*
 * Commits spaced by grace periods find their descriptors back without a
 * steal: each burst takes no more than its CPU's pool, after rcu_barrier() has
 * run the callbacks of the previous one.
 */
static void rcu_txn_test_local_recycle(struct kunit *test)
{
	unsigned long k = 0, steals;
	void __rcu *s[2];
	int round, i;

	RCU_INIT_POINTER(s[0], REUSE_V(0, 0));
	RCU_INIT_POINTER(s[1], REUSE_V(1, 0));

	migrate_disable();
	local_drain();
	steals = __rcu_txn_local_steals();
	for (round = 0; round < 3; round++) {
		for (i = 0; i < RCU_TXN_LOCAL_POOL; i++)
			local_commit(s, ++k);
		rcu_barrier();
	}
	steals = __rcu_txn_local_steals() - steals;
	migrate_enable();

	KUNIT_EXPECT_EQ(test, steals, 0);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s[0]), REUSE_V(0, k));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s[1]), REUSE_V(1, k));
}

/*
 * A burst of commits on one CPU, faster than grace periods end, steals the
 * descriptors back, about once per pool's worth.
 */
static void rcu_txn_test_local_steal(struct kunit *test)
{
	unsigned long k, steals;
	void __rcu *s[2];

	RCU_INIT_POINTER(s[0], REUSE_V(0, 0));
	RCU_INIT_POINTER(s[1], REUSE_V(1, 0));

	migrate_disable();
	steals = __rcu_txn_local_steals();
	for (k = 1; k <= 8 * RCU_TXN_LOCAL_POOL; k++)
		local_commit(s, k);
	steals = __rcu_txn_local_steals() - steals;
	migrate_enable();

	kunit_info(test, "%lu steals\n", steals);
	KUNIT_EXPECT_GT(test, steals, 0);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s[0]),
			    REUSE_V(0, 8 * RCU_TXN_LOCAL_POOL));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s[1]),
			    REUSE_V(1, 8 * RCU_TXN_LOCAL_POOL));
}

/*
 * A reader on another CPU holds a resolve section open, with the proxy of a
 * local descriptor loaded, while the writer settles the descriptor's proxies,
 * then commits more local transactions than its pool holds: a steal takes the
 * descriptor back, and the next commits reuse it.  The section holds off the
 * grace period of the descriptor's batch as well.  The steal waits for the
 * section first, so that the reader still resolves the proxy to a target of
 * its own transaction.  A pool that reused the descriptor without waiting, by
 * a steal or before a grace period, would have it resolve to a later one.
 */
struct steal_park {
	void __rcu **slot;
	int holding;		/* the section is open, the proxy loaded */
	void *loaded;
	void *resolved;
};

static int steal_park_fn(void *arg)
{
	struct steal_park *sp = arg;
	unsigned long flags;
	unsigned int level;
	u64 start;
	void *v;

	local_irq_save(flags);
	level = __rcu_txn_resolve_enter();
	v = rcu_dereference_raw(*sp->slot);
	/* Pairs with the test's load-acquire. */
	smp_store_release(&sp->holding, 1);
	start = local_clock();
	while (local_clock() - start < PARK_HOLD_NS)
		cpu_relax();
	sp->loaded = v;
	if (rcu_txn_is_proxy(v, TAG))
		v = __rcu_txn_proxy_resolve(__rcu_txn_untag(v, TAG));
	sp->resolved = v;
	__rcu_txn_resolve_exit(level);
	local_irq_restore(flags);

	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void rcu_txn_test_local_steal_waits(struct kunit *test)
{
	struct steal_park sp = { };
	struct task_struct *task;
	unsigned long k, steals;
	struct rcu_txn txn;
	void __rcu *s[2];
	void *proxy;
	unsigned int cpu;
	int i;

	RCU_INIT_POINTER(s[0], REUSE_V(0, 0));
	RCU_INIT_POINTER(s[1], REUSE_V(1, 0));
	sp.slot = &s[0];

	/* The test commits on its CPU, the reader holds its section on another. */
	migrate_disable();
	cpu = cpumask_any_but(cpu_online_mask, smp_processor_id());
	if (cpu >= nr_cpu_ids) {
		migrate_enable();
		kunit_skip(test, "needs two CPUs online");
	}
	task = kthread_create(steal_park_fn, &sp, "rcu_txn_park");
	if (IS_ERR(task)) {
		migrate_enable();
		KUNIT_FAIL(test, "cannot create the kthread");
		return;
	}
	kthread_bind(task, cpu);
	/* Round 1's descriptor then goes into a batch of its own. */
	local_drain();

	/* Round 1, by hand: the proxies stay installed until the reader holds one. */
	rcu_txn_init_local(&txn);
	rcu_txn_record(&txn, &s[0], REUSE_V(0, 0), REUSE_V(0, 1), TAG);
	rcu_txn_record(&txn, &s[1], REUSE_V(1, 0), REUSE_V(1, 1), TAG);
	for (i = 0; i < 2; i++)
		__rcu_txn_record_install(&txn.desc->group, &txn.records[i]);
	proxy = rcu_access_pointer(s[0]);
	wake_up_process(task);
	/* Pairs with the store-release of steal_park_fn(). */
	while (!smp_load_acquire(&sp.holding))
		cpu_relax();
	__rcu_txn_group_commit(&txn.desc->group);
	for (i = 0; i < 2; i++)
		__rcu_txn_record_settle(&txn.records[i]);
	__rcu_txn_local_retire(txn.desc);
	__rcu_txn_done(&txn);

	/* Then twice what the pool holds: a steal, and every descriptor reused. */
	steals = __rcu_txn_local_steals();
	for (k = 2; k < 2 + 2 * RCU_TXN_LOCAL_POOL; k++)
		local_commit(s, k);
	steals = __rcu_txn_local_steals() - steals;
	migrate_enable();
	kthread_stop(task);

	KUNIT_EXPECT_GT(test, steals, 0);
	KUNIT_EXPECT_PTR_EQ(test, sp.loaded, proxy);
	/* Selector flipped: round 1's new target, not a later round's. */
	KUNIT_EXPECT_PTR_EQ(test, sp.resolved, REUSE_V(0, 1));
}

/*
 * The commit of local_commit(), by hand, holding each round's proxies installed
 * for a while before and after the flip, as reuse_round() does, so that the
 * readers resolve many of them.  The descriptor then goes back to its pool,
 * as rcu_txn_commit() puts it there.
 */
static void local_round(struct reuse *ru, unsigned long k)
{
	struct rcu_txn txn;
	int i;

	rcu_txn_init_local(&txn);
	for (i = 0; i < 2; i++) {
		int j = (k & 1) ? 1 - i : i;

		rcu_txn_record(&txn, &ru->slot[j], REUSE_V(j, k - 1),
			       REUSE_V(j, k), TAG);
	}
	for (i = 0; i < 2; i++)
		__rcu_txn_record_install(&txn.desc->group, &txn.records[i]);
	udelay(1);
	__rcu_txn_group_commit(&txn.desc->group);
	udelay(1);
	for (i = 0; i < 2; i++)
		__rcu_txn_record_settle(&txn.records[i]);
	__rcu_txn_local_retire(txn.desc);
	__rcu_txn_done(&txn);
}

/*
 * Reuse of descriptors stolen back, against concurrent readers: as
 * rcu_txn_test_resolve_reuse(), through a pool, with more commits than it holds
 * within a grace period.
 */
#define LOCAL_REUSE_ROUNDS	REUSE_ROUNDS

static void rcu_txn_test_local_reuse(struct kunit *test)
{
	unsigned long steals = __rcu_txn_local_steals();
	long proxies;

	if (num_online_cpus() < 2)
		kunit_skip(test, "needs two CPUs online");
	proxies = reuse_test(test, local_round, LOCAL_REUSE_ROUNDS);
	steals = __rcu_txn_local_steals() - steals;
	kunit_info(test, "%lu steals\n", steals);
	/* The readers raced the commits, and descriptors stolen back. */
	KUNIT_EXPECT_GT(test, proxies, 0);
	KUNIT_EXPECT_GT(test, steals, 0);
}

/*
 * Local handles under a raw spinlock with interrupts off, and under a bit
 * spinlock, steals included: neither call_rcu() nor the steal's busy-wait
 * takes a sleeping lock, which PREEMPT_RT with lockdep would report.
 */
static void rcu_txn_test_local_atomic(struct kunit *test)
{
	static DEFINE_RAW_SPINLOCK(raw_lock);
	bool tainted = test_taint(TAINT_WARN);
	unsigned long k = 0, steals;
	struct hlist_bl_head bucket;
	int locks = debug_locks;
	unsigned long flags;
	void __rcu *s[2];
	int i;

	INIT_HLIST_BL_HEAD(&bucket);
	RCU_INIT_POINTER(s[0], REUSE_V(0, 0));
	RCU_INIT_POINTER(s[1], REUSE_V(1, 0));

	steals = __rcu_txn_local_steals();
	raw_spin_lock_irqsave(&raw_lock, flags);
	for (i = 0; i < 4 * RCU_TXN_LOCAL_POOL; i++)
		local_commit(s, ++k);
	raw_spin_unlock_irqrestore(&raw_lock, flags);
	expect_no_report(test, tainted, locks);

	hlist_bl_lock(&bucket);
	for (i = 0; i < 4 * RCU_TXN_LOCAL_POOL; i++)
		local_commit(s, ++k);
	hlist_bl_unlock(&bucket);
	expect_no_report(test, tainted, locks);

	KUNIT_EXPECT_GT(test, __rcu_txn_local_steals() - steals, 0);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s[0]), REUSE_V(0, k));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(s[1]), REUSE_V(1, k));
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
	KUNIT_CASE(rcu_txn_test_list_add),
	KUNIT_CASE(rcu_txn_test_list_del),
	KUNIT_CASE(rcu_txn_test_list_replace),
	KUNIT_CASE(rcu_txn_test_list_proxy_phases),
	KUNIT_CASE(rcu_txn_test_list_compose_adjacent_del),
	KUNIT_CASE(rcu_txn_test_list_compose_del_and_add),
	KUNIT_CASE(rcu_txn_test_list_compose_triple_del),
	KUNIT_CASE(rcu_txn_test_list_move),
	KUNIT_CASE(rcu_txn_test_list_move_phases),
	KUNIT_CASE(rcu_txn_test_list_compose_moves),
	KUNIT_CASE(rcu_txn_test_list_debug_disjoint_compose),
	KUNIT_CASE_SLOW(rcu_txn_test_list_concurrent),
	KUNIT_CASE(rcu_txn_test_commit_atomic),
	KUNIT_CASE(rcu_txn_test_resolve_nest),
	KUNIT_CASE(rcu_txn_test_resolve_wait),
	KUNIT_CASE_SLOW(rcu_txn_test_resolve_reuse),
	KUNIT_CASE(rcu_txn_test_local_commit),
	KUNIT_CASE(rcu_txn_test_local_over_cap),
	KUNIT_CASE(rcu_txn_test_local_recycle),
	KUNIT_CASE(rcu_txn_test_local_steal),
	KUNIT_CASE(rcu_txn_test_local_steal_waits),
	KUNIT_CASE_SLOW(rcu_txn_test_local_reuse),
	KUNIT_CASE(rcu_txn_test_local_atomic),
	{}
};

static struct kunit_suite rcu_txn_test_suite = {
	.name = "rcu_txn",
	.test_cases = rcu_txn_test_cases,
};

kunit_test_suite(rcu_txn_test_suite);

MODULE_DESCRIPTION("KUnit tests of the single-writer RCU pseudo-transactions");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
