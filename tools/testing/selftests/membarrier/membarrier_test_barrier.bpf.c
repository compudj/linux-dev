// SPDX-License-Identifier: GPL-2.0
/*
 * Count the memory barriers and the core serializing instructions which
 * membarrier relies on, for each registered thread.
 *
 * See membarrier_test_barrier.c.
 */
#include <stdbool.h>
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "membarrier_test_barrier.h"

struct mm_struct;

/* The fields which this program reads. CO-RE relocates them. */
struct task_struct {
	struct mm_struct *mm;
} __attribute__((preserve_access_index));

struct pt_regs;

/* Slot of a registered thread, plus one. */
struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, __u32);
} task_slot SEC(".maps");

/* The counters of each slot. User-space maps them. */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, MBT_MAX_SLOTS);
	__type(key, __u32);
	__type(value, struct mbt_counters);
} counters SEC(".maps");

/* What the scheduler owes to the thread which it switched to. */
#define OWES_BARRIER	1
#define OWES_SYNC_CORE	2

struct cpu_state {
	/* Slot, plus one, of the thread which this CPU last switched to. */
	__u32 slot;
	/* OWES_* flags. A probe of the scheduler clears them. */
	__u32 owed;
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct cpu_state);
} cpu_state SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, MBT_MAX_SITES);
	__type(key, __u32);
	__type(value, __u64);
} site_hits SEC(".maps");

/*
 * Only the CPU which runs a thread, or which switches to it, counts for
 * that thread. So the increments need no lock.
 */
static __always_inline void count(__u32 slot, __u32 mask)
{
	struct mbt_counters *c;
	__u32 key = slot - 1;
	int i;

	if (!slot)
		return;
	c = bpf_map_lookup_elem(&counters, &key);
	if (!c)
		return;
	for (i = 0; i < MBT_NR_COUNTERS; i++)
		if (mask & (1U << i))
			c->c[i]++;
}

static __always_inline __u32 slot_of(struct task_struct *task)
{
	__u32 *slot = bpf_task_storage_get(&task_slot, task, 0, 0);

	return slot ? *slot : 0;
}

/* The attach cookie says which counters this instruction increments. */
SEC("kprobe")
int barrier_site(struct pt_regs *ctx)
{
	__u64 cookie = bpf_get_attach_cookie(ctx);
	__u32 site = cookie >> MBT_COOKIE_SITE_SHIFT;
	__u32 mask = cookie & MBT_COOKIE_MASK;
	struct cpu_state *state;
	__u32 zero = 0, slot = 0;
	__u64 *hits;

	hits = bpf_map_lookup_elem(&site_hits, &site);
	if (hits)
		__sync_fetch_and_add(hits, 1);
	if (cookie & MBT_COOKIE_NEXT) {
		/* Executed on behalf of the thread which is scheduled in. */
		state = bpf_map_lookup_elem(&cpu_state, &zero);
		if (!state)
			return 0;
		slot = state->slot;
		if (mask & (1U << MBT_SWITCH_MM))
			state->owed &= ~OWES_BARRIER;
		if (mask & (1U << MBT_SYNC_CORE_SWITCH))
			state->owed &= ~OWES_SYNC_CORE;
	} else {
		slot = slot_of(bpf_get_current_task_btf());
	}
	count(slot, mask);
	return 0;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next)
{
	struct cpu_state *state;
	__u32 zero = 0, slot;

	state = bpf_map_lookup_elem(&cpu_state, &zero);
	if (!state)
		return 0;
	/* The CPU leaves the thread which it last switched to. */
	if (state->owed & OWES_BARRIER)
		count(state->slot, 1U << MBT_SCHED_IN_MISSED);
	if (state->owed & OWES_SYNC_CORE)
		count(state->slot, 1U << MBT_SYNC_CORE_IN_MISSED);
	count(slot_of(prev), 1U << MBT_SCHED_OUT);
	slot = slot_of(next);
	state->slot = slot;
	state->owed = 0;
	/* The cases which need a barrier after the store to rq->curr. */
	if (slot && next->mm && prev->mm != next->mm) {
		state->owed = OWES_BARRIER | OWES_SYNC_CORE;
		count(slot, 1U << MBT_SCHED_IN);
	}
	return 0;
}

/* Give a slot to the calling thread. User-space runs this program. */
SEC("syscall")
int register_thread(struct mbt_register *ctx)
{
	struct mbt_counters *c;
	__u32 key = ctx->slot;
	__u32 *slot;
	int i;

	c = bpf_map_lookup_elem(&counters, &key);
	if (!c)
		return -1;
	for (i = 0; i < MBT_NR_COUNTERS; i++)
		c->c[i] = 0;
	slot = bpf_task_storage_get(&task_slot, bpf_get_current_task_btf(), 0,
				    BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!slot)
		return -1;
	*slot = key + 1;
	return 0;
}

char _license[] SEC("license") = "GPL";
