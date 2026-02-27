// SPDX-License-Identifier: GPL-2.0+ OR MIT
// SPDX-FileCopyrightText: 2025 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>

/*
 * Split Counters With Tree Approximation Propagation
 *
 * * Propagation diagram when reaching batch size thresholds (± batch size):
 *
 * Example diagram for 8 CPUs, with a fan-out of 2 at each level:
 *
 * log2(8) = 3 levels
 *
 * At each level, each pair propagates its values to the next level when
 * reaching the batch size thresholds. The fan-out of each level depends
 * on the topology (see per_nr_cpu_order_config): small systems use a flat
 * tree, where the level 0 counters propagate directly to the root.
 *
 * Level 0:  0    4    2    6    1    5    3    7
 *           |   /     |   /     |   /     |   /
 *           |  /      |  /      |  /      |  /
 *           | /       | /       | /       | /
 * Level 1:  0         2         1         3
 *           |       /           |       /
 *           |    /              |    /
 *           | /                 | /
 * Level 2:  0                   1
 *           |               /
 *           |         /
 *           |   /
 * Level 3:  0
 *
 * CPU c propagates into item (c & (L - 1)) of a level holding L items,
 * so the CPUs sharing an item are not adjacent: CPUs 0 and 4 share
 * level 1 item 0, and CPUs 0, 2, 4, 6 share level 2 item 0. This mapping
 * uses every item of each level and gives each item the same number of
 * CPUs, give or take one, even when nr_cpu_ids is not a power of 2. The
 * diagram lists the CPUs in bit-reversed order to avoid crossing lines.
 *
 * * Approximation accuracy:
 *
 * BATCH(level N): Level N batch size.
 *
 * Example for BATCH(level 0) = 32.
 *
 * BATCH(level 0) =  32
 * BATCH(level 1) =  64
 * BATCH(level 2) = 128
 * BATCH(level N) = BATCH(level 0) * 2^N
 *
 * Each counter keeps the residual it has not propagated yet. Counters
 * above level 0 only receive multiples of BATCH(level N - 1). Once the
 * residual of a counter reaches +/- BATCH(level N), it propagates its
 * multiples of BATCH(level N), truncated toward zero, and subtracts them.
 * The residual of a settled counter therefore stays within
 * (-BATCH(level N), +BATCH(level N)). This hysteresis prevents updates
 * going back and forth around a BATCH(level N) boundary from propagating
 * up the tree each time.
 *
 *            residual        worst case        worst case
 *            per counter     below precise     above precise
 * Level 0:   [-31 .. 31]     248  (8 * 31)     248  (8 * 31)
 * Level 1:   {-32, 0, 32}    128  (4 * 32)     128  (4 * 32)
 * Level 2:   {-64, 0, 64}    128  (2 * 64)     128  (2 * 64)
 * Total:                     504               504
 *
 * The approximate sum is the precise sum minus the residuals, so a
 * settled counter (no update in progress) is within:
 *
 *   precise - 504 <= approx <= precise + 504
 *
 * The accuracy range reported by the API is exact. It adds the level 0
 * residuals of the nr_cpu_ids counters to BATCH(level 0) * over_multiplier,
 * where over_multiplier = sum over levels N >= 1 of
 *   (number of counters at level N) *
 *     (BATCH(level N) - BATCH(level N - 1)) / BATCH(level 0)
 * which is 4 * 1 + 2 * 2 = 8 here:
 *
 *   accuracy = nr_cpu_ids * (BATCH(level 0) - 1) +
 *              BATCH(level 0) * over_multiplier = 248 + 256 = 504
 *
 * Every counter above level 0 is reachable from a CPU below nr_cpu_ids,
 * even when nr_cpu_ids is not a power of 2, so both bounds can be reached.
 *
 * -----
 *
 * Approximate Sum Carry Propagation
 *
 * Let's define a number of counter bits for each level, e.g.:
 *
 * log2(BATCH(level 0)) = log2(32) = 5
 * Let's assume, for this example, a 32-bit architecture (sizeof(long) == 4).
 *
 *               nr_bit        value_mask                      residual
 * Level 0:      5 bits        v                           -31 ..  +31
 * Level 1:      1 bit        (v & ~((1UL << 5) - 1))      -32 ..  +32
 * Level 2:      1 bit        (v & ~((1UL << 6) - 1))      -64 ..  +64
 * Level 3:     25 bits       (v & ~((1UL << 7) - 1))        0 .. 2^32-1
 *
 * Note: Level 0 per-cpu counters are signed bytes. The precise sum adds the
 *       residuals of all levels.
 *
 * Note: Each tree node packs the items of the counters initialized together
 *       in cache lines, which are not shared with other nodes to prevent
 *       false sharing between CPUs.
 *
 * Example with expanded values:
 *
 * counter_add(counter, inc):
 *
 *         if (!inc)
 *                 return;
 *
 *         // Level 0: per-CPU residual within (-32, +32).
 *         old = percpu_read(counter @ Level 0);
 *         do {
 *                 v = old + inc;
 *                 out = v < 0 ? -(-v & ~0b00011111) : (v & ~0b00011111);
 *         } while (!percpu_try_cmpxchg(counter @ Level 0, &old, v - out));
 *         inc = out;
 *         if (!inc)
 *                 return;
 *
 *         // Levels 1 and 2: propagate the multiples of the level batch
 *         // held by the counter, truncated toward zero.
 *         res = atomic_long_add_return(counter @ Level 1, inc);
 *         inc = 0;
 *         while (res >= 0b01000000 || res <= -0b01000000) {
 *                 out = res < 0 ? -(-res & ~0b00111111) : (res & ~0b00111111);
 *                 res = atomic_long_sub_return(counter @ Level 1, out);
 *                 inc += out;
 *         }
 *         if (!inc)
 *                 return;
 *
 *         res = atomic_long_add_return(counter @ Level 2, inc);
 *         inc = 0;
 *         while (res >= 0b10000000 || res <= -0b10000000) {
 *                 out = res < 0 ? -(-res & ~0b01111111) : (res & ~0b01111111);
 *                 res = atomic_long_sub_return(counter @ Level 2, out);
 *                 inc += out;
 *         }
 *         if (!inc)
 *                 return;
 *
 *         atomic_long_add(counter @ Level 3, inc);
 */

