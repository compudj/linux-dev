// SPDX-License-Identifier: LGPL-2.1
/*
 * Basic rseq NUMA test. Validate that (mm_cid, numa_node_id) pairs are
 * invariant when the number of threads >= number of allowed CPUs, as
 * long as those preconditions are respected:
 *
 *   - A process has a number of threads >= number of allowed CPUs,
 *   - The allowed CPUs mask is unchanged, and
 *   - The NUMA configuration is unchanged.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/time.h>

#include "rseq.h"

#define NR_LOOPS	100

static int nr_threads, nr_active_threads, test_go, test_stop;

#ifdef RSEQ_ARCH_HAS_LOAD_U32_U32

static int cpu_numa_id[CPU_SETSIZE];

static int get_affinity_weight(void)
{
	cpu_set_t allowed_cpus;

	if (sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus)) {
		perror("sched_getaffinity");
		abort();
	}
	return CPU_COUNT(&allowed_cpus);
}

static void numa_id_init(void)
{
	int i;

	for (i = 0; i < CPU_SETSIZE; i++)
		cpu_numa_id[i] = -1;
}

static void *test_thread(void *arg)
{
	int i;

	if (rseq_register_current_thread()) {
		fprintf(stderr, "Error: rseq_register_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		abort();
	}
	/*
	 * Rendez-vous across all threads to make sure the number of
	 * threads >= number of possible CPUs for the entire test duration.
	 */
	if (__atomic_add_fetch(&nr_active_threads, 1, __ATOMIC_RELAXED) == nr_threads)
		__atomic_store_n(&test_go, 1, __ATOMIC_RELAXED);
	while (!__atomic_load_n(&test_go, __ATOMIC_RELAXED))
		rseq_barrier();

	for (i = 0; i < NR_LOOPS; i++) {
		uint32_t mm_cid, node;
		int cached_node_id;

		while (rseq_load_u32_u32(RSEQ_MO_RELAXED, &mm_cid,
					 &rseq_get_abi()->mm_cid,
					 &node, &rseq_get_abi()->node_id) != 0) {
			/* Retry. */
		}
		cached_node_id = RSEQ_READ_ONCE(cpu_numa_id[mm_cid]);
		if (cached_node_id == -1) {
			RSEQ_WRITE_ONCE(cpu_numa_id[mm_cid], node);
		} else {
			if (node != cached_node_id) {
				fprintf(stderr, "Error: NUMA node id discrepancy: mm_cid %u cached node id %d node id %u.\n",
					mm_cid, cached_node_id, node);
				fprintf(stderr, "This is likely a kernel bug, or caused by a concurrent NUMA topology reconfiguration.\n");
				abort();
			}
		}
		(void) poll(NULL, 0, 10);	/* wait 10ms */
	}
	/*
	 * Rendez-vous before exiting all threads to make sure the
	 * number of threads >= number of possible CPUs for the entire
	 * test duration.
	 */
	if (__atomic_sub_fetch(&nr_active_threads, 1, __ATOMIC_RELAXED) == 0)
		__atomic_store_n(&test_stop, 1, __ATOMIC_RELAXED);
	while (!__atomic_load_n(&test_stop, __ATOMIC_RELAXED))
		rseq_barrier();

	if (rseq_unregister_current_thread()) {
		fprintf(stderr, "Error: rseq_unregister_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		abort();
	}
	return NULL;
}

static int test_numa(void)
{
	pthread_t tid[nr_threads];
	int err, i;
	void *tret;

	numa_id_init();

	printf("testing rseq (mm_cid, numa_node_id) invariant, multi-threaded (%d threads)\n",
	       nr_threads);

	for (i = 0; i < nr_threads; i++) {
		err = pthread_create(&tid[i], NULL, test_thread, NULL);
		if (err != 0)
			abort();
	}

	for (i = 0; i < nr_threads; i++) {
		err = pthread_join(tid[i], &tret);
		if (err != 0)
			abort();
	}

	return 0;
}
#else
static int test_numa(void)
{
	fprintf(stderr, "rseq_load_u32_u32 is not implemented on this architecture. Skipping numa test.\n");
	return 0;
}
#endif

int main(int argc, char **argv)
{
	nr_threads = get_affinity_weight();
	return test_numa();
}
