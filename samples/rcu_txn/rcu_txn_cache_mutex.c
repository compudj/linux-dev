// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * A cache on RCU pseudo-transactions, its writers under one mutex
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Entries are found by key in a hash table of rcu_txn_hlist buckets, and kept
 * in an rcu_txn_list from the most recently inserted to the oldest.  Lookup
 * threads find entries under RCU alone, and mark the ones they find
 * referenced.  Writer threads insert, update and delete entries under one
 * mutex.  The cache holds CACHE_SIZE entries at most: an insertion into a full
 * cache evicts the oldest entry, after giving a second chance at the front of
 * the list to the referenced entries it finds at the tail, CACHE_CHANCES of
 * them at most.
 *
 * Each change is one transaction over the hash table and the list together.
 * An insertion that evicts links the new entry into its bucket and at the
 * front of the list, moves the entries it gives a second chance, and unlinks
 * the victim from its bucket and from the list: at any instant, the cache holds
 * all of it, or none of it.  An update replaces the entry in its bucket and
 * moves the new one to the front of the list.
 *
 * Its records are reserved before the writer takes the mutex, which keeps the
 * allocation out of the critical section.
 *
 * The sample runs for duration_ms when it is loaded, then checks that the hash
 * table and the list hold the same entries, and reports what it did.  An
 * allocation failure stops the writers there.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/hash.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/rcu_txn.h>
#include <linux/rcu_txn_hlist.h>
#include <linux/rcu_txn_list.h>
#include <linux/slab.h>

#define CACHE_HASH_BITS	6
#define CACHE_KEYS	512	/* keys are 0 to CACHE_KEYS - 1 */
#define CACHE_SIZE	128	/* entries held at most */
#define CACHE_CHANCES	4	/* second chances an eviction gives at most */
#define CACHE_THREADS	16	/* of each kind, at most */

/*
 * What an insertion records at most: the entry into its bucket (two records,
 * with its successor's pprev) and at the front of the list (two), the second
 * chances (six each), and the eviction (four).
 */
#define CACHE_RECORDS	(4 + 6 * CACHE_CHANCES + 4)

static unsigned int nr_lookups = 2;
module_param(nr_lookups, uint, 0444);
MODULE_PARM_DESC(nr_lookups, "Lookup threads");

static unsigned int nr_writers = 2;
module_param(nr_writers, uint, 0444);
MODULE_PARM_DESC(nr_writers, "Writer threads");

static unsigned int duration_ms = 2000;
module_param(duration_ms, uint, 0444);
MODULE_PARM_DESC(duration_ms, "How long the threads run, from the load");

struct cache_entry {
	unsigned long key;
	unsigned long value;			/* key + CACHE_KEYS * version */
	struct rcu_txn_hlist_node hnode;	/* in its bucket */
	struct rcu_txn_list_node lru;		/* in the list, newest first */
	struct rcu_head rcu;
	bool referenced;			/* found since it last moved */
};

static struct rcu_txn_hlist_head cache_hash[1 << CACHE_HASH_BITS];
static RCU_TXN_LIST_HEAD(cache_lru);
static DEFINE_MUTEX(cache_mutex);	/* excludes the writers */
static unsigned int cache_count;	/* entries, under cache_mutex */

static atomic_long_t nr_lookup, nr_hit, nr_bad, nr_insert, nr_update,
		     nr_delete, nr_evict, nr_chance, nr_nomem;

/* Set at the first allocation failure, which stops the writers. */
static bool cache_failed;

static void cache_fail(void)
{
	atomic_long_inc(&nr_nomem);
	WRITE_ONCE(cache_failed, true);
}

static struct rcu_txn_hlist_head *cache_bucket(unsigned long key)
{
	return &cache_hash[hash_long(key, CACHE_HASH_BITS)];
}

/* Within an RCU read-side critical section, or with the mutex held. */
static struct cache_entry *cache_find(unsigned long key)
{
	struct cache_entry *e;

	rcu_txn_hlist_for_each_entry_rcu(e, cache_bucket(key), hnode,
					 lockdep_is_held(&cache_mutex))
		if (e->key == key)
			return e;
	return NULL;
}

