// SPDX-License-Identifier: LGPL-2.1
/*
 * Basic test for SKSM.
 */

#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#ifndef MADV_MERGE
#define MADV_MERGE	26
#endif

#define PAGE_SIZE	4096

#define WRITE_ONCE(x, val) ((*(volatile typeof(x) *) &(x)) = (val))

static int opt_stop_at = 0, opt_pause = 0;

struct test_page {
	char array[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
};

struct test_page2 {
	char array[2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
};

/* identical to zero page. */
static struct test_page zero;

/* a1 and a2 are identical. */
static struct test_page a1 = {
	.array[0] = 0x42,
	.array[1] = 0x42,
};

static struct test_page a2 = {
	.array[0] = 0x42,
	.array[1] = 0x42,
};

/* b1 and b2 are identical. */
static struct test_page2 b1 = {
	.array[0] = 0x43,
	.array[1] = 0x43,
	.array[PAGE_SIZE] = 0x44,
	.array[PAGE_SIZE + 1] = 0x44,
};

static struct test_page2 b2 = {
	.array[0] = 0x43,
	.array[1] = 0x43,
	.array[PAGE_SIZE] = 0x44,
	.array[PAGE_SIZE + 1] = 0x44,
};

static void touch_pages(void *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i += PAGE_SIZE)
		WRITE_ONCE(((char *)p)[i], ((char *)p)[i]);
}

static void test_step(char step)
{
	printf("\nTest step: <%c>\n", step);
	if (opt_pause) {
		printf("Press ENTER to continue...\n");
		getchar();
	}
	if (opt_stop_at == step) {
		poll(NULL, 0, -1);
		exit(0);
	}
}

static void show_usage(int argc, char **argv)
{
	printf("Usage : %s <OPTIONS>\n",
		argv[0]);
	printf("OPTIONS:\n");
	printf("	[-s stop_at] Stop test at step A, B, C, D, E, or F and wait forever.\n");
	printf("	[-p] Pause test between steps (await newline from the console).\n");
	printf("	[-h] Show this help.\n");
	printf("\n");
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-')
			continue;
		switch (argv[i][1]) {
		case 's':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			opt_stop_at = *argv[i + 1];
			switch (opt_stop_at) {
			case 'A':
			case 'B':
			case 'C':
			case 'D':
			case 'E':
			case 'F':
				break;
			default:
				show_usage(argc, argv);
				return -1;
			}
			i++;
			break;
		case 'p':
			opt_pause = 1;
			i++;
			break;
		case 'h':
			show_usage(argc, argv);
			return 0;
		default:
			show_usage(argc, argv);
			return -1;
		}
	}


	printf("PID: %d\n", getpid());
	printf("Shared mapping (write-protected)\n");

	test_step('A');

	printf("madvise MADV_MERGE a1\n");
	if (madvise(&a1, sizeof(a1), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE a2\n");
	if (madvise(&a2, sizeof(a2), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE b1\n");
	if (madvise(&b1, sizeof(b1), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE b2\n");
	if (madvise(&b2, sizeof(b2), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE zero\n");
	if (madvise(&zero, sizeof(zero), MADV_MERGE))
		goto error;

	test_step('B');

	printf("Trigger COW\n");
	touch_pages(&zero, sizeof(zero));
	touch_pages(&a1, sizeof(a1));
	touch_pages(&a2, sizeof(a2));
	touch_pages(&b1, sizeof(b1));
	touch_pages(&b2, sizeof(b2));

	test_step('C');

	printf("madvise MADV_MERGE a1\n");
	if (madvise(&a1, sizeof(a1), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE a2\n");
	if (madvise(&a2, sizeof(a2), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE b1\n");
	if (madvise(&b1, sizeof(b1), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE b2\n");
	if (madvise(&b2, sizeof(b2), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE zero\n");
	if (madvise(&zero, sizeof(zero), MADV_MERGE))
		goto error;

	test_step('D');

	printf("Trigger COW\n");
	touch_pages(&zero, sizeof(zero));
	touch_pages(&a1, sizeof(a1));
	touch_pages(&a2, sizeof(a2));
	touch_pages(&b1, sizeof(b1));
	touch_pages(&b2, sizeof(b2));

	test_step('E');

	printf("madvise MADV_MERGE a1\n");
	if (madvise(&a1, sizeof(a1), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE a2\n");
	if (madvise(&a2, sizeof(a2), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE b1\n");
	if (madvise(&b1, sizeof(b1), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE b2\n");
	if (madvise(&b2, sizeof(b2), MADV_MERGE))
		goto error;
	printf("madvise MADV_MERGE zero\n");
	if (madvise(&zero, sizeof(zero), MADV_MERGE))
		goto error;

	test_step('F');

	return 0;

error:
	perror("madvise");
	return -1;
}
