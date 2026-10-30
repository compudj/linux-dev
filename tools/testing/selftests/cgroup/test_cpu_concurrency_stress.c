// SPDX-License-Identifier: GPL-2.0
#include "cpu_concurrency_util.h"
/*
 * Stress tests for cpu.concurrency.max: the limit, the IDs under it,
 * CPU hotplug, cpuset writes and IPC namespaces with IDs, each for a few
 * seconds. Apart from the tests of test_cpu_concurrency.c, which the
 * kselftest timeout fits.
 */

/*
 * Stress: a process in a limited cgroup, its threads sampling their rseq
 * mm_cid without pause, while a controller changes everything that can
 * disturb the bound: affinity masks drawn from a pool that keeps the
 * number of classes at or below the limit, moves of the process between
 * two sibling cgroups, constant thread creation and exit, and limit
 * changes between epochs. Half of the long-lived threads spin, the other
 * half sleep briefly every few hundred samples, so that slot release at
 * sleep, claims at wakeup and the reclaim of idle slots are exercised as
 * much as preemption. Each epoch asserts that no thread saw an ID at or
 * above the limit and that the effective value did not relax.
 *
 * Two more disturbances can run alongside: a thread taking CPUs offline
 * and back online throughout, and a thread rewriting the cpuset of the
 * cgroup, which changes every thread's allowed CPUs from the kernel side.
 * The process can also run in an IPC namespace with concurrency IDs, its
 * threads then sampling ns_cid as well, which must stay below the limit
 * like mm_cid, and the namespace's soft target must follow the limit.
 */
#define STRESS_SPINNERS		6
#define STRESS_EPOCHS		12
#define STRESS_EPOCH_MS		150
#define STRESS_SUBROUND_MS	15
#define STRESS_HOTPLUG		1
#define STRESS_CPUSET		2
#define STRESS_NS		4
struct stress_shared {
	volatile int epoch;
	long long max_cid[2];		/* indexed by epoch parity, -1 for none */
	int max_who[2], max_cpu[2];	/* who saw it, and where */
	long max_us[2];			/* how long after the epoch began */
	struct timespec epoch_start;
	volatile int stop;
	volatile int cur_cpu[STRESS_SPINNERS];
	unsigned int seed;
	int nconf;			/* configured CPUs, online or not */
	int keep;			/* CPUs 0 to keep - 1 stay in the cpuset */
	volatile int hotplug_unavail;
	volatile int cpuset_err;
	const char *cpuset_cg;
	volatile int max_eff;		/* largest effective value seen this epoch */
	int ns;				/* in a namespace with concurrency IDs */
	unsigned int max_ns[2];		/* largest ns_cid seen, by epoch parity */
	volatile int noext;		/* a thread got no extended rseq area */
};

/*
 * The value in force, read as often as the file can be: a decrease
 * stalled behind a cpuset change or an eviction in flight keeps it at
 * the number of CPUs still in use for a moment, and the IDs follow it,
 * see the design document. The check at the end of an epoch compares
 * the IDs seen against the largest value read.
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
	int idx;			/* -1 for a short-lived thread */
	pthread_t thread;
};

/* Unsigned: an ID the kernel never assigned is a large value, not a negative one. */
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
	ts_add_ms(&until, 2);	/* short-lived: 2ms */
	while (!sh->stop) {
		int cpu = ext ? (int)rseq_field(ext, RSEQ_CPU_ID_OFFSET) : sched_getcpu();

		if (sp->idx >= 0 && cpu >= 0)
			sh->cur_cpu[sp->idx] = cpu;
		if (has_cid)
			stress_note_cid(sh, rseq_field(r, RSEQ_MM_CID_OFFSET), sp->idx, cpu);
		if (ext)
			stress_note_ns(sh, rseq_field(ext, RSEQ_NS_CID_OFFSET));
		/* The sleepy regime: a short sleep, variable, every few hundred samples. */
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
		write_cpu_online(cpu, 1);
		usleep(10000 + (r >> 12) % 20000);
	}
	return NULL;
}

/*
 * Rewrite the cgroup's cpuset: all CPUs, all but one, or the first ones
 * with a window of the rest. Every value keeps CPUs 0 to keep - 1, which
 * the spinners' pool masks stay within: under a limit a cpuset write that
 * would cut a requested mask is refused.
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
		usleep(5000 + (r >> 12) % 20000);
	}
	snprintf(buf, sizeof(buf), "0-%d", n - 1);
	cg_write(sh->cpuset_cg, "cpuset.cpus", buf);
	return NULL;
}

struct stress_param {
	char *parent, *a, *b;
	int nlimits;
	int limits[3];
	unsigned int flags;
};

/*
 * A mask change that races with a cpuset update (a write, or hotplug
 * adjusting the cpuset's effective CPUs) is refused with EINVAL and the
 * previous mask is kept: repeat it, so that no class lingers.
 */
static int stress_setaffinity(struct stress_param *p, pthread_t thread, cpu_set_t *set)
{
	int err, tries = 0;

	do {
		err = pthread_setaffinity_np(thread, sizeof(*set), set);
		if (err == EINVAL && p->flags && ++tries < 1000)
			usleep(100);
		else
			break;
	} while (1);
	return err;
}

