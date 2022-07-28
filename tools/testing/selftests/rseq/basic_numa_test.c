// SPDX-License-Identifier: LGPL-2.1
/*
 * Basic rseq NUMA test. Validate that (vm_vcpu_id, numa_node_id) pairs are
 * invariant. The only known scenario where this is untrue is on Power which
 * can reconfigure the NUMA topology on CPU hotunplug/hotplug sequence.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "rseq.h"

#define NR_LOOPS	100000000
#define NR_THREADS	16

#ifdef RSEQ_ARCH_HAS_LOAD_U32_U32

static
int cpu_numa_id[CPU_SETSIZE];

static
void numa_id_init(void)
{
	int i;

	for (i = 0; i < CPU_SETSIZE; i++)
		cpu_numa_id[i] = -1;
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

	for (i = 0; i < NR_LOOPS; i++) {
		uint32_t vm_vcpu_id, node;
		int cached_node_id;

		while (rseq_load_u32_u32(RSEQ_MO_RELAXED, &vm_vcpu_id, &rseq_get_abi()->vm_vcpu_id,
					 &node, &rseq_get_abi()->node_id) != 0) {
			/* Retry. */
		}
		cached_node_id = RSEQ_READ_ONCE(cpu_numa_id[vm_vcpu_id]);
		if (cached_node_id == -1) {
			RSEQ_WRITE_ONCE(cpu_numa_id[vm_vcpu_id], node);
		} else {
			if (node != cached_node_id) {
				fprintf(stderr, "Error: NUMA node id discrepancy: vm_vcpu_id %u cached node id %d node id %u.\n",
					vm_vcpu_id, cached_node_id, node);
				fprintf(stderr, "This is likely a kernel bug, or caused by a concurrent NUMA topology reconfiguration.\n");
				abort();
			}
		}
	}

	if (rseq_unregister_current_thread()) {
		fprintf(stderr, "Error: rseq_unregister_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		abort();
	}
	return NULL;
}

static
int test_numa(void)
{
        pthread_t tid[NR_THREADS];
        int err, i;
        void *tret;

	numa_id_init();

	printf("testing rseq (vm_vcpu_id, numa_node_id) invariant, single thread\n");

	(void) test_thread(NULL);

	printf("testing rseq (vm_vcpu_id, numa_node_id) invariant, multi-threaded\n");

        for (i = 0; i < NR_THREADS; i++) {
                err = pthread_create(&tid[i], NULL, test_thread, NULL);
                if (err != 0)
                        abort();
        }

        for (i = 0; i < NR_THREADS; i++) {
                err = pthread_join(tid[i], &tret);
                if (err != 0)
                        abort();
        }

	return 0;
}
#else
static
int test_numa(void)
{
	fprintf(stderr, "rseq_load_u32_u32 is not implemented on this architecture. "
			"Skipping numa test.\n");
	return 0;
}
#endif

int main(int argc, char **argv)
{
	return test_numa();
}
