// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * A cache on RCU pseudo-transactions, its writers under a lock per node
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * The same cache as rcu_txn_cache_mutex.c, with writers that exclude each other
 * per node rather than under one mutex, so that writers updating distinct
 * entries run in parallel.  Every entry, every bucket and the head of the list
 * has a spinlock, and a writer holds the lock of every node whose links its
 * transaction changes.  An entry's links are its next and pprev in its bucket
 * and its next and prev in the list; a bucket's is its first entry; the head's
 * are the first and the last entry of the list.
 *
 * A writer plans under RCU, with no lock held: it finds the entry it works on
 * by walking its bucket, which finds the associated node, and reads the other
 * nodes around it from its links.  It then takes the surrounding nodes' locks
 * in the order of the locks' addresses, so that two writers cannot deadlock,
 * and validates the plan: no planned node is dead, and the planned links are in
 * place, in the entry and in its neighbours.  If not, the writer releases the
 * locks and plans again.
 *
 * The dead flag is an entry's tombstone, set with its lock held by the writer
 * that unlinked it.  An unlinked entry keeps its links, for the readers
 * standing on it, so its neighbours' links alone are not enough to determine
 * that it is gone.
 *
 * The operation is one RCU read-side critical section from the plan to the
 * commit, which keeps every node it found allocated.  lockdep is told the
 * order of the locks: they are one class, with a comparison function.
 *
 * Each operation is one transaction over the hash table and the list, recorded
 * and committed with the locks held, its records reserved before.  An
 * insertion links the new entry into its bucket and at the front of the list;
 * an update replaces the entry in both; a deletion or an eviction unlinks one
 * from both; a second chance moves a referenced entry from the tail of the
 * list to its front.  Lookup threads find entries under RCU alone, and mark
 * the ones they find referenced.
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
#include <linux/random.h>
#include <linux/rcu_txn.h>
#include <linux/rcu_txn_hlist.h>
#include <linux/rcu_txn_list.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>

#define CACHE_HASH_BITS	6
#define CACHE_KEYS	512	/* keys are 0 to CACHE_KEYS - 1 */
#define CACHE_SIZE	128	/* entries held, about */
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
	spinlock_t lock;			/* its links, and @dead */
	unsigned long key;
	unsigned long value;			/* key + CACHE_KEYS * version */
	struct rcu_txn_hlist_node hnode;	/* in its bucket */
	struct rcu_txn_list_node lru;		/* in the list, newest first */
	struct rcu_head rcu;
	bool dead;				/* the tombstone: unlinked */
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

/* The entry of a node of the list, or NULL for the head. */
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

/* The first entry of a bucket, with the bucket's lock held. */
static struct rcu_txn_hlist_node *bucket_first(struct cache_bucket *b)
{
	return rcu_txn_hlist_first_check(&b->head, lockdep_is_held(&b->lock));
}

/* The first node of the list, with the head's lock held. */
static struct rcu_txn_list_node *lru_first(void)
{
	return rcu_txn_list_next_check(&cache_lru.head.node,
				       lockdep_is_held(&cache_lru.lock));
}

/* Within an RCU read-side critical section. */
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
	struct rcu_txn_hlist_node __rcu **pprev; /* the slot pointing to @e, */
	struct cache_entry *pprev_entry;	/* an entry's, or NULL */
	spinlock_t *pprev_lock;			/* and its lock */
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
 * Find the entry of @key by walking its bucket, which gives the slot that
 * points to it there: the bucket's first, or the next of the entry before it.
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
	CACHE_UNLINK,	/* the bucket: a deletion, an update */
	CACHE_FRONT,	/* the front of the list: a second chance */
};

/* Is @e, a planned entry or NULL for a bucket or the head, unlinked? */
static bool cache_dead(const struct cache_entry *e)
{
	return e && e->dead;
}

/* A slot of a bucket, with the lock of its owner held. */
static struct rcu_txn_hlist_node *
hlist_link(struct rcu_txn_hlist_node __rcu **slot, spinlock_t *lock)
{
	return rcu_txn_dereference_check(*slot, RCU_TXN_HLIST_TAG,
					 lockdep_is_held(lock));
}