static int stress_run(const char *cgroup, void *arg)
{
	struct stress_param *p = arg;
	struct stress_shared sh = { .seed = 12345 };
	struct stress_spinner sp[STRESS_SPINNERS];
	pthread_t churn, hotplug, cpuset, poll;
	char buf[32];
	int i, e, ret = 0, in_a = 1;

	sh.nconf = get_nprocs_conf();
	sh.keep = sh.nconf / 2 < 2 ? 2 : sh.nconf / 2;
	sh.cpuset_cg = p->parent;
	if (p->flags & STRESS_NS) {
		if (unshare(CLONE_NEWIPC | UNSHARE_IPC_CID)) {
			if (errno == EINVAL || errno == EPERM)
				return skip("no IPC namespace concurrency IDs (UNSHARE_IPC_CID)");
			fprintf(stderr, "unshare(CLONE_NEWIPC | UNSHARE_IPC_CID): %s\n",
				strerror(errno));
			return 1;
		}
		sh.ns = 1;
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
		cpu_set_t all;
		long long max;
		int eff, max_eff;

		/*
		 * Masks left from the previous epoch could relax the new limit
		 * (classes above the request are honoured): unpin first.
		 */
		stress_all_cpus(&all);
		for (i = 0; i < STRESS_SPINNERS; i++)
			stress_setaffinity(p, sp[i].thread, &all);

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
		 * The ns_cid maximum is unsigned, so that an ID the kernel
		 * never assigned does not read as a negative one: 0, the
		 * lowest ID and below every limit, stands for no sample.
		 */
		__atomic_store_n(&sh.max_ns[(e + 1) & 1], 0, __ATOMIC_RELAXED);
		sh.max_eff = 0;
		clock_gettime(CLOCK_MONOTONIC, &sh.epoch_start);
		sh.epoch = e + 1;

		clock_gettime(CLOCK_MONOTONIC, &end);
		ts_add_ms(&end, STRESS_EPOCH_MS);
		do {
			cpu_set_t pool[3], all;
			int npool = 0, j, used[3];
			struct timespec sub;

			/* Unpin everything: no class is left from the last round. */
			stress_all_cpus(&all);
			for (i = 0; i < STRESS_SPINNERS; i++)
				stress_setaffinity(p, sp[i].thread, &all);

			/*
			 * Masks excluding CPUs in use, at most limit of them;
			 * within the CPUs the cpuset keeps when it is rewritten.
			 */
			for (i = 0; i < STRESS_SPINNERS && npool < limit; i++) {
				int cpu = sh.cur_cpu[i];

				for (j = 0; j < npool; j++)
					if (used[j] == cpu)
						break;
				if (j < npool)
					continue;
				used[npool] = cpu;
				if (p->flags & STRESS_CPUSET) {
					CPU_ZERO(&pool[npool]);
					for (j = 0; j < sh.keep; j++)
						CPU_SET(j, &pool[npool]);
				} else {
					stress_all_cpus(&pool[npool]);
				}
				CPU_CLR(cpu, &pool[npool]);
				npool++;
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
			do {
				unsigned int r = stress_rand(&sh);
				cpu_set_t *set = (r & 3) == 0 || !npool ?
						 &all : &pool[(r >> 2) % npool];

				struct timespec t0;
				int err;

				clock_gettime(CLOCK_MONOTONIC, &t0);
				err = stress_setaffinity(p, sp[(r >> 8) % STRESS_SPINNERS].thread, set);
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
			 * Transient, a change still being applied (a shrink whose
			 * tasks have not reached their return to user space yet,
			 * a set-move's swap), or stuck?
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
		if (!ret && sh.ns) {
			unsigned int max_ns = __atomic_load_n(&sh.max_ns[(e + 1) & 1],
							      __ATOMIC_RELAXED);
			long cur = read_sysctl("ns_cid_cur");

			if (sh.noext) {
				fprintf(stderr, "a thread could not register an extended rseq area\n");
				ret = 18;
			} else if (max_ns >= (unsigned int)limit &&
				   max_ns >= (unsigned int)max_eff) {
				fprintf(stderr, "epoch %d: ns_cid %u seen under limit %d (effective now %d, at most %d)\n",
					e, max_ns, limit, eff, max_eff);
				ret = 19;
			} else if (cur != limit) {
				fprintf(stderr, "epoch %d: ns_cid_cur %ld, limit %d\n", e, cur, limit);
				ret = 20;
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
	if (!ret && sh.hotplug_unavail)
		return skip("CPU hotplug unavailable");
	return ret;
}

static int stress_test(const char *root, const char *name, unsigned int flags)
{
	struct stress_param param = { .flags = flags };
	int ret = KSFT_FAIL, i, r;

	if (need_cpus(4))
		return KSFT_SKIP;
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
	free(param.a);
	free(param.b);
	free(param.parent);
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
};

#undef T
int main(int argc, char *argv[])
{
	char root[PATH_MAX];
	char *probe;
	int i;

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

	ksft_set_plan(ARRAY_SIZE(tests));
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		/* An argument selects the tests whose name contains it. */
		if (argc > 1 && !strstr(tests[i].name, argv[1])) {
			ksft_test_result_skip("%s\n", tests[i].name);
			continue;
		}
		switch (tests[i].fn(root)) {
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
	}

	ksft_finished();
}
