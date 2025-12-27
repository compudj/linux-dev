// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _LINUX_HAZPTR_H
#define _LINUX_HAZPTR_H

/*
 * hazptr: Hazard Pointers
 *
 * This API provides existence guarantees of objects through hazard
 * pointers.
 *
 * Its main benefit over RCU is that it allows fast reclaim of
 * HP-protected pointers without needing to wait for a grace period.
 *
 * References:
 *
 * [1]: M. M. Michael, "Hazard pointers: safe memory reclamation for
 *      lock-free objects," in IEEE Transactions on Parallel and
 *      Distributed Systems, vol. 15, no. 6, pp. 491-504, June 2004
 */

#include <linux/percpu.h>
#include <linux/types.h>
#include <linux/cleanup.h>
#include <linux/sched.h>

/* 8 slots (each sizeof(void *)) fit in a single cache line. */
#define NR_HAZPTR_PERCPU_SLOTS	8

/*
 * Hazard pointer slot.
 */
struct hazptr_slot {
	void *addr;
};

struct hazptr_overflow_list;

struct hazptr_backup_slot {
	struct hlist_node overflow_node;
	struct hazptr_slot slot;
	/* Overflow list where the backup slot is added. */
	struct hazptr_overflow_list *overflow_list;
};

struct hazptr_ctx {
	struct hazptr_slot *slot;
	/* Backup slot in case all per-CPU slots are used. */
	struct hazptr_backup_slot backup_slot;
	struct hlist_node preempt_node;
};

struct hazptr_slot_ctx {
	struct hazptr_ctx *ctx;
};

struct hazptr_percpu_slots {
	struct hazptr_slot slots[NR_HAZPTR_PERCPU_SLOTS];
	/*
	 * The slot ctx array is populated with the same index as
	 * slot array. The slot ctx items contain pointers which are
	 * only meaningful when the corresponding slot addr is non-NULL.
	 */
	struct hazptr_slot_ctx slots_ctx[NR_HAZPTR_PERCPU_SLOTS];
} ____cacheline_aligned;

DECLARE_PER_CPU(struct hazptr_percpu_slots, hazptr_percpu_slots);

void *__hazptr_acquire(struct hazptr_ctx *ctx, void * const * addr_p, struct hazptr_slot *slot);

/*
 * hazptr_synchronize: Wait until @addr is released from all slots.
 *
 * Wait to observe that each slot contains a value that differs from
 * @addr before returning.
 * Should be called from preemptible context.
 */
void hazptr_synchronize(void *addr);

/*
 * hazptr_chain_backup_slot: Chain backup slot into overflow list.
 *
 * Set backup slot address to @addr, and chain it into the overflow
 * list.
 */
struct hazptr_slot *hazptr_chain_backup_slot(struct hazptr_ctx *ctx);

/*
 * hazptr_unchain_backup_slot: Unchain backup slot from overflow list.
 */
void hazptr_unchain_backup_slot(struct hazptr_ctx *ctx);

static inline
bool hazptr_slot_is_backup(struct hazptr_ctx *ctx, struct hazptr_slot *slot)
{
	return slot == &ctx->backup_slot.slot;
}

static inline
void hazptr_note_context_switch(void)
{
	struct hazptr_percpu_slots *percpu_slots = this_cpu_ptr(&hazptr_percpu_slots);
	unsigned int idx;

	for (idx = 0; idx < NR_HAZPTR_PERCPU_SLOTS; idx++) {
		struct hazptr_slot *slot = &percpu_slots->slots[idx], *backup_slot;
		struct hazptr_ctx *ctx;

		if (!slot->addr)
			continue;
		ctx = percpu_slots->slots_ctx[idx].ctx;
		backup_slot = hazptr_chain_backup_slot(ctx);
		/*
		 * Move hazard pointer from per-CPU slot to backup slot.
		 * This requires hazard pointer synchronize to iterate
		 * on per-CPU slots with load-acquire before iterating
		 * on the overflow list.
		 */
		WRITE_ONCE(backup_slot->addr, slot->addr);
		/*
		 * store-release orders store to backup slot addr before
		 * store to per-CPU slot addr.
		 */
		smp_store_release(&slot->addr, NULL);
		/* Use the backup slot for context. */
		ctx->slot = backup_slot;
	}
}

/*
 * hazptr_acquire: Load pointer at address and protect with hazard pointer.
 *
 * Load @addr_p, and protect the loaded pointer with hazard pointer.
 * When using hazptr_acquire from interrupt handlers, the acquired slots
 * need to be released before returning from the interrupt handler.
 *
 * Returns a non-NULL protected address if the loaded pointer is non-NULL.
 * Returns NULL if the loaded pointer is NULL.
 *
 * On success the protected hazptr slot is stored in @ctx->slot.
 */
static inline
void *hazptr_acquire(struct hazptr_ctx *ctx, void * const * addr_p)
{
	struct hazptr_percpu_slots *percpu_slots;
	struct hazptr_slot *slot;
	void *addr, *addr2;

	/*
	 * Load @addr_p to know which address should be protected.
	 */
	addr = READ_ONCE(*addr_p);
	if (unlikely(!addr))
		return NULL;

	guard(preempt)();
	percpu_slots = this_cpu_ptr(&hazptr_percpu_slots);
	slot = &percpu_slots->slots[0];
	if (unlikely(slot->addr))
		return __hazptr_acquire(ctx, addr_p, NULL);
	percpu_slots->slots_ctx[0].ctx = ctx;

	WRITE_ONCE(slot->addr, addr);	/* Store B */

	/* Memory ordering: Store B before Load A. */
	smp_mb();
	/*
	 * Re-load @addr_p after storing it to the hazard pointer slot.
	 */
	addr2 = READ_ONCE(*addr_p);	/* Load A */
	if (unlikely(!ptr_eq(addr2, addr)))
		return __hazptr_acquire(ctx, addr_p, slot);
	ctx->slot = slot;
	/*
	 * Use addr2 loaded from the second READ_ONCE() to preserve
	 * address dependency ordering.
	 */
	return addr2;
}

/* Release the protected hazard pointer from @slot. */
static inline
void hazptr_release(struct hazptr_ctx *ctx, void *addr)
{
	struct hazptr_slot *slot;

	if (!addr)
		return;
	guard(preempt)();
	slot = ctx->slot;
	WARN_ON_ONCE(slot->addr != addr);
	smp_store_release(&slot->addr, NULL);
	if (unlikely(hazptr_slot_is_backup(ctx, slot)))
		hazptr_unchain_backup_slot(ctx);
}

void hazptr_init(void);

#endif /* _LINUX_HAZPTR_H */