/* A link of the list, with the lock of its node held. */
static struct rcu_txn_list_node *
lru_link(struct rcu_txn_list_node *node, struct rcu_txn_list_node __rcu **link)
{
	return rcu_txn_dereference_check(*link, RCU_TXN_LIST_TAG,
					 lockdep_is_held(lru_lock(node)));
}

/*
 * With the locks held: no planned node is dead, and the planned links are in
 * place, as @p->e and as its neighbours see them.
 */
static bool cache_plan_holds(const struct cache_plan *p, enum cache_mode mode)
{
	struct cache_entry *e = p->e;

	if (e->dead || cache_dead(lru_entry(p->prev)) ||
	    cache_dead(lru_entry(p->next)))
		return false;
	if (lru_link(&e->lru, &e->lru.prev) != p->prev ||
	    lru_link(&e->lru, &e->lru.next) != p->next ||
	    lru_link(p->prev, &p->prev->next) != &e->lru ||
	    lru_link(p->next, &p->next->prev) != &e->lru)
		return false;

	if (mode == CACHE_UNLINK) {
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
 * Plan an operation of @mode on the entry of @key, take the locks of the nodes
 * whose links it changes, and look again.  Returns false, holding no lock, if
 * there is no such entry, or if it is not @want when that is given.  Within an
 * RCU read-side critical section, which keeps what it finds allocated.
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
		p->prev = rcu_txn_list_prev_rcu(&p->e->lru);
		p->next = rcu_txn_list_next_rcu(&p->e->lru);
		p->first = rcu_txn_list_next_rcu(head);

		cache_want(p, &p->e->lock);
		if (mode == CACHE_UNLINK) {
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
		dead = p->e->dead;
		cache_unlock(p);
		if (dead && want)
			return false;
		atomic_long_inc(&nr_retry);
	}
}

/*
 * Unlink the entry of @key from its bucket and from the list, or replace it
 * with @new in both, with @txn, which has four records reserved and which this
 * consumes.  When @want is given, only if it is that entry.  Returns false if
 * there is no such entry, or if the transaction could not be allocated.
 * Within an RCU read-side critical section, which keeps @want allocated.
 */
static bool __cache_unlink(struct rcu_txn *txn, unsigned long key,
			   struct cache_entry *want, struct cache_entry *new)
{
	struct cache_plan p;
	bool done = false;

	if (!cache_lock_entry(&p, key, want, CACHE_UNLINK)) {
		rcu_txn_cancel(txn);
		return false;
	}
	if (new) {
		rcu_txn_hlist_replace_prepare(txn, &p.e->hnode, &new->hnode);
		rcu_txn_list_replace_prepare(txn, &p.e->lru, &new->lru);
	} else {
		rcu_txn_hlist_del_prepare(txn, &p.e->hnode);
		rcu_txn_list_del_prepare(txn, &p.e->lru);
	}
	if (!rcu_txn_commit(txn)) {
		p.e->dead = true;
		done = true;
	}
	cache_unlock(&p);
	if (done) {
		kfree_rcu(p.e, rcu);
		if (!new)
			atomic_dec(&cache_count);
	} else {
		cache_fail();
	}
	return done;
}

/* Unlink the entry of @key, or replace it with @new: see __cache_unlink(). */
static bool cache_unlink(unsigned long key, struct cache_entry *new)
{
	struct rcu_txn txn;
	bool done;

	/* Before RCU and the locks, as it may sleep. */
	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_reserve(&txn, 4);

	rcu_read_lock();
	done = __cache_unlink(&txn, key, NULL, new);
	rcu_read_unlock();
	return done;
}

/*
 * Move @e to the front of the list, with @txn, which has six records reserved
 * and which this consumes.  Within an RCU read-side critical section, which
 * keeps @e allocated.
 */
static void cache_move_front(struct rcu_txn *txn, struct cache_entry *e)
{
	struct cache_plan p;

	if (!cache_lock_entry(&p, e->key, e, CACHE_FRONT)) {
		rcu_txn_cancel(txn);
		return;
	}
	/* Nothing is recorded if it is there already. */
	rcu_txn_list_move_after_prepare(txn, &e->lru, &cache_lru.head.node);
	if (rcu_txn_commit(txn))
		cache_fail();
	cache_unlock(&p);
}

/*
 * Evict the oldest entry that no lookup found since it last moved, after giving
 * the referenced entries at the tail a second chance at the front.
 */
static void cache_evict(void)
{
	unsigned int chances = 0;

	for (;;) {
		bool evicted = false;
		struct cache_entry *e;
		struct rcu_txn txn;

		/* Before RCU, for a move (six records) or an unlink (four). */
		rcu_txn_init(&txn, GFP_KERNEL);
		rcu_txn_reserve(&txn, 6);

		/* One critical section, which keeps @e allocated throughout. */
		rcu_read_lock();
		e = lru_entry(rcu_txn_list_prev_rcu(&cache_lru.head.node));
		if (!e) {
			rcu_read_unlock();
			rcu_txn_cancel(&txn);
			break;
		}
		if (chances < CACHE_CHANCES && READ_ONCE(e->referenced)) {
			WRITE_ONCE(e->referenced, false);
			cache_move_front(&txn, e);
			chances++;
		} else {
			evicted = __cache_unlink(&txn, e->key, e, NULL);
		}
		rcu_read_unlock();
		if (evicted) {
			atomic_long_inc(&nr_evict);
			break;
		}
		if (READ_ONCE(cache_failed))
			break;
		/* Another writer unlinked it first: try the new tail. */
	}
	atomic_long_add(chances, &nr_chance);
}

/*
 * Link @new into its bucket and at the front of the list.  Returns false if
 * its key is in the cache already, or if the transaction could not be
 * allocated.
 */
static bool cache_link(struct cache_entry *new)
{
	struct cache_bucket *b = cache_bucket(new->key);
	struct rcu_txn_list_node *head = &cache_lru.head.node;
	struct cache_plan p;
	struct rcu_txn txn;
	bool there;

	rcu_txn_init(&txn, GFP_KERNEL);
	rcu_txn_reserve(&txn, 4);

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
		rcu_txn_hlist_add_head_prepare(&txn, &new->hnode, &b->head);
		rcu_txn_list_add_after_prepare(&txn, &new->lru, head);
	}
	if (rcu_txn_commit(&txn)) {
		cache_fail();
		there = true;
	} else if (!there) {
		atomic_inc(&cache_count);
	}
	cache_unlock(&p);
	rcu_read_unlock();
	return !there;
}