#define pr_fmt(fmt) "percpu_counter_tree: " fmt

#include <linux/percpu_counter_tree.h>
#include <linux/align.h>
#include <linux/cache.h>
#include <linux/cpumask.h>
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/math.h>
#include <linux/log2.h>
#include <linux/args.h>
#include <kunit/visibility.h>

#define MAX_NR_LEVELS 7

/*
 * The counter configuration is selected at boot time based on the
 * hardware topology.
 */
struct counter_config {
	/*
	 * nr_levels is the number of hierarchical counter tree levels. It
	 * excludes the final level (approximate sum).
	 */
	unsigned char nr_levels;
	/* n-arity of tree nodes for each level from 0 to (nr_levels - 1). */
	unsigned char n_arity_order[MAX_NR_LEVELS];
};

/*
 * The fan-out of each level of the tree is chosen with a heuristic trying to
 * minimize both the tree depth and the fan-out.
 */
#define PCT_COUNTER_CONFIG(order, ...)						\
	[order] = {								\
		.nr_levels = COUNT_ARGS(__VA_ARGS__),				\
		.n_arity_order = { __VA_ARGS__ },				\
	}

/*
 * Up to 32 CPUs, the tree is flat: the level 0 counters propagate directly
 * to the root, like struct percpu_counter. From 33 to 64 CPUs, the level 0
 * counters propagate to 4 intermediate counters, each shared by up to 16
 * CPUs, so the carries of all CPUs do not contend on the root. Beyond, each
 * level has a fan-out of 8, with a smaller level below the root for the
 * remaining order. This keeps the carry rate reaching each tree node low
 * with the RSS batch size of 32, and the approximation error grows as
 * O(nr_cpus * log(nr_cpus)), compared to O(nr_cpus^2) for struct
 * percpu_counter whose batch size grows with the number of CPUs.
 *
 * The tree is shaped for nr_cpu_ids rounded up to a power of 2, and each
 * tree node takes a cache line. A set of 4 counters initialized together,
 * such as the RSS counters of an mm, uses less memory than 4 struct
 * percpu_counter from a few CPUs up to 64, and at most 1.03 times as much
 * beyond when nr_cpu_ids is a power of 2. Between powers of 2, fewer CPUs
 * share the cost of the tree nodes: up to 1.26 times at 3 * 2^n CPUs, and
 * up to 1.7 times just above a power of 2. A lone counter uses one item of
 * each tree node: 3.2 to 3.9 times the memory of a struct percpu_counter
 * when nr_cpu_ids is a power of 2 beyond 64, and up to 6.3 times just
 * above.
 */
static const struct counter_config per_nr_cpu_order_config[] = {
	PCT_COUNTER_CONFIG(0),
	PCT_COUNTER_CONFIG(1, 1),
	PCT_COUNTER_CONFIG(2, 2),
	PCT_COUNTER_CONFIG(3, 3),
	PCT_COUNTER_CONFIG(4, 4),
	PCT_COUNTER_CONFIG(5, 5),
	PCT_COUNTER_CONFIG(6, 4, 2),
	PCT_COUNTER_CONFIG(7, 3, 1, 3),
	PCT_COUNTER_CONFIG(8, 3, 2, 3),
	PCT_COUNTER_CONFIG(9, 3, 3, 3),
	PCT_COUNTER_CONFIG(10, 3, 3, 1, 3),
	PCT_COUNTER_CONFIG(11, 3, 3, 2, 3),
	PCT_COUNTER_CONFIG(12, 3, 3, 3, 3),
	PCT_COUNTER_CONFIG(13, 3, 3, 3, 1, 3),
	PCT_COUNTER_CONFIG(14, 3, 3, 3, 2, 3),
	PCT_COUNTER_CONFIG(15, 3, 3, 3, 3, 3),
	PCT_COUNTER_CONFIG(16, 3, 3, 3, 3, 1, 3),
	PCT_COUNTER_CONFIG(17, 3, 3, 3, 3, 2, 3),
	PCT_COUNTER_CONFIG(18, 3, 3, 3, 3, 3, 3),
	PCT_COUNTER_CONFIG(19, 3, 3, 3, 3, 3, 1, 3),
	PCT_COUNTER_CONFIG(20, 3, 3, 3, 3, 3, 2, 3),
};

#undef PCT_COUNTER_CONFIG

