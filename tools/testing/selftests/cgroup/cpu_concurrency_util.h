/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared by the tests of cpu.concurrency.max, test_cpu_concurrency.c, and
 * their stress variants, test_cpu_concurrency_stress.c.
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

/* Offsets of fields in struct rseq, read through rseq_field(). */
#define RSEQ_CPU_ID_OFFSET	offsetof(struct rseq, cpu_id)
#define RSEQ_MM_CID_OFFSET	offsetof(struct rseq, mm_cid)
#define RSEQ_NS_CID_OFFSET	offsetof(struct rseq, ns_cid)

struct spinner {
	pthread_t thread;
	pid_t tid;
	struct timespec until;
	unsigned long cpus_seen[(CPU_SETSIZE + 63) / 64];
	long long max_mm_cid;	/* the largest seen, -1 for none */
	int nap_us;		/* sleep that long after every sample: a sleeper */
	int first_nap_us;	/* and that long before the first one */
	int *go;		/* when set, spin without sampling until it reads 1 */
};

struct spin_param {
	int nthreads;
	int limit;
	long duration_ms;
};

struct rseq *thread_rseq(void);
int rseq_has_mm_cid(void);
__u32 rseq_field(const struct rseq *r, size_t offset);
struct rseq *rseq_ext_register(size_t *lenp);
void rseq_ext_unregister(struct rseq *area, size_t len);
int skip(const char *fmt, ...);
int need_cpus(int n);
long read_sysctl(const char *name);
int ts_before(const struct timespec *a, const struct timespec *b);
void ts_add_ms(struct timespec *t, long ms);
long long ns_since(const struct timespec *from);
long us_since(const struct timespec *from);
void spin_ns(long long ns);
void *spin(void *arg);
void stress_all_cpus(cpu_set_t *set);
int pin_cpus(pid_t pid, const int *cpus);
int count_cpus_seen(struct spinner *s, int n);
long long max_mm_cid_seen(struct spinner *s, int n);
int spin_and_check(const char *cgroup, void *arg);
int write_cpu_online(int cpu, int online);

#endif /* CPU_CONCURRENCY_UTIL_H */
