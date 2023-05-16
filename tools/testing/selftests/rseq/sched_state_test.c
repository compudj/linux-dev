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

static struct rseq_abi_sched_state *target_thread_state;

//TODO:
//Use rseq c.s. and rseq fence to protect access to remote thread's rseq_abi.

static
void show_sched_state(struct rseq_abi_sched_state *rseq_thread_state)
{
	uint32_t state;

	state = rseq_thread_state->state;
	printf("Target thread: %u, ON_CPU=%d\n",
	       rseq_thread_state->tid,
	       !!(state & RSEQ_ABI_SCHED_STATE_FLAG_ON_CPU));
}

static
void *test_thread(void *arg)
{
	int i;

	for (i = 0; i < 1000; i++) {
		show_sched_state(target_thread_state);
		(void) poll(NULL, 0, 100);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t test_thread_id;
	int i;

	if (rseq_register_current_thread()) {
		fprintf(stderr, "Error: rseq_register_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		goto init_thread_error;
	}
	target_thread_state = rseq_get_sched_state(rseq_get_abi());

	pthread_create(&test_thread_id, NULL, test_thread, NULL);

	for (i = 0; i < 1000000000; i++)
		rseq_barrier();
	//for (i = 0; i < 10000; i++)
	//	(void) poll(NULL, 0, 75);

	pthread_join(test_thread_id, NULL);

	if (rseq_unregister_current_thread()) {
		fprintf(stderr, "Error: rseq_unregister_current_thread(...) failed(%d): %s\n",
			errno, strerror(errno));
		goto init_thread_error;
	}
	return 0;

init_thread_error:
	return -1;
}