/* The configuration table covers every possible number of CPUs. */
#define MAX_NR_CPUS_ORDER	20
#if NR_CPUS > (1UL << MAX_NR_CPUS_ORDER)
#error "NR_CPUS exceeds the hierarchical counter configuration table"
#endif
static_assert(ARRAY_SIZE(per_nr_cpu_order_config) == MAX_NR_CPUS_ORDER + 1);

/* Hierarchical counter configuration for the hardware topology. */
static const struct counter_config *counter_config __ro_after_init;
/* Order of nr_cpu_ids. */
static unsigned int nr_cpus_order __ro_after_init;
/* Calculate the "over" accuracy for a given batch size (multiplication factor). */
static unsigned long over_multiplier __ro_after_init;

/* Number of tree nodes, including the root. UINT_MAX catches early boot misuses. */
static unsigned int nr_nodes __ro_after_init = UINT_MAX;
/* Tree nodes of the counters whose items fit in one cache line per node. */
static struct kmem_cache *nodes_cachep __ro_after_init;

/*
 * A tree node holds the items of one tree position for each of the
 * counters initialized together by percpu_counter_tree_init_many(), packed
 * in cache lines which are not shared with other nodes. Each item is the
 * residual of the carries received and not propagated yet, within
 * (-batch, +batch) of its tree level when settled. The counters sharing a
 * node are updated by the same CPUs, so packing them does not add
 * contention between unrelated CPUs.
 */
static size_t percpu_counter_tree_node_size(unsigned int nr_counters)
{
	return ALIGN(nr_counters * sizeof(atomic_long_t), SMP_CACHE_BYTES);
}

/* Allocate the zeroed tree nodes of @nr_counters counters, the root last. */
static atomic_long_t *percpu_counter_tree_alloc_nodes(unsigned int nr_counters, gfp_t gfp_flags)
{
	size_t node_size = percpu_counter_tree_node_size(nr_counters);

	if (node_size == SMP_CACHE_BYTES)
		return kmem_cache_zalloc(nodes_cachep, gfp_flags);
	/* kmalloc() aligns power of 2 sizes on their size. */
	return kzalloc(roundup_pow_of_two(nr_nodes * node_size), gfp_flags);
}

static void percpu_counter_tree_free_nodes(atomic_long_t *nodes, unsigned int nr_counters)
{
	if (percpu_counter_tree_node_size(nr_counters) == SMP_CACHE_BYTES)
		kmem_cache_free(nodes_cachep, nodes);
	else
		kfree(nodes);
}

static
int __percpu_counter_tree_init(struct percpu_counter_tree *counter,
			       unsigned long batch_size,
			       signed char __percpu *level0,
			       atomic_long_t *items, unsigned int items_stride)
{
	/* Batch size must be greater than 1, a power of 2, and fit in a byte. */
	if (WARN_ON(batch_size <= 1 || (batch_size & (batch_size - 1)) ||
		    batch_size > PERCPU_COUNTER_TREE_MAX_BATCH))
		return -EINVAL;
	counter->batch_order = ilog2(batch_size);
	counter->bias = 0;
	counter->level0 = level0;
	counter->items_stride = items_stride;
	/* The root is in the last node. */
	counter->approx_sum = &items[(nr_nodes - 1) * items_stride];
	return 0;
}

static inline
unsigned long percpu_counter_tree_batch_size(struct percpu_counter_tree *counter)
{
	return 1UL << counter->batch_order;
}

/* This counter's item of the first tree node. */
static inline
atomic_long_t *percpu_counter_tree_items(struct percpu_counter_tree *counter)
{
	return counter->approx_sum - (nr_nodes - 1) * counter->items_stride;
}

/*
 * The residuals of all levels are within (-batch, +batch) of their level,
 * so the approximation is within the same distance of the precise sum in
 * both directions: the largest level 0 residuals of the nr_cpu_ids
 * counters, plus the largest residuals of the counters above level 0. See
 * "Approximation accuracy" at the top of this file.
 */
static inline
unsigned long percpu_counter_tree_accuracy(struct percpu_counter_tree *counter)
{
	unsigned long batch_size = percpu_counter_tree_batch_size(counter);

	return nr_cpu_ids * (batch_size - 1) + batch_size * over_multiplier;
}

/**
 * percpu_counter_tree_init_many() - Initialize many per-CPU counter trees.
 * @counters: An array of @nr_counters counters to initialize.
 *	      Their memory is provided by the caller.
 * @nr_counters: The number of counter trees to initialize
 * @batch_size: The batch size is the increment step at level 0 which triggers a
 *		carry propagation.
 *		The batch size is required to be greater than 1, a power of 2, and at
 *		most PERCPU_COUNTER_TREE_MAX_BATCH.
 * @gfp_flags: gfp flags to pass to the per-CPU and tree node allocators.
 *
 * Initialize many per-CPU counter trees, with a single allocation of
 * per-CPU counters and a single allocation of tree nodes for the
 * @nr_counters counters. Each tree node packs the items of the
 * @nr_counters counters in cache lines. When the items of a node fit in
 * one cache line (up to 8 counters on 64-bit with 64-byte cache lines),
 * the tree nodes come from a slab cache; beyond, their allocation is
 * rounded up to a power of 2, which can double it.
 *
 * Return:
 * * %0: Success
 * * %-EINVAL:		- Invalid @batch_size argument
 * * %-ENOMEM:		- Out of memory
 */
