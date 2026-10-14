// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * A cache on RCU pseudo-transactions, its writers under a lock per node
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This is the same cache as rcu_txn_cache_mutex.c.  Its writers exclude each
 * other per node instead of under one mutex.  Writers that change entries far
 * apart can then run in parallel.  Every entry, every bucket and the head of
 * the list has a spinlock.  A writer holds the lock of every node whose links
 * its transaction changes.  These are the links of each node:
 * - an entry: its next and pprev in its bucket, its next and prev in the list
 * - a bucket: its first entry
 * - the head: the first and the last entry of the list
 *
 * A writer does each operation in four steps.  The operation is one RCU
 * read-side critical section, from the plan to the commit.  The section keeps
 * every node the writer finds in it allocated.
 *
 * 1. Plan, with no lock held.  Find the entry by walking its bucket.  The walk
 *    also gives the node whose next points to the entry.  Read the other nodes
 *    around the entry from the entry's links.
 * 2. Lock these nodes, in the order of the locks' addresses.  So two writers
 *    cannot deadlock.
 * 3. Validate the plan.  No planned node is dead.  The planned links are in
 *    place, in the entry and in its neighbours.  Otherwise release the locks
 *    and go back to step 1.
 * 4. Commit one transaction, then release the locks.
 *
 * All the locks are of one lockdep class.  A comparison function gives lockdep
 * their order.
 *
 * An entry is dead once it is unlinked.  Its hlist node shows it: a delete or
 * a replace sets its pprev to NULL, in the commit that also unlinks it from
 * the list.  That test is local to the node.  An rcu_txn_list keeps the links
 * of a removed node, for the readers that are still on it.  So only the links
 * of its neighbours show that it is gone.  An entry that is in lists only,
 * with no hlist node, would need an explicit flag.
 *
 * The transaction of step 4 records at most RCU_TXN_LOCAL_CAP edges, so it is
 * a local transaction (rcu_txn_init_local()).  It allocates nothing and cannot
 * fail.  A local transaction disables preemption, and on PREEMPT_RT a
 * spinlock_t can sleep.  So the writer starts the transaction only once it
 * holds its locks.  These are the operations:
 * - an insertion links the new entry into its bucket and at the front of the
 *   list
 * - an update replaces the entry in both
 * - a deletion or an eviction unlinks an entry from both
 * - a second chance moves a referenced entry from the tail of the list to its
 *   front
 * Lookup threads find entries under RCU alone.  They mark the entries they find
 * as referenced.
 *
 * Two things differ from the mutex sample in what the cache does.  An insertion
 * into a full cache is several transactions: each second chance, the eviction
 * and the link of the new entry commit on their own.  And the cache holds about
 * CACHE_SIZE entries, not at most: an insertion reads the number of entries
 * with no lock held.
 *
 * The sample runs for duration_ms when it is loaded.  Then it checks that the
 * hash table and the list hold the same entries, and reports what it did.  A
 * failure to allocate an entry stops the writers.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/hash.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/rcu_txn.h>
#include <linux/rcu_txn_hlist.h>
#include <linux/rcu_txn_list.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>

#define CACHE_HASH_BITS	6
#define CACHE_KEYS	512	/* keys are 0 to CACHE_KEYS - 1 */
#define CACHE_SIZE	128	/* about this many entries held */
#define CACHE_CHANCES	4	/* second chances an eviction gives at most */
#define CACHE_THREADS	16	/* of each kind, at most */
#define CACHE_LOCKS	5	/* locks an operation holds at most */

static unsigned int nr_lookups = 2;
module_param(nr_lookups, uint, 0444);
MODULE_PARM_DESC(nr_lookups, "Lookup threads");

static unsigned int nr_writers = 4;
module_param(nr_writers, uint, 0444);
MODULE_PARM_DESC(nr_writers, "Writer threads");

static unsigned int duration_ms = 2000;
module_param(duration_ms, uint, 0444);
MODULE_PARM_DESC(duration_ms, "How long the threads run, from the load");

struct cache_entry {
	spinlock_t lock;			/* its links */
	unsigned long key;
	unsigned long value;			/* key + CACHE_KEYS * version */
	struct rcu_txn_hlist_node hnode;	/* in its bucket */
	struct rcu_txn_list_node lru;		/* in the list, newest first */
	struct rcu_head rcu;
	bool referenced;			/* found since it last moved */
};