static int cache_lookup_fn(void *arg)
{
	unsigned long lookups = 0, hits = 0, bad = 0;

	while (!kthread_should_stop()) {
		unsigned long key = get_random_u32_below(CACHE_KEYS);
		struct cache_entry *e;

		rcu_read_lock();
		e = cache_find(key);
		if (e) {
			hits++;
			if (e->value % CACHE_KEYS != key)
				bad++;
			if (!READ_ONCE(e->referenced))
				WRITE_ONCE(e->referenced, true);
		}
		rcu_read_unlock();
		lookups++;
		cond_resched();
	}
	atomic_long_add(lookups, &nr_lookup);
	atomic_long_add(hits, &nr_hit);
	atomic_long_add(bad, &nr_bad);
	return 0;
}

/*
 * Record the eviction of the oldest entry that no lookup found since it last
 * moved, after giving the referenced entries at the tail a second chance at
 * the front.  Returns the victim, to free a grace period after the commit.
 */
static struct cache_entry *cache_evict_prepare(struct rcu_txn *txn)
{
	struct rcu_txn_list_node *head = &cache_lru.node;
	struct cache_entry *e;
	unsigned int chances;

	for (chances = 0; ; chances++) {
		/* The tail as this transaction leaves it so far. */
		e = container_of(rcu_txn_list_pending(txn, &head->prev),
				 struct cache_entry, lru);
		if (chances == CACHE_CHANCES || !READ_ONCE(e->referenced))
			break;
		WRITE_ONCE(e->referenced, false);
		rcu_txn_list_move_after_prepare(txn, &e->lru, head);
	}
	atomic_long_add(chances, &nr_chance);
	rcu_txn_hlist_del_prepare(txn, &e->hnode);
	rcu_txn_list_del_prepare(txn, &e->lru);
	return e;
}

/* Insert @key, or update it, at version @version. */
static void cache_insert(unsigned long key, unsigned long version)
{
	struct cache_entry *new, *old, *victim = NULL;
	struct rcu_txn txn;

	new = kzalloc_obj(*new, GFP_KERNEL);
	if (!new) {
		cache_fail();
		return;
	}
	new->key = key;
	new->value = key + CACHE_KEYS * version;

	/* An allocation failure is sticky: the commit reports it. */
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_reserve(&txn, CACHE_RECORDS);

	mutex_lock(&cache_mutex);
	old = cache_find(key);
	if (old) {
		/* The new entry takes the old one's place in its bucket. */
		rcu_txn_hlist_replace_prepare(&txn, &old->hnode, &new->hnode);
		rcu_txn_list_del_prepare(&txn, &old->lru);
		victim = old;
	} else {
		if (cache_count == CACHE_SIZE)
			victim = cache_evict_prepare(&txn);
		rcu_txn_hlist_add_head_prepare(&txn, &new->hnode,
					       cache_bucket(key));
	}
	rcu_txn_list_add_after_prepare(&txn, &new->lru, &cache_lru.node);
	if (rcu_txn_commit(&txn)) {
		/* Nothing was published. */
		mutex_unlock(&cache_mutex);
		cache_fail();
		kfree(new);
		return;
	}
	if (!victim)
		cache_count++;
	mutex_unlock(&cache_mutex);

	if (old)
		atomic_long_inc(&nr_update);
	else
		atomic_long_inc(victim ? &nr_evict : &nr_insert);
	if (victim)
		kfree_rcu(victim, rcu);
}

static void cache_delete(unsigned long key)
{
	struct cache_entry *e;
	struct rcu_txn txn;

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_reserve(&txn, 4);

	mutex_lock(&cache_mutex);
	e = cache_find(key);
	if (e) {
		rcu_txn_hlist_del_prepare(&txn, &e->hnode);
		rcu_txn_list_del_prepare(&txn, &e->lru);
	}
	if (rcu_txn_commit(&txn)) {
		mutex_unlock(&cache_mutex);
		cache_fail();
		return;
	}
	if (e)
		cache_count--;
	mutex_unlock(&cache_mutex);

	if (e) {
		atomic_long_inc(&nr_delete);
		kfree_rcu(e, rcu);
	}
}