/* Insert @key, or update it, at version @version. */
static void cache_insert(unsigned long key, unsigned long version)
{
	struct cache_entry *new;
	bool old;

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
		if (cache_unlink(key, new)) {
			atomic_long_inc(&nr_update);
			return;
		}
	} else {
		if (atomic_read(&cache_count) >= CACHE_SIZE)
			cache_evict();
		if (cache_link(new)) {
			atomic_long_inc(&nr_insert);
			return;
		}
	}
	atomic_long_inc(&nr_lost);		/* another writer was first */
	kfree(new);
}

static void cache_delete(unsigned long key)
{
	if (cache_unlink(key, NULL))
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
 * With the threads stopped: every entry of the hash table is in its bucket and
 * in the list, both ways, and there are cache_count of them.
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
 * With the threads stopped, no reader or writer is left: free every entry
 * without unlinking it, which allocates nothing and so cannot fail.
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

static int __init cache_init(void)
{
	struct task_struct *threads[2 * CACHE_THREADS];
	unsigned int i, n = 0;
	bool ok;

	for (i = 0; i < ARRAY_SIZE(cache_hash); i++) {
		cache_lock_init(&cache_hash[i].lock);
		rcu_txn_hlist_init(&cache_hash[i].head);
	}
	cache_lock_init(&cache_lru.lock);

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
	pr_info("%ld lookups, %ld hits, %ld bad; %ld insertions, %ld updates, %ld deletions, %ld evictions, %ld second chances, %ld retries, %ld lost races, %ld allocation failures; %d entries\n",
		atomic_long_read(&nr_lookup), atomic_long_read(&nr_hit),
		atomic_long_read(&nr_bad), atomic_long_read(&nr_insert),
		atomic_long_read(&nr_update), atomic_long_read(&nr_delete),
		atomic_long_read(&nr_evict), atomic_long_read(&nr_chance),
		atomic_long_read(&nr_retry), atomic_long_read(&nr_lost),
		atomic_long_read(&nr_nomem), atomic_read(&cache_count));
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

MODULE_DESCRIPTION("A cache on RCU pseudo-transactions, its writers under a lock per node");
MODULE_LICENSE("GPL");
