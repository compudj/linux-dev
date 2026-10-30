// SPDX-License-Identifier: GPL-2.0
#include "cpu_concurrency_util.h"
/*
 * Stress tests for cpu.concurrency.max. They cover the limit and the IDs
 * under it, with CPU hotplug, cpuset writes and a concurrency ID
 * namespace. Each variant runs for a few seconds. They are in a binary
 * of their own, so that the tests of test_cpu_concurrency.c still fit
 * the kselftest timeout.
 */

/*
 * The process runs in a or b, two children of a limited cgroup.
 * STRESS_SPINNERS threads sample their mm_cid in a loop. Half of them
 * spin. The other half sleep briefly every 300 samples. So the test
 * covers a slot released at sleep, a claim at wakeup and the removal of
 * idle slots, as well as preemption. One more thread creates and reaps
 * short-lived threads. Another reads the effective value in a loop.
 *
 * The main thread runs STRESS_EPOCHS epochs. In each one it:
 *  - unpins the spinners and writes a new limit to the parent, and the
 *    same limit or "max" to a and b;
 *  - moves the process between a and b every STRESS_SUBROUND_MS;
 *  - pins spinners to masks and unpins them, one change every
 *    millisecond. The masks are disjoint, and at most as many as the
 *    limit. So the spinners fit the limit whenever the changes stop.
 *
 * At the end of an epoch, the largest mm_cid seen must be below the
 * limit, or below the largest effective value read. The effective value
 * must equal the limit, at once or 50 ms later. No counter of
 * cpu.concurrency.stat of the parent but nr_held may be below its value
 * at the end of the epoch before, and rotated_usec may not be above
 * held_usec.
 *
 * Four flags add to this:
 *  - STRESS_HOTPLUG: a thread takes CPUs offline and back online;
 *  - STRESS_CPUSET: a thread rewrites the parent's cpuset.cpus, which
 *    changes the allowed CPUs of every thread;
 *  - STRESS_NS: the process runs in a concurrency ID namespace. Its
 *    threads also sample ns_cid, which must stay below the maximum of
 *    the namespace;
 *  - STRESS_WIDE: the masks ask for more CPUs than the limit, see
 *    stress_wide_masks(). An epoch lasts STRESS_WIDE_EPOCH_MS. The
 *    process moves once, each spinner is pinned once, and the masks then
 *    stay until the end of the epoch. That is three rotation periods: the
 *    spinners which wait for a slot get one by rotation.
 *
 * Once the threads have ended, nr_held of the parent must be 0 within
 * 2 s. The variant with STRESS_WIDE alone must have counted a rotation.
 * With the other flags the masks and the CPUs change too often for that.
 *
 * A variant prints how many affinity changes, retries, cpuset writes,
 * hotplug cycles, holds and rotations it made. It fails if an unpin never
 * succeeds, if a cpuset variant wrote no cpuset, or if a hotplug variant
 * unplugged no CPU. A hotplug variant is skipped, before it runs, when no
 * CPU can go offline.
 */
#define STRESS_SPINNERS		6
#define STRESS_EPOCHS		12
#define STRESS_EPOCH_MS		150
#define STRESS_SUBROUND_MS	15
#define STRESS_HOTPLUG		1
#define STRESS_CPUSET		2
#define STRESS_NS		4
#define STRESS_WIDE		8
#define STRESS_WIDE_EPOCH_MS	300
#define STRESS_WIDE_MASKS	5
/*
 * The ceiling of the cgroup in the namespace variants. The limits
 * written stay at or below it.
 */
#define STRESS_NS_CEILING	3
#define STRESS_NS_CEILING_STR	"3"
#ifndef CLONE_NEWCID
#define CLONE_NEWCID	0x00000040
#endif
#ifndef PR_GET_NS_CID_MAX
#define PR_GET_NS_CID_MAX	82
#endif
struct stress_shared {
	long long max_cid[2];		/* by epoch parity, -1 for none */
	long max_us[2];			/* how long after the epoch began */
	struct timespec epoch_start;
	const char *cpuset_cg;
	volatile int epoch;
	int max_who[2], max_cpu[2];	/* who saw it, and where */
	volatile int stop;
	volatile int cur_cpu[STRESS_SPINNERS];
	unsigned int seed;
	int nconf;			/* configured CPUs, online or not */
	int keep;			/* the cpuset keeps CPUs 0..keep-1 */
	volatile int hotplug_unavail;
	volatile int cpuset_err;
	/* What the variant did, printed at its end. */
	unsigned long hotplug_cycles;	/* a CPU went offline and came back */
	unsigned long cpuset_writes;	/* accepted writes to cpuset.cpus */
	unsigned long aff_calls;	/* affinity changes asked for */
	unsigned long aff_retries;	/* EINVAL results which were retried */
	unsigned long aff_failed;	/* unpins which never succeeded */
	volatile int max_eff;		/* largest effective value this epoch */
	int ns;				/* maximum of the ID namespace, or 0 */
	unsigned int max_ns[2];		/* largest ns_cid, by epoch parity */
	volatile int noext;		/* a thread got no extended rseq area */
};

