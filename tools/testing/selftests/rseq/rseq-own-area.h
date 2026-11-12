/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * Helpers for tests that register their own rseq area with the rseq(2)
 * system call, without the registration of the C library.
 */
#ifndef RSEQ_OWN_AREA_H
#define RSEQ_OWN_AREA_H

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rseq.h"

#ifndef AT_RSEQ_FEATURE_SIZE
# define AT_RSEQ_FEATURE_SIZE	27
#endif
#ifndef AT_RSEQ_ALIGN
# define AT_RSEQ_ALIGN		28
#endif

/* The size and the alignment of the original struct rseq. */
#define RSEQ_ORIG_SIZE		32

#define RSEQ_NO_LIBC_TUNABLE	"glibc.pthread.rseq=0"

/*
 * The V2 mode needs the generic interrupt entry code. On the other
 * architectures every registration is a legacy one, whatever its size.
 * The ABI has no field that tells which mode a registration got.
 */
#if defined(__arm__) || defined(__mips__) || defined(__or1k__)
# define RSEQ_ARCH_HAS_V2	0
#else
# define RSEQ_ARCH_HAS_V2	1
#endif

#define RSEQ_DEBUGFS_DEBUG	"/sys/kernel/debug/rseq/debug"

/*
 * Return 1 if the debug mode of rseq is on, 0 if it is off, and -1 if
 * debugfs does not tell. The debug mode adds checks that kill.
 */
static inline int rseq_debug_mode(void)
{
	FILE *f = fopen(RSEQ_DEBUGFS_DEBUG, "r");
	int on = -1;

	if (!f)
		return -1;
	if (fscanf(f, "%d", &on) != 1)
		on = -1;
	fclose(f);
	return on;
}

/* Call rseq(2). Return 0, or a negative error number. */
static inline int sys_rseq(void *area, uint32_t len, int flags, uint32_t sig)
{
	return syscall(__NR_rseq, area, len, flags, sig) ? -errno : 0;
}

/*
 * Run the test again without the rseq registration of glibc. A thread can
 * register one area, and glibc 2.35 and later register theirs at startup.
 * Call this first in main().
 */
static inline void rseq_reexec_without_libc(char **argv)
{
	const char *old = getenv("GLIBC_TUNABLES");
	char *tunables;

	if (old && strstr(old, RSEQ_NO_LIBC_TUNABLE))
		return;
	if (asprintf(&tunables, "%s%s%s", old ? old : "", old ? ":" : "",
		     RSEQ_NO_LIBC_TUNABLE) < 0)
		return;
	if (setenv("GLIBC_TUNABLES", tunables, 1))
		return;
	execv("/proc/self/exe", argv);
	/* Without /proc the tests skip: glibc owns the registration. */
}

/* Return the size of an optimized (V2) registration, or 0 if none. */
static inline uint32_t rseq_v2_size(void)
{
	unsigned long size = getauxval(AT_RSEQ_FEATURE_SIZE);

	return size > RSEQ_ORIG_SIZE ? size : 0;
}

/* Return the alignment an area of @len bytes needs. */
static inline unsigned long rseq_area_align(uint32_t len)
{
	unsigned long align = getauxval(AT_RSEQ_ALIGN);

	if (len == RSEQ_ORIG_SIZE || !align)
		return RSEQ_ORIG_SIZE;
	return align;
}

/* Pin the calling thread to @cpu. Return 0, or -1. */
static inline int rseq_pin(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

/*
 * Move the calling thread to another allowed CPU and back. The kernel
 * then writes the IDs to the rseq area. Return 0, or -1 with one CPU.
 */
static inline int rseq_migrate_and_back(void)
{
	cpu_set_t allowed;
	int cpu, here = sched_getcpu(), ret = -1;

	if (here < 0 || sched_getaffinity(0, sizeof(allowed), &allowed))
		return -1;
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (cpu == here || !CPU_ISSET(cpu, &allowed))
			continue;
		if (!rseq_pin(cpu) && !rseq_pin(here))
			ret = 0;
		break;
	}
	if (sched_setaffinity(0, sizeof(allowed), &allowed))
		return -1;
	return ret;
}

/*
 * Run @fn(@arg) in a child process. Return its exit status, or the
 * negative number of the signal that killed it.
 */
static inline int rseq_run_child(int (*fn)(void *), void *arg)
{
	int status;
	pid_t pid;

	fflush(NULL);
	pid = fork();
	if (pid < 0)
		return 255;
	if (!pid)
		_exit(fn(arg));
	if (waitpid(pid, &status, 0) != pid)
		return 255;
	if (WIFSIGNALED(status))
		return -WTERMSIG(status);
	return WEXITSTATUS(status);
}

#endif /* RSEQ_OWN_AREA_H */
