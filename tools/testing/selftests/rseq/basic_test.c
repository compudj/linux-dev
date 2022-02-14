// SPDX-License-Identifier: LGPL-2.1
/*
 * Basic test coverage for critical regions and rseq_current_cpu().
 */

#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "rseq.h"

#define NR_LOOPS	10

int cpu_numa_id[CPU_SETSIZE];

void test_cpu_pointer_iter(int iter)
{
	cpu_set_t affinity, test_affinity;
	int i;

	sched_getaffinity(0, sizeof(affinity), &affinity);
	CPU_ZERO(&test_affinity);
	for (i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &affinity)) {
			int node, vcpu_id;

			CPU_SET(i, &test_affinity);
			sched_setaffinity(0, sizeof(test_affinity),
					&test_affinity);
			vcpu_id = rseq_current_cpu_raw();
			node = rseq_fallback_current_node();
			assert(rseq_current_node() == node);
			assert(rseq_current_node_raw() == node);
			printf("vcpu=%d node=%d\n", vcpu_id, node);
			if (iter == 0)
				cpu_numa_id[vcpu_id] = node;
			else
				assert(cpu_numa_id[vcpu_id] == node);
			CPU_CLR(i, &test_affinity);
		}
	}
	sched_setaffinity(0, sizeof(affinity), &affinity);
}

void test_cpu_pointer(void)
{
	int i;

	for (i = 0; i < NR_LOOPS; i++)
		test_cpu_pointer_iter(i);
}

int main(int argc, char **argv)
{
	if (rseq_register_current_thread()) {
		fprintf(stderr, "Error: rseq_register_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		goto init_thread_error;
	}
	printf("testing current cpu\n");
	test_cpu_pointer();
	if (rseq_unregister_current_thread()) {
		fprintf(stderr, "Error: rseq_unregister_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		goto init_thread_error;
	}
	return 0;

init_thread_error:
	return -1;
}