int percpu_counter_tree_init_many(struct percpu_counter_tree *counters,
				  unsigned int nr_counters, unsigned long batch_size,
				  gfp_t gfp_flags)
{
	unsigned int items_stride = percpu_counter_tree_node_size(nr_counters) /
				    sizeof(atomic_long_t);
	signed char __percpu *level0;
	atomic_long_t *items;
	unsigned int i;
	int ret;

	/* The tree shape is selected by percpu_counter_tree_subsystem_init() at boot. */
	if (WARN_ON_ONCE(nr_nodes == UINT_MAX))
		return -EINVAL;
	level0 = __alloc_percpu_gfp(nr_counters * sizeof(*level0),
				    __alignof__(*counters->level0), gfp_flags);
	if (!level0)
		return -ENOMEM;
	items = percpu_counter_tree_alloc_nodes(nr_counters, gfp_flags);
	if (!items) {
		ret = -ENOMEM;
		goto free_level0;
	}
	for (i = 0; i < nr_counters; i++) {
		ret = __percpu_counter_tree_init(&counters[i], batch_size, level0 + i,
						 items + i, items_stride);
		if (ret)
			goto free_items;
	}
	return 0;

free_items:
	percpu_counter_tree_free_nodes(items, nr_counters);
free_level0:
	free_percpu(level0);
	return ret;
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_init_many);

/**
 * percpu_counter_tree_init() - Initialize one per-CPU counter tree.
 * @counter: Counter to initialize.
 *	     Its memory is provided by the caller.
 * @batch_size: The batch size is the increment step at level 0 which triggers a
 *		carry propagation.
 *		The batch size is required to be greater than 1, a power of 2, and at
 *		most PERCPU_COUNTER_TREE_MAX_BATCH.
 * @gfp_flags: gfp flags to pass to the per-CPU and tree node allocators.
 *
 * Initialize one per-CPU counter tree.
 *
 * Return:
 * * %0: Success
 * * %-EINVAL:		- Invalid @batch_size argument
 * * %-ENOMEM:		- Out of memory
 */
int percpu_counter_tree_init(struct percpu_counter_tree *counter,
			     unsigned long batch_size, gfp_t gfp_flags)
{
	return percpu_counter_tree_init_many(counter, 1, batch_size, gfp_flags);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_init);

/**
 * percpu_counter_tree_destroy_many() - Destroy many per-CPU counter trees.
 * @counters: Array of counters trees to destroy.
 * @nr_counters: The number of counter trees to destroy.
 *
 * Release internal resources allocated for @nr_counters per-CPU counter trees.
 * @counters and @nr_counters must be those passed to
 * percpu_counter_tree_init_many().
 */
void percpu_counter_tree_destroy_many(struct percpu_counter_tree *counters,
				      unsigned int nr_counters)
{
	free_percpu(counters->level0);
	/* The first item of the first counter is the base of the tree nodes. */
	percpu_counter_tree_free_nodes(percpu_counter_tree_items(counters), nr_counters);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_destroy_many);

/**
 * percpu_counter_tree_destroy() - Destroy one per-CPU counter tree.
 * @counter: Counter to destroy.
 *
 * Release internal resources allocated for one per-CPU counter tree.
 */
void percpu_counter_tree_destroy(struct percpu_counter_tree *counter)
{
	return percpu_counter_tree_destroy_many(counter, 1);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_destroy);

/*
 * Amount propagated by a counter holding @v: the multiples of @bit_mask,
 * truncated toward zero.
 */
static inline
long percpu_counter_tree_item_carry(long v, unsigned long bit_mask)
{
	if (v < 0)
		return -(long)(-(unsigned long)v & ~(bit_mask - 1));
	return (long)((unsigned long)v & ~(bit_mask - 1));
}

/*
 * It does not matter through which path the carry propagates up the
 * tree, therefore there is no need to disable preemption because the
 * cpu number is only used to favor cache locality.
 */
static
void percpu_counter_tree_add_slowpath(struct percpu_counter_tree *counter, long inc)
{
	unsigned int level_items, nr_levels = counter_config->nr_levels,
		     level, n_arity_order;
	unsigned long bit_mask;
	unsigned int stride = counter->items_stride;
	atomic_long_t *item = percpu_counter_tree_items(counter);
	unsigned int cpu = raw_smp_processor_id();

	n_arity_order = counter_config->n_arity_order[0];
	bit_mask = percpu_counter_tree_batch_size(counter) << n_arity_order;
	level_items = 1U << (nr_cpus_order - n_arity_order);

	for (level = 1; level < nr_levels; level++) {
		/*
		 * A multiple of bit_mask would leave the residual of this
		 * level unchanged: propagate it to the next level directly.
		 * This trades the hysteresis of this level for not touching
		 * its cache line.
		 */
		if (abs(inc) & (bit_mask - 1)) {
			atomic_long_t *count = &item[(cpu & (level_items - 1)) * stride];
			long res, out;

			/*
			 * Add the carry to the residual of this level. While
			 * the residual is outside of (-bit_mask, +bit_mask),
			 * subtract its multiples of bit_mask and propagate them
			 * to the next level.
			 *
			 * Concurrent updaters may subtract from the same
			 * residual based on stale values. For instance, with
			 * a residual of bit_mask/2 and three updaters adding
			 * bit_mask/2 whose additions all complete before any
			 * subtraction, they observe bit_mask, 3*bit_mask/2 and
			 * 2*bit_mask, and subtract bit_mask, bit_mask and
			 * 2*bit_mask, which leaves -2*bit_mask. This is
			 * correct because the read-modify-write operations on
			 * a single atomic are totally ordered, and each one
			 * returns the value resulting from all those preceding
			 * it: whatever is subtracted is propagated, so the sum
			 * is conserved, and the updater performing the last
			 * operation observes the final residual and brings it
			 * back within range. Relaxed ordering is therefore
			 * sufficient for the residuals. The release ordering
			 * of the addition orders the departure of the carry
			 * from the level below before its arrival, for the
			 * precise sum.
			 */
			res = atomic_long_add_return_release(inc, count);
			inc = 0;
			while ((out = percpu_counter_tree_item_carry(res, bit_mask))) {
				res = atomic_long_sub_return_relaxed(out, count);
				inc += out;
			}
			if (likely(!inc))
				return;
		}
		item += level_items * stride;
		n_arity_order = counter_config->n_arity_order[level];
		level_items >>= n_arity_order;
		bit_mask <<= n_arity_order;
	}
	atomic_long_add_return_release(inc, counter->approx_sum);
}