struct cache_bucket {
	spinlock_t lock;			/* its first entry */
	struct rcu_txn_hlist_head head;
};

static struct cache_bucket cache_hash[1 << CACHE_HASH_BITS];
static struct {
	spinlock_t lock;			/* the first and last entries */
	struct rcu_txn_list_head head;
} cache_lru = { .head = RCU_TXN_LIST_HEAD_INIT(cache_lru.head) };
static atomic_t cache_count;

static atomic_long_t nr_lookup, nr_hit, nr_bad, nr_insert, nr_update,
		     nr_delete, nr_evict, nr_chance, nr_retry, nr_lost,
		     nr_nomem;

/* Set at the first allocation failure, which stops the writers. */
static bool cache_failed;

static void cache_fail(void)
{
	atomic_long_inc(&nr_nomem);
	WRITE_ONCE(cache_failed, true);
}

#ifdef CONFIG_PROVE_LOCKING
/* The locks of the cache are taken in the order of their addresses. */
static int cache_lock_cmp(const struct lockdep_map *a,
			  const struct lockdep_map *b)
{
	return cmp_int((unsigned long)a, (unsigned long)b);
}
#endif

/* Every cache lock is of one lockdep class, ordered by cache_lock_cmp(). */
static void cache_lock_init(spinlock_t *lock)
{
	spin_lock_init(lock);
	lock_set_cmp_fn(lock, cache_lock_cmp, NULL);
}

static struct cache_bucket *cache_bucket(unsigned long key)
{
	return &cache_hash[hash_long(key, CACHE_HASH_BITS)];
}

static struct cache_entry *hnode_entry(struct rcu_txn_hlist_node *node)
{
	return node ? container_of(node, struct cache_entry, hnode) : NULL;
}

/* Return the entry of a list node, or NULL for the head. */
static struct cache_entry *lru_entry(struct rcu_txn_list_node *node)
{
	if (node == &cache_lru.head.node)
		return NULL;
	return container_of(node, struct cache_entry, lru);
}

static spinlock_t *lru_lock(struct rcu_txn_list_node *node)
{
	struct cache_entry *e = lru_entry(node);

	return e ? &e->lock : &cache_lru.lock;
}

/* Return the first entry of @b.  The caller holds the lock of @b. */
static struct rcu_txn_hlist_node *bucket_first(struct cache_bucket *b)
{
	return rcu_txn_hlist_first_check(&b->head, lockdep_is_held(&b->lock));
}

/* Return the first node of the list.  The caller holds the head's lock. */
static struct rcu_txn_list_node *lru_first(void)
{
	return rcu_txn_list_next_check(&cache_lru.head.node,
				       lockdep_is_held(&cache_lru.lock));
}

/* Find the entry of @key, within an RCU read-side critical section. */
static struct cache_entry *cache_find(unsigned long key)
{
	struct cache_entry *e;

	rcu_txn_hlist_for_each_entry_rcu(e, &cache_bucket(key)->head, hnode)
		if (e->key == key)
			return e;
	return NULL;
}

/* What a writer found under RCU, and the locks it takes. */
struct cache_plan {
	struct cache_entry *e;			/* the entry it works on */
	struct rcu_txn_hlist_node __rcu **pprev; /* the slot pointing to @e */
	struct cache_entry *pprev_entry;	/* the slot's entry, or NULL */
	spinlock_t *pprev_lock;			/* the slot's lock */
	struct rcu_txn_hlist_node *hnext;	/* next in the bucket */
	struct rcu_txn_list_node *prev, *next;	/* in the list */
	struct rcu_txn_list_node *first;	/* of the list */
	spinlock_t *locks[CACHE_LOCKS];		/* taken in address order */
	unsigned int nr_locks;
};

static void cache_want(struct cache_plan *p, spinlock_t *lock)
{
	unsigned int i;

	for (i = 0; i < p->nr_locks; i++)
		if (p->locks[i] == lock)
			return;
	if (!WARN_ON(p->nr_locks == CACHE_LOCKS))
		p->locks[p->nr_locks++] = lock;
}

