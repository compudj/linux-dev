// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * HPREF: Hazard Pointers Protected Reference Counters
 */

#include <linux/hpref.h>
#include <linux/percpu.h>

DEFINE_PER_CPU(struct hpref_slot, hpref_percpu_slots);

static
void hpref_release(struct hpref_node *node)
{
	node->release(node);
}

/*
 * hpref_synchronize: Wait for hazard pointer slots to be cleared.
 *
 * Wait to observe that each slot contains a value that differs from
 * @node.
 */
void hpref_synchronize(struct hpref_node *node)
{
	int cpu;

	if (!node)
		return;
	/* Memory ordering: Store A before Load B. */
	smp_mb();
	/* Scan all CPUs slots. */
	for_each_possible_cpu(cpu) {
		struct hpref_slot *slot = per_cpu_ptr(&hpref_percpu_slots, cpu);

		/* Busy-wait if node is found. */
		while ((smp_load_acquire(&slot->node)) == node)	/* Load B */
			cpu_relax();
	}
}

void hpref_refcount_dec(struct hpref_node *node)
{
	if (!node)
		return;
	if (refcount_dec_and_test(&node->refcount))
		hpref_release(node);
}