/**
 * percpu_counter_tree_add() - Add to a per-CPU counter tree.
 * @counter: Counter added to.
 * @inc: Increment value (either positive or negative).
 *
 * Add @inc to a per-CPU counter tree. This is a fast-path which will
 * typically increment per-CPU counters as long as there is no carry
 * greater or equal to the counter tree batch size.
 */
void percpu_counter_tree_add(struct percpu_counter_tree *counter, long inc)
{
	unsigned long bit_mask = percpu_counter_tree_batch_size(counter);
	signed char old, new;
	long v, carry;

	/*
	 * Keep the residual within (-batch, +batch) and propagate its
	 * multiples of the batch size, truncated toward zero. This
	 * hysteresis prevents updates going back and forth around a batch
	 * boundary from propagating each time. After a migration or an
	 * interrupt between the read and the cmpxchg, the cmpxchg either
	 * fails and retries with the current residual of the current CPU,
	 * or succeeds on a residual equal to the value read, which the new
	 * value correctly replaces.
	 */
	old = this_cpu_read(*counter->level0);
	do {
		v = old + inc;
		carry = percpu_counter_tree_item_carry(v, bit_mask);
		new = (signed char)(v - carry);
	} while (!this_cpu_try_cmpxchg(*counter->level0, &old, new));
	if (likely(!carry))
		return;
	percpu_counter_tree_add_slowpath(counter, carry);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_add);

/*
 * The precise sum is the sum of the residuals of all tree levels. A carry
 * leaves a level before reaching a higher level, and the release ordering
 * of its arrival orders its departure before it. Reading the levels from
 * the root down, separated by read barriers, therefore never counts a carry
 * twice. A carry in flight during the sum may be missed: the result is
 * precise for a counter without concurrent updates.
 */
static
long percpu_counter_tree_precise_sum_unbiased(struct percpu_counter_tree *counter)
{
	unsigned int nr_levels = counter_config->nr_levels, level_items[MAX_NR_LEVELS], i,
		     stride = counter->items_stride;
	atomic_long_t *item;
	unsigned long sum;
	int level, cpu;

	/* Number of items of each level above level 0. */
	level_items[0] = 1U << nr_cpus_order;
	for (level = 1; level < (int)nr_levels; level++)
		level_items[level] = level_items[level - 1] >>
				     counter_config->n_arity_order[level - 1];
	sum = atomic_long_read(counter->approx_sum);
	smp_rmb();	/* Read each level before the level below it. */
	/* The nodes of each level precede those of the level above it. */
	item = counter->approx_sum;
	for (level = (int)nr_levels - 1; level >= 1; level--) {
		item -= level_items[level] * stride;
		for (i = 0; i < level_items[level]; i++)
			sum += atomic_long_read(&item[i * stride]);
		smp_rmb();	/* Read this level before the level below it. */
	}
	for_each_possible_cpu(cpu)
		sum += (long)READ_ONCE(*per_cpu_ptr(counter->level0, cpu));
	return (long)sum;
}

/**
 * percpu_counter_tree_precise_sum() - Return precise counter sum.
 * @counter: The counter to sum.
 *
 * Querying the precise sum is relatively expensive because it needs to
 * iterate over all CPUs.
 * This is meant to be used when accuracy is preferred over speed.
 *
 * Return: The current precise counter sum.
 */
long percpu_counter_tree_precise_sum(struct percpu_counter_tree *counter)
{
	return percpu_counter_tree_precise_sum_unbiased(counter) + READ_ONCE(counter->bias);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_precise_sum);

/*
 * The comparison between counters is on the wrapped difference. It is valid
 * only when the two counters differ by less than LONG_MAX, and results are
 * undefined beyond that.
 */
static inline
long wrapped_delta(long a, long b)
{
	return (long)((unsigned long)a - (unsigned long)b);
}