/* Take the locks in address order. */
static void cache_lock(struct cache_plan *p)
{
	unsigned int i, j;

	for (i = 1; i < p->nr_locks; i++)
		for (j = i; j > 0 && p->locks[j - 1] > p->locks[j]; j--)
			swap(p->locks[j - 1], p->locks[j]);
	for (i = 0; i < p->nr_locks; i++)
		spin_lock(p->locks[i]);
}

static void cache_unlock(struct cache_plan *p)
{
	while (p->nr_locks)
		spin_unlock(p->locks[--p->nr_locks]);
}

/*
 * Find the entry of @key by walking its bucket.  Record in @p the entry and the
 * slot that points to it, with the slot's owner and lock.  The slot is the
 * bucket's first, or the next of the entry before it.  Return the entry, or
 * NULL if there is none.
 */
static struct cache_entry *cache_find_slot(unsigned long key,
					   struct cache_plan *p)
{
	struct cache_bucket *b = cache_bucket(key);
	struct rcu_txn_hlist_node __rcu **slot = &b->head.first;
	struct cache_entry *before = NULL;
	struct rcu_txn_hlist_node *node;

	for (node = rcu_txn_hlist_first_rcu(&b->head); node;
	     node = rcu_txn_hlist_next_rcu(node)) {
		struct cache_entry *e = hnode_entry(node);

		if (e->key == key) {
			p->e = e;
			p->pprev = slot;
			p->pprev_entry = before;
			p->pprev_lock = before ? &before->lock : &b->lock;
			return e;
		}
		slot = &node->next;
		before = e;
	}
	return NULL;
}

/* What an operation on an entry changes besides its list neighbours. */
enum cache_mode {
	CACHE_UNLINK,	/* the bucket, for a deletion or an update */
	CACHE_EVICT,	/* the bucket, only for the tail: an eviction */
	CACHE_FRONT,	/* the front of the list, for a second chance */
};

/* Return true if @e is unlinked.  @e is NULL for a bucket or the head. */
static bool cache_dead(struct cache_entry *e)
{
	return e && rcu_txn_hlist_unhashed(&e->hnode, lockdep_is_held(&e->lock));
}

/* Read a slot of a bucket.  The caller holds the lock of its owner. */
static struct rcu_txn_hlist_node *
hlist_link(struct rcu_txn_hlist_node __rcu **slot, spinlock_t *lock)
{
	return rcu_txn_dereference_check(*slot, RCU_TXN_HLIST_TAG,
					 lockdep_is_held(lock));
}

/* Read a link of the list.  The caller holds the lock of its node. */
static struct rcu_txn_list_node *
lru_link(struct rcu_txn_list_node *node, struct rcu_txn_list_node __rcu **link)
{
	return rcu_txn_dereference_check(*link, RCU_TXN_LIST_TAG,
					 lockdep_is_held(lru_lock(node)));
}

/*
 * Step 3: return true if the plan in @p holds.  The caller holds the locks of
 * @p.  For CACHE_FRONT, check the first node of the list instead of the links
 * in the bucket.
 */
static bool cache_plan_holds(const struct cache_plan *p, enum cache_mode mode)
{
	struct cache_entry *e = p->e;

	if (cache_dead(e) || cache_dead(lru_entry(p->prev)) ||
	    cache_dead(lru_entry(p->next)))
		return false;
	if (lru_link(&e->lru, &e->lru.prev) != p->prev ||
	    lru_link(&e->lru, &e->lru.next) != p->next ||
	    lru_link(p->prev, &p->prev->next) != &e->lru ||
	    lru_link(p->next, &p->next->prev) != &e->lru)
		return false;

	if (mode != CACHE_FRONT) {
		struct cache_entry *n = hnode_entry(p->hnext);

		if (cache_dead(p->pprev_entry) || cache_dead(n))
			return false;
		if (rcu_access_pointer(e->hnode.pprev) != p->pprev ||
		    hlist_link(p->pprev, p->pprev_lock) != &e->hnode ||
		    hlist_link(&e->hnode.next, &e->lock) != p->hnext ||
		    (n && rcu_access_pointer(n->hnode.pprev) != &e->hnode.next))
			return false;
	}
	if (mode == CACHE_FRONT)
		return !cache_dead(lru_entry(p->first)) &&
		       lru_first() == p->first;
	return true;
}

/*
 * Run steps 1 to 3 for an operation of @mode on the entry of @key.  Return
 * true with the locks held.
 *
 * Return false, with no lock held, if there is no such entry.  Also return
 * false if @want is given and the entry is not @want.  An eviction also
 * returns false if the entry is not the tail.  The caller is in an RCU
 * read-side critical section.
 */
