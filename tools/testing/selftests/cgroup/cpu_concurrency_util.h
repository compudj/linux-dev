/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Helpers shared by the tests of cpu.concurrency.max.
 */
#ifndef CPU_CONCURRENCY_UTIL_H
#define CPU_CONCURRENCY_UTIL_H

#define _GNU_SOURCE
#include <linux/auxvec.h>
#include <linux/limits.h>
#include <linux/sched.h>
#include <linux/vhost.h>
#include <sys/auxv.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/prctl.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <linux/sched/types.h>
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kselftest.h"
#include "cgroup_util.h"

#define MAX_THREADS	64

#define RSEQ_MM_CID_OFFSET	offsetof(struct rseq, mm_cid)

struct spinner {
	pthread_t thread;
	struct timespec until;
	unsigned long cpus_seen[(CPU_SETSIZE + 63) / 64];
	long long max_mm_cid;	/* the largest seen, -1 for none */
	int *go;		/* if set, spin until it reads 1, then sample */
	pid_t tid;
	int nap_us;		/* sleep that long after every sample */
	int first_nap_us;	/* and that long before the first one */
	unsigned long samples;	/* how many samples it took so far */
};

struct spin_param {
	int nthreads;
	int limit;
	long duration_ms;
};

/* What the kernel has reported so far, see kernel_state_record(). */
struct kernel_state {
	long taint;	/* the taint flags */
	int lockdep;	/* debug_locks, -1 without lockdep */
	int kmsg;	/* /dev/kmsg at the end of the log */
};

/* The values of cpu.concurrency.stat, see conc_stat_read(). */
struct conc_stat {
	long long nr_held;	/* tasks held now */
	long long nr_holds;	/* holds since the cgroup was created */
	long long held_usec;	/* total time tasks spent held */
	long long nr_rotations;	/* slots taken from running tasks */
	long long rotated_usec;	/* held time of the holds a rotation ended */
};

struct rseq *thread_rseq(void);
int rseq_has_mm_cid(void);
__u32 rseq_field(const struct rseq *r, size_t offset);
int skip(const char *fmt, ...);
int need_cpus(int n);
long read_sysctl(const char *name);
int ts_before(const struct timespec *a, const struct timespec *b);
void ts_add_ms(struct timespec *t, long ms);
long long ns_since(const struct timespec *from);
void spin_ns(long long ns);
void *spin(void *arg);
void stress_all_cpus(cpu_set_t *set);
int pin_cpus(pid_t pid, const int *cpus);
int count_cpus_seen(struct spinner *s, int n);
int spinner_only_on(const struct spinner *s, int cpu);
long long max_mm_cid_seen(struct spinner *s, int n);
int spin_and_check(const char *cgroup, void *arg);
void cpus_record_online(void);
int cpus_restore(void);
void kernel_state_record(struct kernel_state *ks);
int conc_stat_read(const char *cg, struct conc_stat *st);
int kernel_state_check(struct kernel_state *ks, const char *name);

#endif /* CPU_CONCURRENCY_UTIL_H */