/*
 * Each counter's approximation lies within [precise - under, precise + over].
 * The comparison only observes the approximations, so it must reason about
 * the possible precise values around each observed approximation, which
 * lie within [approx - over, approx + under]:
 *
 *                 precise_a range
 *            <-----------+----------->
 *          over_a        |      under_a
 *                    approx_a
 *                        |                         precise_b range
 *                        |                    <-----------+----------->
 *                        |                  over_b        |      under_b
 *                        |                             approx_b
 *                        |                                |
 *                        |<------------ gap ------------->|
 *  ----------------------+--------------------------------+-------------> values
 *
 * The two precise ranges are disjoint, and the comparison can return a
 * definitive answer, only once the gap between approximations exceeds the
 * two facing half-widths:
 *
 *   a below b:  gap > under_a + over_b   (a's upper edge, b's lower edge)
 *   a above b:  gap > over_a + under_b   (a's lower edge, b's upper edge)
 *
 * When comparing against a plain value, the value has zero width, so the
 * margins reduce to that single counter's half-widths: the value is above
 * the counter once it exceeds approx + under, and below it once it is less
 * than approx - over.
 *
 * accuracy_neg is used when delta < 0, and accuracy_pos is used when delta >= 0.
 */
static
int compare_delta(long delta, unsigned long accuracy_neg, unsigned long accuracy_pos)
{
	if (delta >= 0) {
		if (delta <= accuracy_pos)
			return 0;
		else
			return 1;
	} else {
		if (-delta <= accuracy_neg)
			return 0;
		else
			return -1;
	}
}

/**
 * percpu_counter_tree_approximate_compare - Approximated comparison of two counter trees.
 * @a: First counter to compare.
 * @b: Second counter to compare.
 *
 * Evaluate an approximate comparison of two counter trees. This approximation
 * comparison is fast, and provides an accurate answer if the counters are
 * found to be either less than or greater than the other. However, if the
 * approximated comparison returns 0, the counters respective sums are found to
 * be within the two counters accuracy range. The two counters are read
 * independently; the result is not an atomic snapshot of both.
 * The comparison between counters is on the wrapped difference. It is valid
 * only when the two counters differ by less than LONG_MAX, and results are
 * undefined beyond that.
 *
 * Return:
 * * %0		- Counters @a and @b do not differ by more than the sum of their respective
 *                accuracy ranges.
 * * %-1	- Counter @a less than counter @b.
 * * %1		- Counter @a is greater than counter @b.
 */
int percpu_counter_tree_approximate_compare(struct percpu_counter_tree *a,
					    struct percpu_counter_tree *b)
{
	unsigned long accuracy = percpu_counter_tree_accuracy(a) + percpu_counter_tree_accuracy(b);

	/* See the range geometry above compare_delta(). */
	return compare_delta(wrapped_delta(percpu_counter_tree_approximate_sum(a),
					   percpu_counter_tree_approximate_sum(b)),
			     accuracy, accuracy);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_approximate_compare);

/**
 * percpu_counter_tree_approximate_compare_value - Approximated comparison of a counter tree
 *						   against a given value.
 * @counter: Counter to compare.
 * @v: Value to compare.
 *
 * Evaluate an approximate comparison of a counter tree against a given value.
 * This approximation comparison is fast, and provides an accurate
 * answer if the counter is found to be either less than or greater
 * than the value. However, if the approximated comparison returns
 * 0, the value is within the counter accuracy range.
 * The comparison between counters is on the wrapped difference. It is valid
 * only when @v and the counter differ by less than LONG_MAX, and results are
 * undefined beyond that.
 *
 * Return:
 * * %0		- The value @v is within the accuracy range of the counter.
 * * %-1	- The value @v is less than the counter.
 * * %1		- The value @v is greater than the counter.
 */
int percpu_counter_tree_approximate_compare_value(struct percpu_counter_tree *counter, long v)
{
	unsigned long accuracy = percpu_counter_tree_accuracy(counter);

	/* See the range geometry above compare_delta(). */
	return compare_delta(wrapped_delta(v, percpu_counter_tree_approximate_sum(counter)),
			     accuracy, accuracy);
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_approximate_compare_value);

/**
 * percpu_counter_tree_precise_compare - Precise comparison of two counter trees.
 * @a: First counter to compare.
 * @b: Second counter to compare.
 *
 * Evaluate a precise comparison of two counter trees. As an optimization, it
 * uses the approximate counter comparison to quickly compare counters which
 * are far apart. Only cases where counter sums are within the accuracy range
 * require precise counter sums. The two counters are read independently; the
 * result is not an atomic snapshot of both.
 * The comparison between counters is on the wrapped difference. It is valid
 * only when the two counters differ by less than LONG_MAX, and results are
 * undefined beyond that.
 *
 * Return:
 * * %0		- Counters are equal.
 * * %-1	- Counter @a less than counter @b.
 * * %1		- Counter @a is greater than counter @b.
 */
int percpu_counter_tree_precise_compare(struct percpu_counter_tree *a,
					struct percpu_counter_tree *b)
{
	long count_a = percpu_counter_tree_approximate_sum(a),
	     count_b = percpu_counter_tree_approximate_sum(b);
	unsigned long accuracy_a = percpu_counter_tree_accuracy(a),
		      accuracy_b = percpu_counter_tree_accuracy(b);
	long delta = wrapped_delta(count_a, count_b);
	int res;

	/* See the range geometry above compare_delta(). */
	res = compare_delta(delta, accuracy_a + accuracy_b, accuracy_a + accuracy_b);
	/* The values are distanced enough for an accurate approximated comparison. */
	if (res)
		return res;