static bool cache_lock_entry(struct cache_plan *p, unsigned long key,
			     struct cache_entry *want, enum cache_mode mode)
{
	struct rcu_txn_list_node *head = &cache_lru.head.node;
	bool dead;

	for (;;) {
		p->nr_locks = 0;
		if (!cache_find_slot(key, p) || (want && p->e != want))
			return false;
		p->hnext = rcu_txn_hlist_next_rcu(&p->e->hnode);
		/*
		 * Both links of one node: load the first one with a
		 * load-acquire.  If prev shows a commit, next then shows it
		 * too.  The plan is checked again under the locks, so this
		 * only saves a retry.
		 */
		p->prev = rcu_txn_list_prev_acquire_rcu(&p->e->lru);
		p->next = rcu_txn_list_next_rcu(&p->e->lru);
		p->first = rcu_txn_list_next_rcu(head);
		/*
		 * An eviction takes the tail.  If another writer gave the
		 * entry a second chance, it is no longer the tail.  The caller
		 * then tries the new tail, instead of waiting here.
		 */
		if (mode == CACHE_EVICT && p->next != head)
			return false;

		cache_want(p, &p->e->lock);
		if (mode != CACHE_FRONT) {
			cache_want(p, p->pprev_lock);
			if (p->hnext)
				cache_want(p, &hnode_entry(p->hnext)->lock);
		}
		cache_want(p, lru_lock(p->prev));
		cache_want(p, lru_lock(p->next));
		if (mode == CACHE_FRONT) {
			cache_want(p, &cache_lru.lock);
			cache_want(p, lru_lock(p->first));
		}
		cache_lock(p);
		if (cache_plan_holds(p, mode))
			return true;
		dead = cache_dead(p->e);
		cache_unlock(p);
		if (dead && want)
			return false;
		atomic_long_inc(&nr_retry);
	}
}

/*
 * Unlink the entry of @key from its bucket and from the list.  If @new is
 * given, replace the entry with @new in both instead.
 *
 * If @want is given, act only if the entry is @want.  With @mode CACHE_EVICT,
 * act only while the entry is the tail.  Return 0 on success, or -ENOENT if
 * there is no such entry.  The caller is in the RCU read-side critical section
 * where it found @want.
 */
static int __cache_unlink(unsigned long key, struct cache_entry *want,
			  struct cache_entry *new, enum cache_mode mode)
{
	struct cache_plan p;
	struct rcu_txn txn;

	if (!cache_lock_entry(&p, key, want, mode))
		return -ENOENT;
	/* Five records, or six for a replace: they fit, so it cannot fail. */
	rcu_txn_init_local(&txn);
	if (new) {
		rcu_txn_hlist_replace_prepare(&txn, &p.e->hnode, &new->hnode);
		rcu_txn_list_replace_prepare(&txn, &p.e->lru, &new->lru);
	} else {
		rcu_txn_hlist_del_prepare(&txn, &p.e->hnode);
		rcu_txn_list_del_prepare(&txn, &p.e->lru);
	}
	WARN_ON_ONCE(rcu_txn_commit(&txn));
	cache_unlock(&p);
	kfree_rcu(p.e, rcu);
	if (!new)
		atomic_dec(&cache_count);
	return 0;
}

/* Unlink the entry of @key, or replace it with @new: see __cache_unlink(). */
static int cache_unlink(unsigned long key, struct cache_entry *new)
{
	int err;

	rcu_read_lock();
	err = __cache_unlink(key, NULL, new, CACHE_UNLINK);
	rcu_read_unlock();
	return err;
}

/*
 * Move @e to the front of the list.  Return false if another writer changed @e
 * first, or if @e is first already.  The caller is in the RCU read-side
 * critical section where it found @e.
 */
static bool cache_move_front(struct cache_entry *e)
{
	struct cache_plan p;
	struct rcu_txn txn;
	bool moved;

	if (!cache_lock_entry(&p, e->key, e, CACHE_FRONT))
		return false;
	moved = lru_first() != &e->lru;
	if (moved) {
		/* Six records: they fit, so it cannot fail. */
		rcu_txn_init_local(&txn);
		rcu_txn_list_move_after_prepare(&txn, &e->lru,
						&cache_lru.head.node);
		WARN_ON_ONCE(rcu_txn_commit(&txn));
	}
	cache_unlock(&p);
	return moved;
}

