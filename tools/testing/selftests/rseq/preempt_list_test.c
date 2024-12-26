// SPDX-License-Identifier: LGPL-2.1
/*
 * Preempt list test.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <poll.h>
#include <inttypes.h>

#include "rseq.h"

static __thread struct rseq_reset_area reset_area;
static __thread struct rseq_reset_area reset_area2;

void test_preempt_list(void)
{
	int canary = 0x42;
	uint64_t canary2 = 0x43;
	int update_val = 0x1;

	rseq_reset_area_init(&reset_area, &canary, &update_val, sizeof(canary));
	rseq_reset_area_init(&reset_area2, &canary2, NULL, sizeof(canary2));

	printf("canary before: %d %" PRIu64 "\n", canary, canary2);

	printf("Add rseq area to head of linked list.\n");
	rseq_reset_area_register(&reset_area);

	printf("Add rseq area2 to head of linked list.\n");
	rseq_reset_area_register(&reset_area2);

	poll(NULL, 0, 10);
	printf("canary after: %d %" PRIu64 "\n", canary, canary2);

	printf("Remove rseq area2 from linked list.\n");
	rseq_reset_area_unregister(&reset_area2);

	canary = 0x42;
	canary2 = 0x43;
	printf("canary before: %d %" PRIu64 "\n", canary, canary2);
	poll(NULL, 0, 10);
	printf("canary after: %d %" PRIu64 "\n", canary, canary2);

	printf("Add rseq area2 to head of linked list.\n");
	rseq_reset_area_register(&reset_area2);

	canary = 0x42;
	canary2 = 0x43;
	printf("canary before: %d %" PRIu64 "\n", canary, canary2);
	poll(NULL, 0, 10);
	printf("canary after: %d %" PRIu64 "\n", canary, canary2);

	printf("Remove rseq area from linked list.\n");
	rseq_reset_area_unregister(&reset_area);

	canary = 0x42;
	canary2 = 0x43;
	printf("canary before: %d %" PRIu64 "\n", canary, canary2);
	poll(NULL, 0, 10);
	printf("canary after: %d %" PRIu64 "\n", canary, canary2);

	printf("Remove rseq area2 from linked list.\n");
	rseq_reset_area_unregister(&reset_area2);

	canary = 0x42;
	canary2 = 0x43;
	printf("canary before: %d %" PRIu64 "\n", canary, canary2);
	poll(NULL, 0, 10);
	printf("canary after: %d %" PRIu64 "\n", canary, canary2);
}

int main(int argc, char **argv)
{
	if (rseq_register_current_thread()) {
		fprintf(stderr, "Error: rseq_register_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		goto init_thread_error;
	}
	printf("testing preempt list\n");
	test_preempt_list();
	if (rseq_unregister_current_thread()) {
		fprintf(stderr, "Error: rseq_unregister_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		goto init_thread_error;
	}
	return 0;

init_thread_error:
	return -1;
}