/*
 * Read the effective value in a loop and keep the largest one. During a
 * decrease, the value can stay at the number of CPUs still in use for a
 * moment, behind a cpuset change or an eviction. The IDs follow that
 * value (see the design document). The check at the end of an epoch
 * compares the IDs seen with the largest value read.
 */
static void *stress_poll_effective(void *arg)
{
	struct stress_shared *sh = arg;

	while (!sh->stop) {
		long eff = cg_read_long(sh->cpuset_cg, "cpu.concurrency.effective");

		if (eff > sh->max_eff)
			sh->max_eff = eff;
	}
	return NULL;
}

static unsigned int stress_lcg(unsigned int *seed)
{
	*seed = *seed * 1103515245u + 12345u;
	return *seed >> 8;
}

static unsigned int stress_rand(struct stress_shared *sh)
{
	sh->seed = sh->seed * 1103515245u + 12345u;
	return sh->seed >> 8;
}

static void stress_note_cid(struct stress_shared *sh, __u32 cid, int who, int cpu)
{
	int e = sh->epoch & 1;
	long long *m = &sh->max_cid[e];
	long long old = __atomic_load_n(m, __ATOMIC_RELAXED);

	while (cid > old &&
	       !__atomic_compare_exchange_n(m, &old, cid, true,
					    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
	if (cid > old) {
		sh->max_who[e] = who;
		sh->max_cpu[e] = cpu;
		sh->max_us[e] = us_since(&sh->epoch_start);
	}
}

struct stress_spinner {
	struct stress_shared *sh;
	pthread_t thread;
	int idx;			/* -1 for a short-lived thread */
};

/*
 * Record @cid as the largest ns_cid of the epoch. The maximum is
 * unsigned: an ID which the kernel never assigned is a large value, not
 * a negative one.
 */
static void stress_note_ns(struct stress_shared *sh, unsigned int cid)
{
	unsigned int *m = &sh->max_ns[sh->epoch & 1];
	unsigned int old = __atomic_load_n(m, __ATOMIC_RELAXED);

	while (cid > old &&
	       !__atomic_compare_exchange_n(m, &old, cid, true,
					    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

static void *stress_spin(void *arg)
{
	struct stress_spinner *sp = arg;
	struct stress_shared *sh = sp->sh;
	struct rseq *ext = NULL, *r = thread_rseq();
	int has_cid = rseq_has_mm_cid();
	int sleepy = sp->idx >= 0 && (sp->idx & 1);
	unsigned int n = 0;
	size_t ext_len = 0;
	struct timespec until;

	if (sh->ns) {
		ext = rseq_ext_register(&ext_len);
		if (!ext) {
			sh->noext = 1;
			return NULL;
		}
		r = ext;
		has_cid = 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &until);
	ts_add_ms(&until, 2);	/* a thread with idx < 0 lives 2 ms */
	while (!sh->stop) {
		int cpu = ext ? (int)rseq_field(ext, RSEQ_CPU_ID_OFFSET) : sched_getcpu();

		if (sp->idx >= 0 && cpu >= 0)
			sh->cur_cpu[sp->idx] = cpu;
		if (has_cid)
			stress_note_cid(sh, rseq_field(r, RSEQ_MM_CID_OFFSET), sp->idx, cpu);
		if (ext)
			stress_note_ns(sh, rseq_field(ext, RSEQ_NS_CID_OFFSET));
		/* A sleepy thread sleeps 50 to 650 us every 300 samples. */
		if (sleepy && ++n % 300 == 0)
			usleep(50 + (n / 300) % 7 * 100);
		if (sp->idx < 0) {
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);
			if (!ts_before(&now, &until))
				break;
		}
	}
	if (ext)
		rseq_ext_unregister(ext, ext_len);
	return NULL;
}

/* Create and reap short-lived spinners back to back. */
static void *stress_churn_threads(void *arg)
{
	struct stress_shared *sh = arg;

	while (!sh->stop) {
		struct stress_spinner sp = { .sh = sh, .idx = -1 };

		if (pthread_create(&sp.thread, NULL, stress_spin, &sp))
			break;
		pthread_join(sp.thread, NULL);
	}
	return NULL;
}

/* Take a CPU other than 0 offline and bring it back, over and over. */
static void *stress_hotplug(void *arg)
{
	struct stress_shared *sh = arg;
	unsigned int seed = 4242;
	int failures = 0;

	while (!sh->stop) {
		unsigned int r = stress_lcg(&seed);
		int cpu = 1 + r % (sh->nconf - 1);

		if (write_cpu_online(cpu, 0)) {
			/* No hotplug at all, or not of this CPU. */
			if (++failures >= 3 * sh->nconf) {
				sh->hotplug_unavail = 1;
				break;
			}
			usleep(1000);
			continue;
		}
		usleep(10000 + (r >> 8) % 20000);
		if (!write_cpu_online(cpu, 1))
			sh->hotplug_cycles++;
		usleep(10000 + (r >> 12) % 20000);
	}
	return NULL;
}

/*
 * Rewrite the cpuset of the cgroup in a loop. Each value is every CPU,
 * every CPU but one, or the first CPUs and a block of the others. Every
 * value keeps CPUs 0 to keep - 1. The masks of the spinners stay within
 * these CPUs, so no write leaves a spinner without a CPU of its mask.
 */
static void *stress_cpuset(void *arg)
{
	struct stress_shared *sh = arg;
	unsigned int seed = 777;
	char buf[64];
	int n = sh->nconf, k = sh->keep;

	while (!sh->stop) {
		unsigned int r = stress_lcg(&seed);
		int cpu, w, start;

		switch (n > k ? r % 3 : 0) {
		case 0:
			snprintf(buf, sizeof(buf), "0-%d", n - 1);
			break;
		case 1:
			cpu = k + (r >> 4) % (n - k);
			if (cpu == n - 1)
				snprintf(buf, sizeof(buf), "0-%d", n - 2);
			else
				snprintf(buf, sizeof(buf), "0-%d,%d-%d", cpu - 1, cpu + 1, n - 1);
			break;
		default:
			w = (n - k) / 2 < 1 ? 1 : (n - k) / 2;
			start = k + (r >> 4) % (n - k - w + 1);
			snprintf(buf, sizeof(buf), "0-%d,%d-%d", k - 1, start, start + w - 1);
			break;
		}
		if (cg_write(sh->cpuset_cg, "cpuset.cpus", buf)) {
			sh->cpuset_err = 1;
			break;
		}
		sh->cpuset_writes++;
		usleep(5000 + (r >> 12) % 20000);
	}
	snprintf(buf, sizeof(buf), "0-%d", n - 1);
	cg_write(sh->cpuset_cg, "cpuset.cpus", buf);
	return NULL;
}

struct stress_param {
	char *base, *parent, *a, *b;
	int nlimits;
	int limits[3];
	unsigned int flags;
};

/*
 * Set the affinity of @thread. A change which races with a cpuset update
 * fails with EINVAL and keeps the old mask. A cpuset update is a write,
 * or hotplug changing the effective CPUs of the cpuset. The variants
 * with a flag set retry on EINVAL, up to 1000 times and 100 us apart.
 * Count the calls and the retries in @sh.
 */
static int stress_setaffinity(struct stress_param *p, struct stress_shared *sh, pthread_t thread,
			      cpu_set_t *set)
{
	int err, tries = 0;

	sh->aff_calls++;
	do {
		err = pthread_setaffinity_np(thread, sizeof(*set), set);
		if (err == EINVAL && p->flags && ++tries < 1000) {
			sh->aff_retries++;
			usleep(100);
		} else {
			break;
		}
	} while (1);
	return err;
}

/*
 * Unpin every spinner. A mask of every CPU is valid whatever the cpuset
 * and the limit are. So an unpin which still fails after its retries is
 * counted, and fails the variant at the end of the epoch.
 */
static void stress_unpin_all(struct stress_param *p, struct stress_shared *sh,
			     struct stress_spinner *sp)
{
	cpu_set_t all;
	int i;

	stress_all_cpus(&all);
	for (i = 0; i < STRESS_SPINNERS; i++) {
		if (stress_setaffinity(p, sh, sp[i].thread, &all))
			sh->aff_failed++;
	}
}

/*
 * Build the STRESS_WIDE_MASKS masks of a STRESS_WIDE epoch in @pool, from
 * the @ncpu first CPUs. With c(k) the CPU (@off + k) modulo @ncpu, they
 * are:
 *
 *   {c(0)}  {c(1)}  {c(2), c(3)}  {c(3), c(4)}  {c(5)}
 *
 * The third and the fourth overlap without nesting. With six CPUs or
 * more, the five masks need four CPUs at least. The limit is 3 at most.
 */
static void stress_wide_masks(cpu_set_t *pool, int off, int ncpu)
{
	static const int first[STRESS_WIDE_MASKS] = { 0, 1, 2, 3, 5 };
	static const int len[STRESS_WIDE_MASKS] = { 1, 1, 2, 2, 1 };
	int i, k;

	for (i = 0; i < STRESS_WIDE_MASKS; i++) {
		CPU_ZERO(&pool[i]);
		for (k = 0; k < len[i]; k++)
			CPU_SET((off + first[i] + k) % ncpu, &pool[i]);
	}
}

/*
 * Read cpu.concurrency.stat of @cg into @st and compare it with @prev, the
 * values read before. Return 0 if no counter but nr_held went down, and if
 * rotated_usec is not above held_usec. Else print the values and return
 * -1.
 */
static int stress_stat_check(const char *cg, const struct conc_stat *prev, struct conc_stat *st)
{
	if (conc_stat_read(cg, st)) {
		fprintf(stderr, "cpu.concurrency.stat cannot be read\n");
		return -1;
	}
	if (st->nr_held >= 0 && st->nr_holds >= prev->nr_holds && st->held_usec >= prev->held_usec &&
	    st->nr_rotations >= prev->nr_rotations && st->rotated_usec >= prev->rotated_usec &&
	    st->rotated_usec <= st->held_usec)
		return 0;
	fprintf(stderr, "cpu.concurrency.stat: nr_held %lld, nr_holds %lld -> %lld, held_usec %lld -> %lld, nr_rotations %lld -> %lld, rotated_usec %lld -> %lld\n",
		st->nr_held, prev->nr_holds, st->nr_holds, prev->held_usec, st->held_usec,
		prev->nr_rotations, st->nr_rotations, prev->rotated_usec, st->rotated_usec);
	return -1;
}

static int stress_run(const char *cgroup, void *arg)
{
	struct stress_param *p = arg;
	struct stress_shared sh = { .seed = 12345 };
	struct stress_spinner sp[STRESS_SPINNERS];
	struct conc_stat stat = { }, prev = { };
	pthread_t churn, hotplug, cpuset, poll;
	char buf[32];
	int i, e, ret = 0, in_a = 1;

	sh.nconf = get_nprocs_conf();
	sh.keep = sh.nconf / 2 < 2 ? 2 : sh.nconf / 2;
	sh.cpuset_cg = p->parent;
	if (p->flags & STRESS_NS) {
		pid_t pid;
		int status;

		/*
		 * The new namespace is for the children of the caller. Run the
		 * stress in a child.
		 */
		if (unshare(CLONE_NEWCID)) {
			if (errno == EINVAL || errno == EPERM)
				return skip("no concurrency ID namespace (CLONE_NEWCID)");
			fprintf(stderr, "unshare(CLONE_NEWCID): %s\n", strerror(errno));
			return 1;
		}
		pid = fork();
		if (pid < 0)
			return 1;
		if (pid > 0) {
			if (waitpid(pid, &status, 0) != pid)
				return 1;
			return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
		}
		sh.ns = prctl(PR_GET_NS_CID_MAX, 0, 0, 0, 0);
		if (sh.ns != STRESS_NS_CEILING) {
			fprintf(stderr, "PR_GET_NS_CID_MAX %d under the ceiling %d\n", sh.ns,
				STRESS_NS_CEILING);
			return 1;
		}
	}
	for (i = 0; i < STRESS_SPINNERS; i++) {
		sp[i].sh = &sh;
		sp[i].idx = i;
		if (pthread_create(&sp[i].thread, NULL, stress_spin, &sp[i]))
			return 1;
	}
	if (pthread_create(&churn, NULL, stress_churn_threads, &sh))
		return 1;
	if (pthread_create(&poll, NULL, stress_poll_effective, &sh))
		return 1;
	if ((p->flags & STRESS_HOTPLUG) &&
	    pthread_create(&hotplug, NULL, stress_hotplug, &sh))
		return 1;
	if ((p->flags & STRESS_CPUSET) &&
	    pthread_create(&cpuset, NULL, stress_cpuset, &sh))
		return 1;

	for (e = 0; e < STRESS_EPOCHS && !ret; e++) {
		int limit = p->limits[e % p->nlimits];
		struct timespec end, now;
		long long max;
		int eff, max_eff;

		/* Unpin every spinner first: each epoch starts from the same state. */
		stress_unpin_all(p, &sh, sp);

		snprintf(buf, sizeof(buf), "%d", limit);
		ret = cg_write(p->parent, "cpu.concurrency.max", buf);
		/* The children inherit, or restate, the parent's limit. */
		if (!ret)
			ret = cg_write(p->a, "cpu.concurrency.max", stress_rand(&sh) & 1 ? buf : "max");
		if (!ret)
			ret = cg_write(p->b, "cpu.concurrency.max", stress_rand(&sh) & 1 ? buf : "max");
		if (ret) {
			fprintf(stderr, "epoch %d: writing the limit %d failed: %s\n", e, limit, strerror(-ret));
			ret = 12;
			break;
		}
		/* Let every thread be scheduled in under the new bound. */
		usleep(50000);
		__atomic_store_n(&sh.max_cid[(e + 1) & 1], -1, __ATOMIC_RELAXED);
		/*
		 * 0 stands for no ns_cid sample. It is the lowest ID, below
		 * every limit.
		 */
		__atomic_store_n(&sh.max_ns[(e + 1) & 1], 0, __ATOMIC_RELAXED);
		sh.max_eff = 0;
		clock_gettime(CLOCK_MONOTONIC, &sh.epoch_start);
		sh.epoch = e + 1;

		clock_gettime(CLOCK_MONOTONIC, &end);
		ts_add_ms(&end, (p->flags & STRESS_WIDE) ? STRESS_WIDE_EPOCH_MS : STRESS_EPOCH_MS);
		do {
			cpu_set_t pool[STRESS_WIDE_MASKS], all;
			int npool = 0, j, ncpu, off;
			struct timespec sub;

			/* Unpin all: no mask is left from the last round. */
			stress_all_cpus(&all);
			stress_unpin_all(p, &sh, sp);

			/*
			 * Build the masks: at most "limit" of them, and
			 * disjoint. Take the CPUs which the cpuset variant
			 * never removes, or every CPU in the other variants.
			 * Cut them into blocks, from a random offset. A thread
			 * pinned to a block which does not hold its CPU must
			 * move. With one mask, remove the CPU of the first
			 * spinner from it.
			 */
			ncpu = (p->flags & STRESS_CPUSET) ? sh.keep : sh.nconf;
			npool = limit < 3 ? limit : 3;
			if (npool > ncpu)
				npool = ncpu;
			off = stress_rand(&sh) % ncpu;
			for (i = 0; i < npool; i++)
				CPU_ZERO(&pool[i]);
			for (j = 0; j < ncpu && npool; j++)
				CPU_SET((off + j) % ncpu, &pool[j * npool / ncpu]);
			if (npool == 1 && sh.cur_cpu[0] >= 0 && sh.cur_cpu[0] < ncpu &&
			    CPU_COUNT(&pool[0]) > 1)
				CPU_CLR(sh.cur_cpu[0], &pool[0]);
			if (p->flags & STRESS_WIDE) {
				npool = STRESS_WIDE_MASKS;
				stress_wide_masks(pool, off, ncpu);
			}

			/* Move the whole process to the other sibling. */
			snprintf(buf, sizeof(buf), "%d", getpid());
			ret = cg_write(in_a ? p->b : p->a, "cgroup.procs", buf);
			if (ret) {
				fprintf(stderr, "epoch %d: moving the process to %s failed: %s (effective %ld)\n",
					e, in_a ? "b" : "a", strerror(-ret),
					cg_read_long(p->parent, "cpu.concurrency.effective"));
				ret = 13;
				break;
			}
			in_a = !in_a;

			clock_gettime(CLOCK_MONOTONIC, &sub);
			ts_add_ms(&sub, STRESS_SUBROUND_MS);
			/* STRESS_WIDE: pin each spinner once, then keep the masks. */
			for (i = 0; (p->flags & STRESS_WIDE) && i < STRESS_SPINNERS && !ret; i++) {
				if (stress_setaffinity(p, &sh, sp[i].thread,
						       &pool[stress_rand(&sh) % npool])) {
					fprintf(stderr, "epoch %d: pinning spinner %d failed\n", e, i);
					ret = 14;
				}
			}
			if (p->flags & STRESS_WIDE) {
				clock_gettime(CLOCK_MONOTONIC, &now);
				while (!ret && ts_before(&now, &end)) {
					usleep(1000);
					clock_gettime(CLOCK_MONOTONIC, &now);
				}
				break;
			}
			do {
				unsigned int r = stress_rand(&sh);
				cpu_set_t *set = (r & 3) == 0 || !npool ?
						 &all : &pool[(r >> 2) % npool];

				struct timespec t0;
				int err;

				clock_gettime(CLOCK_MONOTONIC, &t0);
				err = stress_setaffinity(p, &sh,
							 sp[(r >> 8) % STRESS_SPINNERS].thread, set);
				if (err) {
					char cpus[64] = "?";

					clock_gettime(CLOCK_MONOTONIC, &now);
					if (p->flags & STRESS_CPUSET)
						cg_read(p->parent, "cpuset.cpus.effective", cpus, sizeof(cpus));
					fprintf(stderr, "epoch %d: setaffinity(%s) of thread %u failed: %s (after %ld us, cpuset %.*s, effective %ld)\n",
						e, set == &all ? "all" : "pool", (r >> 8) % STRESS_SPINNERS,
						strerror(err),
						us_since(&t0),
						(int)strcspn(cpus, "\n"), cpus,
						cg_read_long(p->parent, "cpu.concurrency.effective"));
					ret = 14;
					break;
				}
				usleep(1000);
				clock_gettime(CLOCK_MONOTONIC, &now);
			} while (ts_before(&now, &sub));
		} while (!ret && ts_before(&now, &end));
		if (ret)
			break;

		max = __atomic_load_n(&sh.max_cid[(e + 1) & 1], __ATOMIC_RELAXED);
		eff = cg_read_long(p->parent, "cpu.concurrency.effective");
		max_eff = sh.max_eff;
		if (rseq_has_mm_cid() && max >= limit && max >= max_eff) {
			fprintf(stderr, "epoch %d: mm_cid %lld seen under limit %d by thread %d on cpu %d, %ld us in (effective now %d, at most %d)\n",
				e, max, limit, sh.max_who[(e + 1) & 1],
				sh.max_cpu[(e + 1) & 1], sh.max_us[(e + 1) & 1], eff, max_eff);
			ret = 15;
		} else if (eff != limit) {
			char cpus[64] = "?";
			int again;

			/*
			 * The value can differ while a change is being
			 * applied: a decrease whose tasks have not returned to
			 * user space yet, or a set-move. Read it again 50 ms
			 * later to tell this from a value which stays wrong.
			 */
			usleep(50000);
			again = cg_read_long(p->parent, "cpu.concurrency.effective");
			if (p->flags & STRESS_CPUSET)
				cg_read(p->parent, "cpuset.cpus.effective", cpus, sizeof(cpus));
			if (again == limit) {
				ksft_print_msg("epoch %d: effective %d under limit %d at the end, %d 50 ms later (cpuset %.*s)\n",
					       e, eff, limit, again, (int)strcspn(cpus, "\n"), cpus);
			} else {
				fprintf(stderr, "epoch %d: effective %d, limit %d (50 ms later %d, cpuset %.*s)\n",
					e, eff, limit, again, (int)strcspn(cpus, "\n"), cpus);
				ret = 16;
			}
		} else if (rseq_has_mm_cid() && max >= limit) {
			ksft_print_msg("epoch %d: mm_cid %lld under limit %d while %d CPUs were in force\n",
				       e, max, limit, max_eff);
		}
		if (!ret && sh.cpuset_err) {
			fprintf(stderr, "epoch %d: writing cpuset.cpus failed\n", e);
			ret = 17;
		}
		if (!ret && stress_stat_check(p->parent, &prev, &stat))
			ret = 23;
		prev = stat;
		if (!ret && sh.aff_failed) {
			fprintf(stderr, "epoch %d: %lu unpins never succeeded\n", e, sh.aff_failed);
			ret = 20;
		}
		if (!ret && sh.ns) {
			unsigned int max_ns = __atomic_load_n(&sh.max_ns[(e + 1) & 1],
							      __ATOMIC_RELAXED);

			if (sh.noext) {
				fprintf(stderr, "a thread could not register an extended rseq area\n");
				ret = 18;
			} else if (max_ns >= (unsigned int)sh.ns) {
				fprintf(stderr, "epoch %d: ns_cid %u seen under the maximum %d (limit %d)\n",
					e, max_ns, sh.ns, limit);
				ret = 19;
			}
		}
	}

	sh.stop = 1;
	pthread_join(churn, NULL);
	pthread_join(poll, NULL);
	if (p->flags & STRESS_HOTPLUG)
		pthread_join(hotplug, NULL);
	if (p->flags & STRESS_CPUSET)
		pthread_join(cpuset, NULL);
	for (i = 0; i < STRESS_SPINNERS; i++)
		pthread_join(sp[i].thread, NULL);
	/* Only this thread is left, and it runs: no task of the cgroup is held. */
	for (i = 0; i < 200 && !ret; i++) {
		if (stress_stat_check(p->parent, &prev, &stat)) {
			ret = 23;
			break;
		}
		if (!stat.nr_held)
			break;
		usleep(10000);
	}
	if (!ret && stat.nr_held) {
		fprintf(stderr, "nr_held is %lld 2 s after the threads ended\n", stat.nr_held);
		ret = 24;
	}
	ksft_print_msg("%lu affinity changes, %lu retries, %lu cpuset writes, %lu hotplug cycles, %lld holds, %lld rotations\n",
		       sh.aff_calls, sh.aff_retries, sh.cpuset_writes, sh.hotplug_cycles,
		       stat.nr_holds, stat.nr_rotations);
	if (!ret && p->flags == STRESS_WIDE && !stat.nr_rotations) {
		fprintf(stderr, "more masks than slots, and no rotation was counted\n");
		ret = 25;
	}
	/*
	 * A variant which did not do what it is named for must not pass.
	 * stress_test() checked that a CPU can go offline, so hotplug which
	 * stops working is a failure.
	 */
	if (!ret && (p->flags & STRESS_HOTPLUG) && (sh.hotplug_unavail || !sh.hotplug_cycles)) {
		fprintf(stderr, "CPU hotplug stopped working: %lu cycles\n", sh.hotplug_cycles);
		ret = 21;
	}
	if (!ret && (p->flags & STRESS_CPUSET) && !sh.cpuset_writes) {
		fprintf(stderr, "no write to cpuset.cpus succeeded\n");
		ret = 22;
	}
	return ret;
}

/*
 * Return true if a CPU other than 0 goes offline and comes back. The
 * hotplug variants are skipped otherwise, before they run.
 */
static bool stress_can_hotplug(void)
{
	int cpu;

	for (cpu = 1; cpu < get_nprocs_conf(); cpu++) {
		if (!write_cpu_online(cpu, 0))
			return !write_cpu_online(cpu, 1);
	}
	return false;
}

static int stress_test(const char *root, const char *name, unsigned int flags)
{
	struct stress_param param = { .flags = flags };
	int ret = KSFT_FAIL, i, r;

	if (need_cpus(4))
		return KSFT_SKIP;
	if ((flags & STRESS_HOTPLUG) && !stress_can_hotplug())
		return skip("no CPU can go offline");
	for (i = 0; i < 3 && i + 1 < get_nprocs(); i++)
		param.limits[param.nlimits++] = i + 1;

	param.parent = cg_name(root, name);
	if (!param.parent)
		goto cleanup;
	param.a = cg_name(param.parent, "a");
	param.b = cg_name(param.parent, "b");
	if (!param.a || !param.b)
		goto cleanup;
	if ((flags & STRESS_CPUSET) &&
	    cg_read_strstr(root, "cgroup.subtree_control", "cpuset") &&
	    cg_write(root, "cgroup.subtree_control", "+cpuset")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}
	/* The namespace takes the ceiling the parent is created with. */
	if (flags & STRESS_NS) {
		param.base = param.parent;
		param.parent = cg_name(param.base, "p");
		if (!param.parent)
			goto cleanup;
		if (cg_create(param.base) || cg_write(param.base, "cgroup.subtree_control", "+cpu") ||
		    ((flags & STRESS_CPUSET) &&
		     cg_write(param.base, "cgroup.subtree_control", "+cpuset")) ||
		    cg_write(param.base, "cpu.concurrency.child_ceiling", STRESS_NS_CEILING_STR))
			goto cleanup;
		free(param.a);
		free(param.b);
		param.a = cg_name(param.parent, "a");
		param.b = cg_name(param.parent, "b");
		if (!param.a || !param.b)
			goto cleanup;
	}
	if (cg_create(param.parent))
		goto cleanup;
	if (cg_write(param.parent, "cgroup.subtree_control", "+cpu"))
		goto cleanup;
	if (cg_create(param.a) || cg_create(param.b))
		goto cleanup;
	if (cg_write(param.parent, "cpu.concurrency.max", "2"))
		goto cleanup;

	r = cg_run(param.a, stress_run, &param);
	if (r == KSFT_SKIP)
		ret = KSFT_SKIP;
	if (r)
		goto cleanup;

	ret = KSFT_PASS;
cleanup:
	if (param.a)
		cg_destroy(param.a);
	if (param.b)
		cg_destroy(param.b);
	if (param.parent)
		cg_destroy(param.parent);
	if (param.base)
		cg_destroy(param.base);
	free(param.a);
	free(param.b);
	free(param.parent);
	free(param.base);
	return ret;
}

static int test_concurrency_stress(const char *root)
{
	return stress_test(root, "cc_test_stress", 0);
}

static int test_concurrency_stress_hotplug(const char *root)
{
	return stress_test(root, "cc_test_stress_hp", STRESS_HOTPLUG);
}

static int test_concurrency_stress_cpuset(const char *root)
{
	return stress_test(root, "cc_test_stress_cs", STRESS_CPUSET);
}

static int test_concurrency_stress_mixed(const char *root)
{
	return stress_test(root, "cc_test_stress_mix", STRESS_HOTPLUG | STRESS_CPUSET);
}

static int test_concurrency_stress_ns(const char *root)
{
	return stress_test(root, "cc_test_stress_ns", STRESS_NS);
}

static int test_concurrency_stress_ns_mixed(const char *root)
{
	return stress_test(root, "cc_test_stress_nsmix", STRESS_NS | STRESS_HOTPLUG | STRESS_CPUSET);
}

static int test_concurrency_stress_wide(const char *root)
{
	return stress_test(root, "cc_test_stress_wide", STRESS_WIDE);
}

static int test_concurrency_stress_wide_mixed(const char *root)
{
	return stress_test(root, "cc_test_stress_widemix",
			   STRESS_WIDE | STRESS_HOTPLUG | STRESS_CPUSET);
}

#define T(x) { x, #x }
struct cc_test {
	int (*fn)(const char *root);
	const char *name;
} tests[] = {
	T(test_concurrency_stress),
	T(test_concurrency_stress_hotplug),
	T(test_concurrency_stress_cpuset),
	T(test_concurrency_stress_mixed),
	T(test_concurrency_stress_ns),
	T(test_concurrency_stress_ns_mixed),
	T(test_concurrency_stress_wide),
	T(test_concurrency_stress_wide_mixed),
};

#undef T
int main(int argc, char *argv[])
{
	struct kernel_state ks;
	char root[PATH_MAX];
	int i, r, plan = 0;
	char *probe;

	ksft_print_header();
	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");

	if (cg_read_strstr(root, "cgroup.subtree_control", "cpu"))
		if (cg_write(root, "cgroup.subtree_control", "+cpu"))
			ksft_exit_skip("Failed to set cpu controller\n");
	if (cg_read_strstr(root, "cgroup.subtree_control", "cpuset"))
		if (cg_write(root, "cgroup.subtree_control", "+cpuset"))
			ksft_exit_skip("Failed to set cpuset controller\n");

	probe = cg_name(root, "cc_test_probe");
	if (!probe || cg_create(probe))
		ksft_exit_skip("Failed to create a probe cgroup\n");
	if (cg_read_strcmp(probe, "cpu.concurrency.max", "max\n") ||
	    cg_read_strcmp(probe, "cpu.concurrency.ceiling", "max\n")) {
		cg_destroy(probe);
		ksft_exit_skip("cpu.concurrency.max is not available\n");
	}
	cg_destroy(probe);
	free(probe);

	cpus_record_online();
	/* An argument selects the tests whose name contains it. */
	for (i = 0; i < ARRAY_SIZE(tests); i++)
		plan += argc < 2 || strstr(tests[i].name, argv[1]);
	ksft_set_plan(plan);
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		if (argc > 1 && !strstr(tests[i].name, argv[1]))
			continue;
		/* A variant fails when the kernel warns while it runs. */
		kernel_state_record(&ks);
		r = tests[i].fn(root);
		if (kernel_state_check(&ks, tests[i].name))
			r = KSFT_FAIL;
		switch (r) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip("%s\n", tests[i].name);
			break;
		default:
			ksft_test_result_fail("%s\n", tests[i].name);
			break;
		}
		if (cpus_restore())
			ksft_print_msg("%s left a CPU offline that does not come back\n",
				       tests[i].name);
	}

	ksft_finished();
}