/*
 * Evict the entry at the tail of the list, after at most CACHE_CHANCES second
 * chances.  Each second chance is a transaction of its own.
 */
static void cache_evict(void)
{
	unsigned int chances = 0, moved = 0;

	for (;;) {
		bool evicted = false;
		struct cache_entry *e;

		/* One critical section, from the read of the tail on. */
		rcu_read_lock();
		e = rcu_txn_list_last_or_null_rcu(&cache_lru.head,
						   struct cache_entry, lru);
		if (!e) {
			rcu_read_unlock();
			break;
		}
		if (chances < CACHE_CHANCES && READ_ONCE(e->referenced)) {
			WRITE_ONCE(e->referenced, false);
			if (cache_move_front(e))
				moved++;
			chances++;
		} else {
			evicted = !__cache_unlink(e->key, e, NULL, CACHE_EVICT);
		}
		rcu_read_unlock();
		if (evicted) {
			atomic_long_inc(&nr_evict);
			break;
		}
		/* Another writer moved or unlinked it.  Try the new tail. */
	}
	atomic_long_add(moved, &nr_chance);
}

/*
 * Link @new into its bucket and at the front of the list.  Return 0 on
 * success, or -EEXIST if its key is in the cache already.  The plan of step 1
 * is the first entry of the bucket and the first node of the list.
 */
static int cache_link(struct cache_entry *new)
{
	struct cache_bucket *b = cache_bucket(new->key);
	struct rcu_txn_list_node *head = &cache_lru.head.node;
	struct cache_plan p;
	struct rcu_txn txn;
	bool there;

	rcu_read_lock();
	for (;;) {
		/* The first entries of the bucket and of the list change. */
		p.nr_locks = 0;
		p.hnext = rcu_txn_hlist_first_rcu(&b->head);
		p.first = rcu_txn_list_next_rcu(head);
		cache_want(&p, &b->lock);
		if (p.hnext)
			cache_want(&p, &hnode_entry(p.hnext)->lock);
		cache_want(&p, &cache_lru.lock);
		cache_want(&p, lru_lock(p.first));
		cache_lock(&p);
		if (bucket_first(b) == p.hnext && lru_first() == p.first)
			break;
		cache_unlock(&p);
		atomic_long_inc(&nr_retry);
	}

	/* No other writer inserts into the bucket while its lock is held. */
	there = !!cache_find(new->key);
	if (!there) {
		/* Five records: they fit, so it cannot fail. */
		rcu_txn_init_local(&txn);
		rcu_txn_hlist_add_head_prepare(&txn, &new->hnode, &b->head);
		rcu_txn_list_add_after_prepare(&txn, &new->lru, head);
		WARN_ON_ONCE(rcu_txn_commit(&txn));
		atomic_inc(&cache_count);
	}
	cache_unlock(&p);
	rcu_read_unlock();
	return there ? -EEXIST : 0;
}

/* Insert @key, or update it, at version @version. */
static void cache_insert(unsigned long key, unsigned long version)
{
	struct cache_entry *new;
	bool old;
	int err;

	new = kzalloc_obj(*new, GFP_KERNEL);
	if (!new) {
		cache_fail();
		return;
	}
	cache_lock_init(&new->lock);
	new->key = key;
	new->value = key + CACHE_KEYS * version;

	rcu_read_lock();
	old = !!cache_find(key);
	rcu_read_unlock();
	if (old) {
		err = cache_unlink(key, new);
		if (!err) {
			atomic_long_inc(&nr_update);
			return;
		}
	} else {
		if (atomic_read(&cache_count) >= CACHE_SIZE)
			cache_evict();
		err = cache_link(new);
		if (!err) {
			atomic_long_inc(&nr_insert);
			return;
		}
	}
	atomic_long_inc(&nr_lost);	/* another writer was first */
	kfree(new);
}