	/*
	 * The approximated comparison is within the accuracy range, therefore at least one
	 * precise sum is needed. Sum the counter which has the largest accuracy first.
	 */
	if (accuracy_b < accuracy_a) {
		count_a = percpu_counter_tree_precise_sum(a);
		/* precise_a - approx_b: only b's precise range remains. */
		res = compare_delta(wrapped_delta(count_a, count_b), accuracy_b, accuracy_b);
		if (res)
			return res;
		/* Precise sum of second counter is required. */
		count_b = percpu_counter_tree_precise_sum(b);
	} else {
		count_b = percpu_counter_tree_precise_sum(b);
		/* approx_a - precise_b: only a's precise range remains. */
		res = compare_delta(wrapped_delta(count_a, count_b), accuracy_a, accuracy_a);
		if (res)
			return res;
		/* Precise sum of second counter is required. */
		count_a = percpu_counter_tree_precise_sum(a);
	}
	delta = wrapped_delta(count_a, count_b);
	if (delta < 0)
		return -1;
	if (delta > 0)
		return 1;
	return 0;
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_precise_compare);

/**
 * percpu_counter_tree_precise_compare_value - Precise comparison of a counter tree against
 *					       a given value.
 * @counter: Counter to compare.
 * @v: Value to compare.
 *
 * Evaluate a precise comparison of a counter tree against a given value. As
 * an optimization, it uses the approximate counter comparison to quickly
 * identify whether the counter and value are far apart. Only cases where the
 * value is within the counter accuracy range require a precise counter sum.
 * The comparison between counters is on the wrapped difference. It is valid
 * only when @v and the counter differ by less than LONG_MAX, and results are
 * undefined beyond that.
 *
 * Return:
 * * %0		- The value @v is equal to the counter.
 * * %-1	- The value @v is less than the counter.
 * * %1		- The value @v is greater than the counter.
 */
int percpu_counter_tree_precise_compare_value(struct percpu_counter_tree *counter, long v)
{
	long count = percpu_counter_tree_approximate_sum(counter), delta;
	unsigned long accuracy = percpu_counter_tree_accuracy(counter);
	int res;

	/* See the range geometry above compare_delta(). */
	res = compare_delta(wrapped_delta(v, count), accuracy, accuracy);
	/* The values are distanced enough for an accurate approximated comparison. */
	if (res)
		return res;

	/* Precise sum is required. */
	count = percpu_counter_tree_precise_sum(counter);
	delta = wrapped_delta(v, count);
	if (delta < 0)
		return -1;
	if (delta > 0)
		return 1;
	return 0;
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_precise_compare_value);

static
void percpu_counter_tree_set_bias(struct percpu_counter_tree *counter, long bias)
{
	WRITE_ONCE(counter->bias, bias);
}

/**
 * percpu_counter_tree_set - Set the counter tree sum to a given value.
 * @counter: Counter to set.
 * @v: Value to set.
 *
 * Set the counter sum to a given value. It can be useful for instance
 * to reset the counter sum to 0. Note that even after setting the
 * counter sum to a given value, the counter sum approximation can
 * return any value within the accuracy range around that value.
 *
 * The caller is responsible for preventing concurrent updates and
 * precise sums of @counter while it is being set.
 */
void percpu_counter_tree_set(struct percpu_counter_tree *counter, long v)
{
	percpu_counter_tree_set_bias(counter,
				     v - percpu_counter_tree_precise_sum_unbiased(counter));
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_set);

/**
 * percpu_counter_tree_approximate_accuracy_range - Query the accuracy range for a counter tree.
 * @counter: Counter to query.
 * @under: Pointer to a variable to be incremented of the approximation
 *         accuracy range below the precise sum.
 * @over: Pointer to a variable to be incremented of the approximation
 *        accuracy range above the precise sum.
 *
 * Query the accuracy range limits for the counter.
 * The approximation differs from the precise sum by the residuals not yet propagated
 * up the tree: "under" bounds the positive residuals, and "over" the negative residuals.
 * The residuals of all levels are symmetric, so both bounds are equal.
 * Those values are derived from the hardware topology and the counter tree batch size.
 * They are invariant for a given counter tree.
 * The bounds describe a settled counter: one with no update in progress.
 * An update in flight between its per-CPU update and its carry propagation is not covered.
 * Using this function should not be typically required, see the following functions instead:
 * * percpu_counter_tree_approximate_compare(),
 * * percpu_counter_tree_approximate_compare_value(),
 * * percpu_counter_tree_precise_compare(),
 * * percpu_counter_tree_precise_compare_value().
 */
void percpu_counter_tree_approximate_accuracy_range(struct percpu_counter_tree *counter,
						    unsigned long *under, unsigned long *over)
{
	unsigned long accuracy = percpu_counter_tree_accuracy(counter);

	*under += accuracy;
	*over += accuracy;
}
EXPORT_SYMBOL_GPL(percpu_counter_tree_approximate_accuracy_range);

