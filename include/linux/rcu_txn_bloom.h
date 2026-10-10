/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Read-your-own-writes Bloom filter of the RCU pseudo-transactions
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */
#ifndef _LINUX_RCU_TXN_BLOOM_H
#define _LINUX_RCU_TXN_BLOOM_H

/*
 * An RCU pseudo-transaction with read-your-own-writes must decide, per
 * access, whether a slot is already in this transaction's write set.
 * The authoritative test is a linear scan of the records, so a write
 * set of n slots costs O(n) per access and O(n^2) to build -- and the
 * scan is pure overhead on the dominant case, the lookup miss.
 *
 * This Bloom filter is an optimisation to answer the miss in O(1)
 * without false negatives: a clear bit means the slot is definitely
 * absent, so the scan is skipped outright.  All k bits set means
 * present or a false positive, so the scan runs.
 *
 * Correctness never depends on the filter's presence, width or k.  A false
 * positive only costs time: the scan of the records that the filter fails to
 * skip, which then finds the slot absent.  Correctness does depend on the
 * absence of false negatives, which is an obligation on the engine: the filter
 * must be set for every slot recorded while it is live (arming rebuilds it
 * from all records, so this holds from the moment it goes live).
 *
 * This is the mechanism only.  The state belongs to the caller, which
 * provides RCU_TXN_BLOOM_SIZE bytes and owns zeroing them; where they live and
 * when the filter is armed is the engine's policy: see RCU_TXN_BLOOM_MIN in
 * <linux/rcu_txn.h>.  These helpers are pure functions over the array.
 */

#include <linux/compiler_types.h>
#include <linux/types.h>

/*
 * RCU_TXN_BLOOM_WORDS sets the filter width (64 bits each; 16 = 1024 bits).
 * With k bits over m = 64 * WORDS bits and n recorded slots the false-positive
 * rate is ~(1 - e^{-kn/m})^k, which for a sparse filter is (kn/m)^k: doubling
 * the width divides it by ~2^k.
 *
 * Both width and k only ever trade filter cost against the false-positive
 * rate: correctness never depends on either.
 */
#define RCU_TXN_BLOOM_WORDS	16

/*
 * RCU_TXN_BLOOM_K sets the number of hash bits a slot maps to.  Raising k by
 * one multiplies the false-positive rate by the fill factor kn/m: a win only
 * while the filter is sparse.
 */
#define RCU_TXN_BLOOM_K		3

#define RCU_TXN_BLOOM_BITS	(64ULL * RCU_TXN_BLOOM_WORDS)
#define RCU_TXN_BLOOM_SIZE	(sizeof(u64) * RCU_TXN_BLOOM_WORDS)

/*
 * Two independent hashes of the slot, for Kirsch-Mitzenmacher double hashing:
 * one SplitMix64 avalanche (two multiplies), its 64 bits split into two 32-bit
 * lanes.  A single multiply would leave the k positions correlated, slot
 * addresses being aligned and clustered.  h2 is forced odd so that h1 + i*h2
 * visits k distinct bits.
 */
static inline void rcu_txn_bloom_h1h2(void __rcu **slot, u64 *h1, u64 *h2)
{
	/*
	 * Shift by the alignment every slot has, 4 bytes on 32-bit: >> 3 would
	 * map adjacent slots there to the same k positions.  On 64-bit the one
	 * zero bit left is mixed away by the avalanche.
	 */
	u64 x = (u64)(unsigned long)slot >> 2;

	x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27; x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	*h1 = x & 0xffffffffULL;		/* low lane */
	*h2 = (x >> 32) | 1;			/* high lane, odd stride */
}

static inline bool rcu_txn_bloom_test(const u64 *bloom, void __rcu **slot)
{
	u64 h1, h2;
	unsigned int i;

	rcu_txn_bloom_h1h2(slot, &h1, &h2);
	for (i = 0; i < RCU_TXN_BLOOM_K; i++) {
		u64 idx = (h1 + (u64)i * h2) % RCU_TXN_BLOOM_BITS;

		if (!(bloom[idx >> 6] & ((u64)1 << (idx & 63))))
			return false;	/* a clear bit: definitely absent */
	}
	return true;	/* all k bits set: present (or a false positive) */
}

static inline void rcu_txn_bloom_set(u64 *bloom, void __rcu **slot)
{
	u64 h1, h2;
	unsigned int i;

	rcu_txn_bloom_h1h2(slot, &h1, &h2);
	for (i = 0; i < RCU_TXN_BLOOM_K; i++) {
		u64 idx = (h1 + (u64)i * h2) % RCU_TXN_BLOOM_BITS;

		bloom[idx >> 6] |= (u64)1 << (idx & 63);
	}
}

#endif /* _LINUX_RCU_TXN_BLOOM_H */