static void cache_delete(unsigned long key)
{
	if (!cache_unlink(key, NULL))
		atomic_long_inc(&nr_delete);
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
 * Check the cache with the threads stopped.  Every entry of the hash table and
 * of the list must be found in its bucket.  The hash table, the list and the
 * list walked backward must each hold cache_count entries.
 */
static bool cache_check(void)
{
	unsigned int in_hash = 0, in_list = 0, in_reverse = 0, i;
	unsigned int count = atomic_read(&cache_count);
	struct cache_entry *e;
	bool ok = true;

	rcu_read_lock();
	for (i = 0; i < ARRAY_SIZE(cache_hash); i++) {
		rcu_txn_hlist_for_each_entry_rcu(e, &cache_hash[i].head,
						 hnode) {
			ok &= cache_find(e->key) == e;
			in_hash++;
		}
	}
	rcu_txn_list_for_each_entry_rcu(e, &cache_lru.head, lru) {
		ok &= cache_find(e->key) == e;
		in_list++;
	}
	rcu_txn_list_for_each_entry_reverse_rcu(e, &cache_lru.head, lru)
		in_reverse++;
	rcu_read_unlock();
	return ok && in_hash == count && in_list == count &&
	       in_reverse == count;
}

/*
 * Free every entry without unlinking it.  The threads are stopped, so no reader
 * or writer is left.  This allocates nothing, so it cannot fail.
 */
static void cache_free_all(void)
{
	struct rcu_txn_list_node *head = &cache_lru.head.node;
	struct rcu_txn_list_node *node, *next;
	unsigned int i;

	for (node = rcu_txn_list_next_check(head, true); node != head;
	     node = next) {
		next = rcu_txn_list_next_check(node, true);
		kfree(lru_entry(node));
	}
	rcu_txn_list_init(&cache_lru.head);
	for (i = 0; i < ARRAY_SIZE(cache_hash); i++)
		rcu_txn_hlist_init(&cache_hash[i].head);
	atomic_set(&cache_count, 0);
}

/* Start a thread, or report why not. */
static struct task_struct *cache_start(int (*fn)(void *), const char *name,
				       unsigned int i)
{
	struct task_struct *task = kthread_run(fn, NULL, name, i);

	if (IS_ERR(task))
		pr_err("could not start %s/%u: %ld\n", name, i, PTR_ERR(task));
	return task;
}

static int __init cache_init(void)
{
	struct task_struct *threads[2 * CACHE_THREADS];
	unsigned int i, n = 0;
	int err = 0;
	bool ok;

	for (i = 0; i < ARRAY_SIZE(cache_hash); i++) {
		cache_lock_init(&cache_hash[i].lock);
		rcu_txn_hlist_init(&cache_hash[i].head);
	}
	cache_lock_init(&cache_lru.lock);

	for (i = 0; !err && i < min(nr_lookups, CACHE_THREADS); i++) {
		threads[n] = cache_start(cache_lookup_fn, "rcu_txn_lookup", i);
		err = PTR_ERR_OR_ZERO(threads[n]);
		n += !err;
	}
	for (i = 0; !err && i < min(nr_writers, CACHE_THREADS); i++) {
		threads[n] = cache_start(cache_writer_fn, "rcu_txn_writer", i);
		err = PTR_ERR_OR_ZERO(threads[n]);
		n += !err;
	}
	if (!err)
		msleep(duration_ms);
	for (i = 0; i < n; i++)
		kthread_stop(threads[i]);
	if (err) {
		cache_free_all();
		return err;
	}

	ok = cache_check();
	pr_info("%ld lookups, %ld hits, %ld bad; %ld insertions, %ld updates, %ld deletions, %ld evictions, %ld second chances, %ld retries, %ld lost races, %ld allocation failures; %d entries\n",
		atomic_long_read(&nr_lookup), atomic_long_read(&nr_hit),
		atomic_long_read(&nr_bad), atomic_long_read(&nr_insert),
		atomic_long_read(&nr_update), atomic_long_read(&nr_delete),
		atomic_long_read(&nr_evict), atomic_long_read(&nr_chance),
		atomic_long_read(&nr_retry), atomic_long_read(&nr_lost),
		atomic_long_read(&nr_nomem), atomic_read(&cache_count));
	if (!ok)
		pr_err("the hash table and the list disagree\n");
	if (atomic_long_read(&nr_bad))
		pr_err("a lookup found an entry whose value is not its key's\n");
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

MODULE_DESCRIPTION("A cache on RCU pseudo-transactions, its writers under a lock per node");
MODULE_LICENSE("GPL");
