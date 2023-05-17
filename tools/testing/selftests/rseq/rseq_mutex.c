// SPDX-License-Identifier: LGPL-2.1

#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <poll.h>

#include "rseq.h"

#define RSEQ_MUTEX_MAX_BUSY_LOOP	100

struct rseq_mutex {
	/*
	 * When non-NULL, owner points to per-thread rseq_abi_sched_state of
	 * owner thread.
	 */
	struct rseq_abi_sched_state *owner;
};

static struct rseq_mutex lock = { .owner = NULL };

static int testvar;

static void rseq_lock_slowpath(struct rseq_mutex *lock)
{
	int i = 0;

	for (;;) {
		struct rseq_abi_sched_state *expected = NULL, *self = rseq_get_sched_state(rseq_get_abi());

		if (__atomic_compare_exchange_n(&lock->owner, &expected, self, false,
						__ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			break;
		//TODO: use rseq critical section to protect dereference of owner thread's
		//rseq_abi_sched_state, combined with rseq fence at thread reclaim.
		if ((RSEQ_READ_ONCE(expected->state) & RSEQ_ABI_SCHED_STATE_FLAG_ON_CPU) &&
		    i < RSEQ_MUTEX_MAX_BUSY_LOOP) {
			rseq_barrier();			/* busy-wait, e.g. cpu_relax(). */
			i++;
		} else {
			//TODO: Enqueue waiter in a wait-queue, and integrate
			//with sys_futex rather than waiting for 10ms.
			(void) poll(NULL, 0, 10);	/* wait 10ms */
		}
	}
}

static void rseq_lock(struct rseq_mutex *lock)
{
	struct rseq_abi_sched_state *expected = NULL, *self = rseq_get_sched_state(rseq_get_abi());

	if (__atomic_compare_exchange_n(&lock->owner, &expected, self, false,
					__ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	rseq_lock_slowpath(lock);
}

static void rseq_unlock(struct rseq_mutex *lock)
{
	__atomic_store_n(&lock->owner, NULL, __ATOMIC_RELEASE);
	//TODO: integrate with sys_futex and wakeup oldest waiter.
}

static
void *test_thread(void *arg)
{
	int i;

	if (rseq_register_current_thread()) {
		fprintf(stderr, "Error: rseq_register_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		abort();
	}

	for (i = 0; i < 1000; i++) {
		int var;

		rseq_lock(&lock);
		var = RSEQ_READ_ONCE(testvar);
		if (var) {
			fprintf(stderr, "Unexpected value %d\n", var);
			abort();
		}
		RSEQ_WRITE_ONCE(testvar, 1);
		if (!(i % 10))
			(void) poll(NULL, 0, 10);
		else
			rseq_barrier();
		RSEQ_WRITE_ONCE(testvar, 0);
		rseq_unlock(&lock);
	}

	if (rseq_unregister_current_thread()) {
		fprintf(stderr, "Error: rseq_unregister_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		abort();
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int nr_threads = 5;
	pthread_t test_thread_id[nr_threads];
	int i;

	for (i = 0; i < nr_threads; i++) {
		pthread_create(&test_thread_id[i], NULL, test_thread, NULL);
	}

	for (i = 0; i < nr_threads; i++) {
		pthread_join(test_thread_id[i], NULL);
	}

	return 0;
}