static int cache_writer_fn(void *arg)
{
	unsigned long version = 0;

	while (!kthread_should_stop()) {
		unsigned long key = get_random_u32_below(CACHE_KEYS);

		/* After an allocation failure, wait to be stopped. */
		if (READ_ONCE(cache_failed)) {
			msleep(20);
			continue;
		}
		if (get_random_u32_below(8))
			cache_insert(key, ++version);
		else
			cache_delete(key);
		cond_resched();
	}
	return 0;
}

/*
 * With the threads stopped: every entry of the hash table is in its bucket and
 * in the list, both ways, and there are cache_count of them.
 */
static bool cache_check(void)
{
	unsigned int in_hash = 0, in_list = 0, in_reverse = 0, i;
	struct cache_entry *e;
	bool ok = true;

	rcu_read_lock();
	for (i = 0; i < ARRAY_SIZE(cache_hash); i++) {
		rcu_txn_hlist_for_each_entry_rcu(e, &cache_hash[i], hnode) {
			ok &= cache_find(e->key) == e;
			in_hash++;
		}
	}
	rcu_txn_list_for_each_entry_rcu(e, &cache_lru, lru) {
		ok &= cache_find(e->key) == e;
		in_list++;
	}
	rcu_txn_list_for_each_entry_reverse_rcu(e, &cache_lru, lru)
		in_reverse++;
	rcu_read_unlock();
	return ok && in_hash == cache_count && in_list == cache_count &&
	       in_reverse == cache_count;
}

/*
 * With the threads stopped, no reader or writer is left: free every entry
 * without unlinking it, which allocates nothing and so cannot fail.
 */
static void cache_free_all(void)
{
	struct rcu_txn_list_node *node, *next;
	unsigned int i;

	for (node = rcu_txn_list_next_check(&cache_lru.node, true);
	     node != &cache_lru.node; node = next) {
		next = rcu_txn_list_next_check(node, true);
		kfree(container_of(node, struct cache_entry, lru));
	}
	rcu_txn_list_init(&cache_lru);
	for (i = 0; i < ARRAY_SIZE(cache_hash); i++)
		rcu_txn_hlist_init(&cache_hash[i]);
	cache_count = 0;
}

static int __init cache_init(void)
{
	struct task_struct *threads[2 * CACHE_THREADS];
	unsigned int i, n = 0;
	bool ok;

	for (i = 0; i < min(nr_lookups, CACHE_THREADS); i++)
		threads[n++] = kthread_run(cache_lookup_fn, NULL,
					   "rcu_txn_lookup/%u", i);
	for (i = 0; i < min(nr_writers, CACHE_THREADS); i++)
		threads[n++] = kthread_run(cache_writer_fn, NULL,
					   "rcu_txn_writer/%u", i);
	msleep(duration_ms);
	for (i = 0; i < n; i++)
		if (!IS_ERR(threads[i]))
			kthread_stop(threads[i]);

	ok = cache_check();
	pr_info("%ld lookups, %ld hits, %ld bad; %ld insertions, %ld updates, %ld deletions, %ld evictions, %ld second chances, %ld allocation failures; %u entries\n",
		atomic_long_read(&nr_lookup), atomic_long_read(&nr_hit),
		atomic_long_read(&nr_bad), atomic_long_read(&nr_insert),
		atomic_long_read(&nr_update), atomic_long_read(&nr_delete),
		atomic_long_read(&nr_evict), atomic_long_read(&nr_chance),
		atomic_long_read(&nr_nomem), cache_count);
	if (!ok || atomic_long_read(&nr_bad))
		pr_err("the hash table and the list disagree\n");
	if (READ_ONCE(cache_failed))
		pr_warn("an allocation failed, which stopped the writers\n");
	cache_free_all();
	return 0;
}

static void __exit cache_exit(void)
{
}

module_init(cache_init);
module_exit(cache_exit);

MODULE_DESCRIPTION("A cache on RCU pseudo-transactions, its writers under one mutex");
MODULE_LICENSE("GPL");