/* Calculate the number of tree nodes and the accuracy multiplier. */
static void __init calculate_topology(void)
{
	unsigned int nr_levels = counter_config->nr_levels, level;
	unsigned int level_items = 1U << nr_cpus_order;
	unsigned long batch_size = 1;

	/* The root node, and the nodes of each level above level 0. */
	nr_nodes = 1;

	/*
	 * The multiplier is derived from a batch size of 1 to speed up
	 * calculating the accuracy at tree initialization. A level N
	 * counter holds at most BATCH(level N) - BATCH(level N - 1) in
	 * either direction.
	 */
	for (level = 1; level < nr_levels; level++) {
		unsigned int n_arity_order = counter_config->n_arity_order[level - 1];

		level_items >>= n_arity_order;
		nr_nodes += level_items;
		over_multiplier += ((batch_size << n_arity_order) - batch_size) * level_items;
		batch_size <<= n_arity_order;
	}
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * Validate the configuration table: the n-arity orders of each entry must
 * sum to its order.
 *
 * Return: The number of invalid entries.
 */
unsigned int percpu_counter_tree_check_config(void)
{
	unsigned int order, nr_errors = 0;

	for (order = 0; order < ARRAY_SIZE(per_nr_cpu_order_config); order++) {
		const struct counter_config *c = &per_nr_cpu_order_config[order];
		unsigned int level, shift = 0;

		for (level = 0; level < c->nr_levels; level++)
			shift += c->n_arity_order[level];
		if (shift != order) {
			pr_err("order %u: arity sum %u\n", order, shift);
			nr_errors++;
		}
	}
	return nr_errors;
}
EXPORT_SYMBOL_IF_KUNIT(percpu_counter_tree_check_config);

/*
 * Check the invariants of a settled counter (no update in progress):
 * - a level 0 counter is within (-BATCH(level 0), +BATCH(level 0)),
 * - a counter at level N, above level 0, holds a multiple of
 *   BATCH(level N - 1) within (-BATCH(level N), +BATCH(level N)),
 * - the root holds a multiple of the batch of the last level.
 *
 * Return: The number of violated invariants.
 */
unsigned int percpu_counter_tree_check_residuals(struct percpu_counter_tree *counter)
{
	unsigned int nr_levels = counter_config->nr_levels, level, level_items,
		     nr_errors = 0, stride = counter->items_stride;
	atomic_long_t *item = percpu_counter_tree_items(counter);
	unsigned long lo = percpu_counter_tree_batch_size(counter);
	long root;
	int cpu;

	for_each_possible_cpu(cpu) {
		long v = READ_ONCE(*per_cpu_ptr(counter->level0, cpu));

		if (v <= -(long)lo || v >= (long)lo) {
			pr_err("cpu %d: level 0 residual %ld, batch %lu\n", cpu, v, lo);
			nr_errors++;
		}
	}
	level_items = 1U << nr_cpus_order;
	for (level = 1; level < nr_levels; level++) {
		unsigned int n_arity_order = counter_config->n_arity_order[level - 1], i;
		unsigned long hi = lo << n_arity_order;

		level_items >>= n_arity_order;
		for (i = 0; i < level_items; i++) {
			long v = atomic_long_read(&item[i * stride]);

			if (v <= -(long)hi || v >= (long)hi || ((unsigned long)v & (lo - 1))) {
				pr_err("level %u item %u: residual %ld, batch %lu\n",
				       level, i, v, hi);
				nr_errors++;
			}
		}
		item += level_items * stride;
		lo = hi;
	}
	root = atomic_long_read(counter->approx_sum);
	if ((unsigned long)root & (lo - 1)) {
		pr_err("root %ld: not a multiple of %lu\n", root, lo);
		nr_errors++;
	}
	return nr_errors;
}
EXPORT_SYMBOL_IF_KUNIT(percpu_counter_tree_check_residuals);

/*
 * Copy the state of counter @src into counter @dst, initialized with the
 * same batch size and without concurrent updates: their per-CPU counters,
 * tree nodes and bias.
 */
void percpu_counter_tree_copy_state(struct percpu_counter_tree *dst,
				    struct percpu_counter_tree *src)
{
	atomic_long_t *dst_items = percpu_counter_tree_items(dst),
		      *src_items = percpu_counter_tree_items(src);
	unsigned int node;
	int cpu;

	for_each_possible_cpu(cpu)
		*per_cpu_ptr(dst->level0, cpu) = *per_cpu_ptr(src->level0, cpu);
	for (node = 0; node < nr_nodes; node++)
		atomic_long_set(&dst_items[node * dst->items_stride],
				atomic_long_read(&src_items[node * src->items_stride]));
	dst->bias = src->bias;
}
EXPORT_SYMBOL_IF_KUNIT(percpu_counter_tree_copy_state);
#endif

/* Format the fan-out of each level, from level 0 to the root, e.g. "8,2,8". */
static int format_shape(char *buf, size_t size, const struct counter_config *config)
{
	unsigned int level;
	int len = 0;

	for (level = 0; level < config->nr_levels; level++)
		len += scnprintf(buf + len, size - len, "%s%u", level ? "," : "",
				 1U << config->n_arity_order[level]);
	return len;
}

int __init percpu_counter_tree_subsystem_init(void)
{
	char shape[8 * MAX_NR_LEVELS + 1] = "";

	nr_cpus_order = get_count_order(nr_cpu_ids);
	if (nr_cpus_order >= ARRAY_SIZE(per_nr_cpu_order_config)) {
		pr_err("Unsupported number of CPUs (%u)\n", nr_cpu_ids);
		return -EINVAL;
	}
	counter_config = &per_nr_cpu_order_config[nr_cpus_order];
	calculate_topology();
	nodes_cachep = kmem_cache_create("percpu_counter_tree_nodes",
					 nr_nodes * SMP_CACHE_BYTES, SMP_CACHE_BYTES,
					 SLAB_PANIC, NULL);
	format_shape(shape, sizeof(shape), counter_config);
	pr_info("%u CPUs, fan-out %s, %u tree nodes\n",
		nr_cpu_ids, nr_cpus_order ? shape : "none",
		nr_nodes);
	return 0;
}
