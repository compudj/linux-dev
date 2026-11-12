// SPDX-License-Identifier: GPL-2.0
/*
 * Check that the expedited membarrier commands make each targeted
 * thread execute a memory barrier.
 *
 * BPF programs count, for each thread, the memory barriers which
 * membarrier relies on. See membarrier_test_barrier.bpf.c. A kprobe is
 * attached to each of these instructions. The sched_switch tracepoint
 * has a program which counts the context switches. The counters are in a BPF array map,
 * which this program maps in memory.
 *
 * A "caller" issues the command in a loop, with a short pause between
 * two calls. It counts the calls which it started and the calls which
 * returned. Each "target" thread runs this check in a loop:
 *
 *   1. read the count of returned calls,
 *   2. read its own counters,
 *   3. read the count of started calls,
 *   4. wait until one more call has returned,
 *   5. read its own counters again,
 *   6. read the count of started calls again.
 *
 * The check is exact when the caller was between two calls at step 2
 * and at step 5. This holds when steps 1 and 3 read the same count,
 * and step 6 reads one more. Then one call ran between step 2 and
 * step 5, and no other call overlaps them. So the target must have
 * executed a memory barrier for that call: its IPI counter must have
 * changed. Otherwise membarrier missed this thread.
 *
 * A check where the target was scheduled out between step 2 and
 * step 5 is dropped. The scheduler then covers the target, and the IPI
 * is not required. A check which is not exact is dropped as well.
 *
 * The BPF programs also check the scheduler. A target can be scheduled
 * in after a task of another mm or after a kernel thread. The scheduler
 * must then execute a memory barrier after the store to rq->curr,
 * before the target is scheduled out again.
 *
 * The sync core command is checked in the same way, with the core
 * serializing instructions.
 *
 * The instructions are found by their bytes in the kernel text, which
 * is read from /proc/kcore. The list is for x86-64. It depends on what
 * the compiler inlines into the functions it names.
 */
#define _GNU_SOURCE
#include <linux/membarrier.h>
#include <linux/types.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <syscall.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include "kselftest_harness.h"
#include "membarrier_test_barrier.h"

#define MAX_TARGETS	16
/* Duration of each test case, in milliseconds. */
#define RUN_MS		1000
/* Duration of each round of the lazy TLB test case, in milliseconds. */
#define ROUND_MS	20
/* Pause of the caller between two calls, in nanoseconds. */
#define PAUSE_NS	1000
/* Bound on step 4, in case the caller is not running. */
#define MAX_SPINS	1000000
/* Seconds to wait for the targets of a test case. */
#define READY_TIMEOUT	10
#define STACK_SIZE	(64 * 1024)

struct target_stats {
	/* Exact checks where the IPI covered the target. */
	atomic_ulong by_ipi;
	/* Checks dropped because the target was scheduled out. */
	atomic_ulong by_sched;
	/* Checks where the target executed no barrier. */
	atomic_ulong missed;
	/* Membarrier IPIs which the target received. */
	atomic_ulong ipis;
	/* Times the target was scheduled in after a task of another mm. */
	atomic_ulong sched_in;
	/* Scheduled in without a barrier after the store to rq->curr. */
	atomic_ulong sched_in_missed;
	/* Checks where the target did not serialize its core. */
	atomic_ulong sync_core_missed;
	/* Scheduled in without a core serializing instruction. */
	atomic_ulong sync_core_in_missed;
};

struct shared {
	atomic_ulong started;
	atomic_ulong done;
	/* Calls issued by the caller, and their memory barriers. */
	atomic_ulong calls;
	atomic_ulong call_barriers;
	atomic_int ready;
	/* The process of the targets is done with its registration. */
	atomic_int registered;
	atomic_int stop;
	atomic_int error;
	/* Next free slot of counters. */
	atomic_uint next_slot;
	/* One more for the test case which adds a target of its own. */
	struct target_stats stats[MAX_TARGETS + 1];
};

struct target {
	int idx;
	/* Pin the target on this CPU, unless negative. */
	int cpu;
	/* Create a thread which exits, every so many checks. */
	bool thread_exit;
	/* Run for this duration. Run until the stop flag when 0. */
	int ms;
};

static struct shared *shared;
static int shared_fd;

/* The targets of this process also check the sync core guarantee. */
static bool check_sync_core;

static int sys_membarrier(int cmd, unsigned int flags, int cpu_id)
{
	return syscall(__NR_membarrier, cmd, flags, cpu_id);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

/* Store the allowed CPUs in @cpus. Return their count, up to @max. */
static int allowed_cpus(int *cpus, int max)
{
	cpu_set_t set;
	int cpu, nr = 0;

	if (sched_getaffinity(0, sizeof(set), &set))
		return 0;
	for (cpu = 0; cpu < CPU_SETSIZE && nr < max; cpu++)
		if (CPU_ISSET(cpu, &set))
			cpus[nr++] = cpu;
	return nr;
}

/* One target per allowed CPU. With the caller, the CPUs are overcommitted. */
static int nr_targets(void)
{
	int cpus[MAX_TARGETS];

	return allowed_cpus(cpus, MAX_TARGETS);
}

#define BPF_OBJECT	"membarrier_test_barrier.bpf.o"
#define COUNTERS_SIZE	(MBT_MAX_SLOTS * sizeof(struct mbt_counters))
#define OPTPROBES_FILE	"/proc/sys/debug/kprobes-optimization"

/* Kind of instruction to probe within a function. */
enum match {
	/* smp_mb(): lock addl $0, -4(%rsp) */
	MATCH_MB,
	/* Write to CR3. */
	MATCH_CR3,
	/* Instruction with a LOCK prefix. */
	MATCH_LOCK,
	/*
	 * SERIALIZE or IRET, as issued by sync_core(). An IRET cannot be
	 * probed: the instruction before it is probed instead.
	 */
	MATCH_SYNC_CORE,
};

struct site {
	/* A compiler suffix such as ".isra.0" is accepted after this name. */
	const char *symbol;
	enum match match;
	/* Mask of the counters to increment. */
	unsigned int counters;
	/* Count for the thread which is scheduled in, not for current. */
	bool next;
};

#define IPI		(1U << MBT_IPI)
#define SYSCALL		(1U << MBT_SYSCALL)
#define SWITCH_MM	(1U << MBT_SWITCH_MM)
#define SYNC_CORE	(1U << MBT_SYNC_CORE)
#define SYNC_CORE_SWITCH (1U << MBT_SYNC_CORE_SWITCH)

static const struct site sites[] = {
	{ "ipi_mb", MATCH_MB, IPI },
	{ "ipi_sync_core", MATCH_MB, IPI },
	{ "ipi_sync_core", MATCH_SYNC_CORE, SYNC_CORE },
	{ "ipi_rseq", MATCH_MB, IPI },
	{ "membarrier_global_expedited", MATCH_MB, SYSCALL },
	{ "membarrier_private_expedited", MATCH_MB, SYSCALL },
	/*
	 * Switch to another mm: write to CR3. It is a full barrier and
	 * it is core serializing.
	 */
	{ "switch_mm_irqs_off", MATCH_CR3, SWITCH_MM | SYNC_CORE_SWITCH, true },
	/* Switch to the mm which the CPU kept in lazy TLB mode: smp_mb(). */
	{ "switch_mm_irqs_off", MATCH_MB, SWITCH_MM, true },
	/*
	 * Switch from a kernel thread: mmdrop_lazy_tlb_sched(). Any other
	 * locked instruction of this function counts as well.
	 */
	{ "finish_task_switch", MATCH_LOCK, SWITCH_MM, true },
	/* membarrier_mm_sync_core_before_usermode() */
	{ "finish_task_switch", MATCH_SYNC_CORE, SYNC_CORE_SWITCH, true },
};

struct probe {
	const struct site *site;
	struct bpf_link *link;
	char symbol[64];
	unsigned long offset;
};

struct ksym {
	unsigned long addr;
	char name[64];
};

static struct probe probes[MBT_MAX_SITES];
static int nr_probes;
/* Probes of each site. */
static int site_probes[ARRAY_SIZE(sites)];
/* The setup failed, and not because the system lacks a feature. */
static bool setup_broken;
static bool too_many_probes;
static struct bpf_object *bpf_obj;
static struct bpf_program *site_prog;
static struct bpf_link *switch_link;
static const struct mbt_counters *all_counters;
static int register_fd = -1, counters_fd = -1, hits_fd = -1;
static int kcore_fd = -1;
static char optprobes_saved;
static pid_t setup_pid;

static int libbpf_quiet(enum libbpf_print_level level, const char *fmt,
			va_list args)
{
	return 0;
}

/* Read @len bytes of kernel memory at @addr from /proc/kcore. */
static bool read_kernel(unsigned long addr, void *buf, size_t len)
{
	Elf64_Ehdr ehdr;
	Elf64_Phdr phdr;
	int i;

	if (pread(kcore_fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr))
		return false;
	for (i = 0; i < ehdr.e_phnum; i++) {
		if (pread(kcore_fd, &phdr, sizeof(phdr),
			  ehdr.e_phoff + i * sizeof(phdr)) != sizeof(phdr))
			return false;
		if (phdr.p_type != PT_LOAD || addr < phdr.p_vaddr ||
		    addr + len > phdr.p_vaddr + phdr.p_memsz)
			continue;
		return pread(kcore_fd, buf, len,
			     phdr.p_offset + addr - phdr.p_vaddr) == (ssize_t)len;
	}
	return false;
}

/*
 * Probe the instruction at @addr. The kernel refuses an address which
 * is not the first byte of an instruction. Return false in that case.
 */
static bool probe_insn(const struct site *site, const struct ksym *sym,
		       unsigned long addr)
{
	LIBBPF_OPTS(bpf_kprobe_opts, opts);
	struct probe *probe = &probes[nr_probes];

	if (nr_probes == MBT_MAX_SITES) {
		too_many_probes = true;
		return false;
	}
	opts.offset = addr;
	opts.bpf_cookie = site->counters | (site->next ? MBT_COOKIE_NEXT : 0) |
			  (nr_probes << MBT_COOKIE_SITE_SHIFT);
	probe->link = bpf_program__attach_kprobe_opts(site_prog, NULL, &opts);
	if (!probe->link)
		return false;
	probe->site = site;
	site_probes[site - sites]++;
	probe->offset = addr - sym->addr;
	snprintf(probe->symbol, sizeof(probe->symbol), "%s", sym->name);
	nr_probes++;
	return true;
}

static const unsigned char smp_mb_insn[] = { 0xf0, 0x83, 0x44, 0x24, 0xfc, 0x00 };

/*
 * Find the instructions of a site by their bytes, in the @size bytes
 * of the function @sym. A match which is not an instruction is
 * rejected by probe_insn().
 */
static void scan_function(const struct site *site, const struct ksym *sym,
			  size_t size)
{
	unsigned char *text;
	size_t i, left;
	int32_t imm;

	text = malloc(size);
	if (!text || !read_kernel(sym->addr, text, size)) {
		free(text);
		return;
	}
	for (i = 0; i < size; i++) {
		unsigned long addr = sym->addr + i;
		unsigned char *insn = text + i;

		left = size - i;
		switch (site->match) {
		case MATCH_MB:
			if (left >= sizeof(smp_mb_insn) &&
			    !memcmp(insn, smp_mb_insn, sizeof(smp_mb_insn)))
				probe_insn(site, sym, addr);
			break;
		case MATCH_CR3:
			/* mov %reg, %cr3 is 0f 22 /3, after a REX prefix or not. */
			if (left < 3 || insn[0] != 0x0f || insn[1] != 0x22 ||
			    (insn[2] & 0xf8) != 0xd8)
				break;
			if (i && (insn[-1] & 0xf0) == 0x40 &&
			    probe_insn(site, sym, addr - 1))
				break;
			probe_insn(site, sym, addr);
			break;
		case MATCH_LOCK:
			if (insn[0] == 0xf0)
				probe_insn(site, sym, addr);
			break;
		case MATCH_SYNC_CORE:
			/* SERIALIZE is 0f 01 e8. */
			if (left >= 3 && insn[0] == 0x0f && insn[1] == 0x01 &&
			    insn[2] == 0xe8) {
				probe_insn(site, sym, addr);
				break;
			}
			/*
			 * iret_to_self() is "pushq $1f; iretq; 1:". Probe the
			 * push.
			 */
			if (left < 7 || insn[0] != 0x68 || insn[5] != 0x48 ||
			    insn[6] != 0xcf)
				break;
			memcpy(&imm, insn + 1, sizeof(imm));
			if (imm == (int32_t)(addr + 7))
				probe_insn(site, sym, addr);
			break;
		}
	}
	free(text);
}

static int ksym_cmp(const void *a, const void *b)
{
	const struct ksym *ka = a, *kb = b;

	return ka->addr < kb->addr ? -1 : ka->addr > kb->addr;
}

/*
 * Probe the instructions of each site. Return NULL, or the reason why
 * a site has no probe.
 */
static const char *probe_sites(void)
{
	struct ksym *syms = NULL;
	size_t nr = 0, max = 0, i, j, k;
	char line[256], type;
	FILE *f;

	f = fopen("/proc/kallsyms", "r");
	if (!f)
		return "cannot read /proc/kallsyms";
	while (fgets(line, sizeof(line), f)) {
		struct ksym sym;

		if (sscanf(line, "%lx %c %63s", &sym.addr, &type, sym.name) != 3)
			continue;
		if (!strchr("tTwW", type))
			continue;
		if (nr == max) {
			struct ksym *more;

			max = max ? 2 * max : 1 << 16;
			more = realloc(syms, max * sizeof(*syms));
			if (!more) {
				free(syms);
				fclose(f);
				setup_broken = true;
				return "out of memory";
			}
			syms = more;
		}
		syms[nr++] = sym;
	}
	fclose(f);
	qsort(syms, nr, sizeof(*syms), ksym_cmp);
	for (i = 0; i < nr; i++) {
		/* The function ends where the next symbol starts. */
		for (j = i + 1; j < nr && syms[j].addr == syms[i].addr; j++)
			;
		if (j == nr || !syms[i].addr)
			continue;
		for (k = 0; k < ARRAY_SIZE(sites); k++) {
			size_t len = strlen(sites[k].symbol);

			if (strncmp(syms[i].name, sites[k].symbol, len))
				continue;
			if (syms[i].name[len] && syms[i].name[len] != '.')
				continue;
			scan_function(&sites[k], &syms[i],
				      syms[j].addr - syms[i].addr);
		}
	}
	free(syms);
	if (too_many_probes)
		return "too many barrier instructions in this kernel";
	for (k = 0; k < ARRAY_SIZE(sites); k++) {
		static char reason[128];

		if (site_probes[k])
			continue;
		snprintf(reason, sizeof(reason),
			 "cannot probe the instructions of %s()",
			 sites[k].symbol);
		return reason;
	}
	return NULL;
}

/*
 * The kernel turns a kprobe into a jump when it can. The handler of
 * such a probe runs with interrupts enabled: the probe of an IPI
 * handler which interrupts it is skipped. So disable this optimization
 * while the test runs.
 *
 * Write @value to the sysctl file. Return false if this fails. A
 * kernel without the file does not optimize.
 */
static bool set_optprobes(char value)
{
	int fd = open(OPTPROBES_FILE, O_RDWR);
	bool ok;

	if (fd < 0)
		return errno == ENOENT;
	ok = optprobes_saved || pread(fd, &optprobes_saved, 1, 0) == 1;
	ok = ok && pwrite(fd, &value, 1, 0) == 1;
	close(fd);
	return ok;
}

/* Restore the sysctl when a signal ends the test. */
static void restore_optprobes(int sig)
{
	if (getpid() == setup_pid && optprobes_saved)
		set_optprobes(optprobes_saved);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void bpf_teardown(void);

/*
 * Load the BPF programs and probe the sites. Return NULL, or the error.
 * The error sets setup_broken unless the system lacks a feature.
 */
static const char *bpf_setup(void)
{
	struct bpf_program *prog;
	libbpf_print_fn_t print;
	struct bpf_map *map;
	const char *err;

	setup_pid = getpid();
	atexit(bpf_teardown);
	if (geteuid())
		return "this test needs root";
	if (access("/sys/kernel/btf/vmlinux", R_OK))
		return "the kernel has no BTF (CONFIG_DEBUG_INFO_BTF)";
	kcore_fd = open("/proc/kcore", O_RDONLY | O_CLOEXEC);
	if (kcore_fd < 0)
		return "cannot read /proc/kcore (CONFIG_PROC_KCORE)";
	setup_broken = true;
	bpf_obj = bpf_object__open_file(BPF_OBJECT, NULL);
	if (!bpf_obj)
		return "cannot open " BPF_OBJECT;
	if (bpf_object__load(bpf_obj))
		return "cannot load the BPF programs";
	site_prog = bpf_object__find_program_by_name(bpf_obj, "barrier_site");
	prog = bpf_object__find_program_by_name(bpf_obj, "register_thread");
	map = bpf_object__find_map_by_name(bpf_obj, "counters");
	if (!site_prog || !prog || !map)
		return "incomplete BPF object";
	register_fd = bpf_program__fd(prog);
	counters_fd = bpf_map__fd(map);
	map = bpf_object__find_map_by_name(bpf_obj, "site_hits");
	hits_fd = map ? bpf_map__fd(map) : -1;
	/* The exec test case passes these two to the new program. */
	if (fcntl(register_fd, F_SETFD, 0) || fcntl(counters_fd, F_SETFD, 0))
		return "fcntl failed";
	signal(SIGINT, restore_optprobes);
	signal(SIGTERM, restore_optprobes);
	signal(SIGHUP, restore_optprobes);
	if (!set_optprobes('0'))
		return "cannot write " OPTPROBES_FILE;
	prog = bpf_object__find_program_by_name(bpf_obj, "sched_switch");
	switch_link = prog ? bpf_program__attach(prog) : NULL;
	if (!switch_link)
		return "cannot attach to the sched_switch tracepoint";
	/* probe_insn() fails on purpose. Do not print these errors. */
	setup_broken = false;
	print = libbpf_set_print(libbpf_quiet);
	err = probe_sites();
	libbpf_set_print(print);
	return err;
}

/* Remove the probes. Print how many times each one was hit. */
static void bpf_teardown(void)
{
	struct bpf_prog_info prog_info = {};
	__u32 len = sizeof(prog_info);
	unsigned long long lost = 0;
	int i;

	/* Only the process which loaded the programs removes them. */
	if (getpid() != setup_pid)
		return;

	for (i = 0; i < nr_probes; i++) {
		struct bpf_link_info info = {};
		__u32 info_len = sizeof(info), key = i;
		__u64 hits = 0;

		bpf_link_get_info_by_fd(bpf_link__fd(probes[i].link), &info,
					&info_len);
		bpf_map_lookup_elem(hits_fd, &key, &hits);
		ksft_print_msg("probe %s+0x%lx counters 0x%x%s hits %llu missed %llu\n",
			       probes[i].symbol, probes[i].offset,
			       probes[i].site->counters,
			       probes[i].site->next ? " (next)" : "",
			       (unsigned long long)hits,
			       (unsigned long long)info.perf_event.kprobe.missed);
		lost += info.perf_event.kprobe.missed;
		bpf_link__destroy(probes[i].link);
	}
	if (site_prog &&
	    !bpf_prog_get_info_by_fd(bpf_program__fd(site_prog), &prog_info,
				     &len))
		lost += prog_info.recursion_misses;
	/* A lost hit looks like a missing barrier. */
	if (lost)
		ksft_print_msg("%llu probe hits were lost: a failure may be spurious\n",
			       lost);
	bpf_link__destroy(switch_link);
	if (optprobes_saved)
		set_optprobes(optprobes_saved);
}

static bool map_all_counters(void)
{
	void *p = mmap(NULL, COUNTERS_SIZE, PROT_READ, MAP_SHARED, counters_fd,
		       0);

	all_counters = p == MAP_FAILED ? NULL : p;
	return all_counters;
}

/* Give counters to the calling thread. Return them, or NULL. */
static const uint64_t *map_counters(void)
{
	struct mbt_register arg = {
		.slot = atomic_fetch_add(&shared->next_slot, 1),
	};
	LIBBPF_OPTS(bpf_test_run_opts, opts,
		    .ctx_in = &arg, .ctx_size_in = sizeof(arg));

	if (arg.slot >= MBT_MAX_SLOTS) {
		errno = ENOSPC;
		return NULL;
	}
	if (bpf_prog_test_run_opts(register_fd, &opts) || opts.retval) {
		if (!errno)
			errno = EIO;
		return NULL;
	}
	return (const uint64_t *)all_counters[arg.slot].c;
}

/*
 * Read a counter. The kernel updates it behind the back of the compiler.
 * The checks rely on the order of these reads with the other atomic
 * reads.
 */
static uint64_t counter(const uint64_t *counters, int idx)
{
	return __atomic_load_n(&counters[idx], __ATOMIC_SEQ_CST);
}

static void *thread_nop(void *arg)
{
	return NULL;
}

/* Report a failure of the test setup. */
static void setup_error(const char *what)
{
	fprintf(stderr, "# %s: %s\n", what, strerror(errno));
	atomic_store(&shared->error, 1);
}

/*
 * Run one check of a target. See the comment at the top of the file.
 * Return false if no call returned during the check.
 */
static bool check_call(const uint64_t *counters, struct target_stats *stats)
{
	unsigned long done = atomic_load(&shared->done);
	uint64_t ipi = counter(counters, MBT_IPI);
	uint64_t sched_out = counter(counters, MBT_SCHED_OUT);
	uint64_t sync_core = counter(counters, MBT_SYNC_CORE);
	unsigned long started = atomic_load(&shared->started);
	bool by_ipi, scheduled, serialized;
	int spins = 0;

	/* A call is in flight. */
	if (done != started)
		return false;
	while (atomic_load(&shared->done) == started) {
		if (++spins == MAX_SPINS || atomic_load(&shared->stop))
			return false;
	}
	by_ipi = counter(counters, MBT_IPI) != ipi;
	scheduled = counter(counters, MBT_SCHED_OUT) != sched_out;
	serialized = counter(counters, MBT_SYNC_CORE) != sync_core;
	if (scheduled) {
		atomic_fetch_add(&stats->by_sched, 1);
		return true;
	}
	/* The next call has started. */
	if (atomic_load(&shared->started) != started + 1)
		return true;
	if (by_ipi)
		atomic_fetch_add(&stats->by_ipi, 1);
	else
		atomic_fetch_add(&stats->missed, 1);
	if (check_sync_core && !serialized)
		atomic_fetch_add(&stats->sync_core_missed, 1);
	return true;
}

/* Run the checks of a target. */
static void run_target(const struct target *target)
{
	struct target_stats *stats = &shared->stats[target->idx];
	const uint64_t *counters;
	long deadline = now_ms() + target->ms;
	unsigned long checks = 0;
	uint64_t first_ipi, first_sched_in, first_sync_core_missed;
	uint64_t first_sched_out;

	if (target->cpu >= 0 && pin_to_cpu(target->cpu))
		setup_error("sched_setaffinity");
	counters = map_counters();
	if (!counters) {
		setup_error("register_thread");
		atomic_fetch_add(&shared->ready, 1);
		return;
	}
	atomic_fetch_add(&shared->ready, 1);
	while (!atomic_load(&shared->registered) && !atomic_load(&shared->stop))
		sched_yield();
	/*
	 * The scheduler serializes the core of a registered process
	 * only. The target may have been scheduled in before the
	 * registration. So wait until it is scheduled out, and count
	 * from there.
	 */
	first_sched_out = counter(counters, MBT_SCHED_OUT);
	while (check_sync_core &&
	       counter(counters, MBT_SCHED_OUT) == first_sched_out &&
	       !atomic_load(&shared->stop))
		usleep(1000);
	first_sync_core_missed = counter(counters, MBT_SYNC_CORE_IN_MISSED);
	first_ipi = counter(counters, MBT_IPI);
	first_sched_in = counter(counters, MBT_SCHED_IN);
	while (!atomic_load(&shared->stop)) {
		if (check_call(counters, stats) && target->thread_exit &&
		    !(++checks % 16)) {
			pthread_t thread;

			if (pthread_create(&thread, NULL, thread_nop, NULL) ||
			    pthread_join(thread, NULL))
				setup_error("thread exit");
		}
		if (target->ms && now_ms() >= deadline)
			break;
	}
	atomic_fetch_add(&stats->ipis, counter(counters, MBT_IPI) - first_ipi);
	atomic_fetch_add(&stats->sched_in,
			 counter(counters, MBT_SCHED_IN) - first_sched_in);
	atomic_fetch_add(&stats->sched_in_missed,
			 counter(counters, MBT_SCHED_IN_MISSED));
	if (check_sync_core)
		atomic_fetch_add(&stats->sync_core_in_missed,
				 counter(counters, MBT_SYNC_CORE_IN_MISSED) -
				 first_sync_core_missed);
}

static void *target_thread(void *arg)
{
	run_target(arg);
	return NULL;
}

/*
 * This task shares the TLS of its parent, errno included. It only makes
 * system calls and reads shared memory, unless it reports an error.
 */
static int target_clone(void *arg)
{
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	run_target(arg);
	return 0;
}

static struct target targets[MAX_TARGETS + 1];
static pthread_t threads[MAX_TARGETS + 1];
static bool created[MAX_TARGETS + 1];

/* Start @nr target threads, with the stats indexes from @first. */
static void start_targets(int first, int nr, bool thread_exit)
{
	int i;

	for (i = first; i < first + nr; i++) {
		targets[i] = (struct target) {
			.idx = i, .cpu = -1, .thread_exit = thread_exit,
		};
		created[i] = !pthread_create(&threads[i], NULL, target_thread,
					     &targets[i]);
		if (!created[i])
			setup_error("pthread_create");
	}
}

static void join_targets(int first, int nr)
{
	int i;

	for (i = first; i < first + nr; i++)
		if (created[i] && pthread_join(threads[i], NULL))
			setup_error("pthread_join");
}

/*
 * Wait until @nr targets run and their process is done with its
 * registration. Return false when this does not happen.
 */
static bool wait_ready(int nr)
{
	time_t start = time(NULL);

	while (atomic_load(&shared->ready) < nr ||
	       !atomic_load(&shared->registered)) {
		if (time(NULL) - start > READY_TIMEOUT)
			return false;
		sched_yield();
	}
	return true;
}

/*
 * Issue the command for @ms milliseconds. Count the calls and the
 * memory barriers which the system call executed. Return 0, or -1
 * with errno set.
 */
static int run_caller(int cmd, unsigned int flags, int cpu_id, int ms)
{
	static const uint64_t *counters;
	long deadline = now_ms() + ms;
	unsigned long calls = 0;
	uint64_t barriers;
	long long pause_end;
	int i;

	if (!counters)
		counters = map_counters();
	if (!counters)
		return -1;
	barriers = counter(counters, MBT_SYSCALL);
	do {
		for (i = 0; i < 64; i++) {
			atomic_fetch_add(&shared->started, 1);
			if (sys_membarrier(cmd, flags, cpu_id))
				return -1;
			atomic_fetch_add(&shared->done, 1);
			calls++;
			/* Let the targets see that no call is in flight. */
			pause_end = now_ns() + PAUSE_NS;
			while (now_ns() < pause_end)
				;
		}
	} while (now_ms() < deadline);
	atomic_fetch_add(&shared->calls, calls);
	atomic_fetch_add(&shared->call_barriers,
			 counter(counters, MBT_SYSCALL) - barriers);
	return 0;
}

/* Wait for a child. Return true if it exited with 0. */
static bool child_ok(pid_t pid)
{
	int status;

	return waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	       !WEXITSTATUS(status);
}

static void sum_stats(int first, int nr, struct target_stats *sum)
{
	int i;

	memset(sum, 0, sizeof(*sum));
	for (i = first; i < first + nr; i++) {
		struct target_stats *stats = &shared->stats[i];

		sum->by_ipi += atomic_load(&stats->by_ipi);
		sum->by_sched += atomic_load(&stats->by_sched);
		sum->missed += atomic_load(&stats->missed);
		sum->ipis += atomic_load(&stats->ipis);
		sum->sched_in += atomic_load(&stats->sched_in);
		sum->sched_in_missed += atomic_load(&stats->sched_in_missed);
		sum->sync_core_missed += atomic_load(&stats->sync_core_missed);
		sum->sync_core_in_missed +=
			atomic_load(&stats->sync_core_in_missed);
	}
}

/* The command must cover each check of these targets. */
#define EXPECT_COVERED(first, nr) do {					\
	struct target_stats __sum;					\
									\
	sum_stats(first, nr, &__sum);					\
	TH_LOG("exact checks: %lu, dropped as scheduled out: %lu, IPIs: %lu",\
	       __sum.by_ipi, __sum.by_sched, __sum.ipis);		\
	EXPECT_EQ(__sum.missed, 0);					\
	EXPECT_EQ(__sum.sched_in_missed, 0);				\
	EXPECT_EQ(__sum.sync_core_missed, 0);				\
	EXPECT_EQ(__sum.sync_core_in_missed, 0);			\
	EXPECT_GT(__sum.by_ipi + __sum.by_sched + __sum.missed, 0);			\
} while (0)

/*
 * The command must not interrupt these targets. A CPU can still
 * receive the IPI just after it switched from a targeted thread to one
 * of these. So allow one IPI each time a target is scheduled in.
 */
#define EXPECT_NO_IPI(first, nr) do {					\
	struct target_stats __sum;					\
									\
	sum_stats(first, nr, &__sum);					\
	TH_LOG("IPIs: %lu, scheduled in: %lu", __sum.ipis,		\
	       __sum.sched_in);						\
	EXPECT_LE(__sum.ipis, __sum.sched_in);				\
	EXPECT_EQ(__sum.sched_in_missed, 0);				\
} while (0)

/*
 * The system call executes a memory barrier on entry and one on exit.
 * It returns early when a single CPU is online.
 */
#define EXPECT_CALL_BARRIERS() do {					\
	unsigned long __calls = atomic_load(&shared->calls);		\
									\
	EXPECT_EQ(atomic_load(&shared->error), 0);			\
	if (sysconf(_SC_NPROCESSORS_ONLN) > 1) {			\
		EXPECT_GE(atomic_load(&shared->call_barriers),		\
			  2 * __calls);					\
	}								\
} while (0)

static bool has_cmd(int cmd)
{
	return sys_membarrier(MEMBARRIER_CMD_QUERY, 0, 0) & cmd;
}

static void reset_shared(void)
{
	memset(shared, 0, sizeof(*shared));
}

/*
 * Private expedited test cases.
 *
 * The caller is the main thread. The targets are threads of the same
 * process.
 */

static void test_private(struct __test_metadata *_metadata,
			 int register_cmd, int cmd)
{
	int nr = nr_targets();

	if (!has_cmd(cmd))
		SKIP(return, "the command is not available");
	reset_shared();
	start_targets(0, nr, false);
	ASSERT_EQ(sys_membarrier(register_cmd, 0, 0), 0);
	atomic_store(&shared->registered, 1);
	ASSERT_TRUE(wait_ready(nr));
	EXPECT_EQ(run_caller(cmd, 0, 0, RUN_MS), 0);
	atomic_store(&shared->stop, 1);
	join_targets(0, nr);
	EXPECT_CALL_BARRIERS();
	EXPECT_COVERED(0, nr);
}

TEST(private_expedited)
{
	test_private(_metadata, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
		     MEMBARRIER_CMD_PRIVATE_EXPEDITED);
}

/*
 * With page table isolation, the kernel serializes the core with the
 * write to CR3 of each return to user-space. This write is in the entry
 * code, which cannot be probed.
 */
static bool page_table_isolation(void)
{
	char *line = NULL;
	bool pti = false;
	size_t len = 0;
	FILE *f;

	f = fopen("/proc/cpuinfo", "r");
	if (!f)
		return false;
	while (!pti && getline(&line, &len, f) >= 0)
		pti = !strncmp(line, "flags", 5) &&
		      (strstr(line, " pti ") || strstr(line, " pti\n"));
	free(line);
	fclose(f);
	return pti;
}

TEST(private_expedited_sync_core)
{
	check_sync_core = !page_table_isolation();
	if (!check_sync_core)
		TH_LOG("page table isolation: core serialization not checked");
	test_private(_metadata,
		     MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE,
		     MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE);
}

TEST(private_expedited_rseq)
{
	test_private(_metadata, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ,
		     MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ);
}

/* MEMBARRIER_CMD_FLAG_CPU interrupts only the CPU it names. */
TEST(private_expedited_rseq_cpu)
{
	int cpus[3], i;

	if (!has_cmd(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ))
		SKIP(return, "the command is not available");
	if (allowed_cpus(cpus, 3) < 3)
		SKIP(return, "this test needs 3 allowed CPUs");
	reset_shared();
	ASSERT_EQ(pin_to_cpu(cpus[0]), 0);
	for (i = 0; i < 2; i++) {
		targets[i] = (struct target) { .idx = i, .cpu = cpus[i + 1] };
		ASSERT_EQ(pthread_create(&threads[i], NULL, target_thread,
					 &targets[i]), 0);
		created[i] = true;
	}
	ASSERT_EQ(sys_membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ,
				 0, 0), 0);
	atomic_store(&shared->registered, 1);
	ASSERT_TRUE(wait_ready(2));
	EXPECT_EQ(run_caller(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
			     MEMBARRIER_CMD_FLAG_CPU, cpus[1], RUN_MS), 0);
	atomic_store(&shared->stop, 1);
	join_targets(0, 2);
	EXPECT_CALL_BARRIERS();
	EXPECT_COVERED(0, 1);
	EXPECT_NO_IPI(1, 1);
}

/* A private command does not interrupt another registered process. */
TEST(private_expedited_other_process)
{
	int nr = nr_targets();
	pid_t pid;

	if (!has_cmd(MEMBARRIER_CMD_PRIVATE_EXPEDITED))
		SKIP(return, "the command is not available");
	reset_shared();
	pid = fork();
	ASSERT_GE(pid, 0);
	if (!pid) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (sys_membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
				   0, 0))
			_exit(1);
		start_targets(0, nr, false);
		join_targets(0, nr);
		_exit(0);
	}
	start_targets(nr, 1, false);
	ASSERT_EQ(sys_membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
				 0, 0), 0);
	atomic_store(&shared->registered, 1);
	ASSERT_TRUE(wait_ready(nr + 1));
	EXPECT_EQ(run_caller(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0, RUN_MS),
		  0);
	atomic_store(&shared->stop, 1);
	join_targets(nr, 1);
	EXPECT_TRUE(child_ok(pid));
	EXPECT_CALL_BARRIERS();
	EXPECT_NO_IPI(0, nr);
	EXPECT_COVERED(nr, 1);
}

/*
 * A private command covers every task which uses the mm. This includes
 * a task of another thread group created with CLONE_VM.
 */
TEST(private_expedited_clone_vm)
{
	char *stack;
	pid_t pid;

	if (!has_cmd(MEMBARRIER_CMD_PRIVATE_EXPEDITED))
		SKIP(return, "the command is not available");
	reset_shared();
	stack = malloc(STACK_SIZE);
	ASSERT_NE(stack, NULL);
	targets[0] = (struct target) { .idx = 0, .cpu = -1 };
	pid = clone(target_clone, stack + STACK_SIZE, CLONE_VM | SIGCHLD,
		    &targets[0]);
	ASSERT_GE(pid, 0);
	ASSERT_EQ(sys_membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
				 0, 0), 0);
	atomic_store(&shared->registered, 1);
	ASSERT_TRUE(wait_ready(1));
	EXPECT_EQ(run_caller(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0, RUN_MS),
		  0);
	atomic_store(&shared->stop, 1);
	EXPECT_TRUE(child_ok(pid));
	free(stack);
	EXPECT_CALL_BARRIERS();
	EXPECT_COVERED(0, 1);
}

/*
 * Global expedited test cases.
 *
 * The caller is the test process. The targets are threads of a child
 * process, which runs one of the setups below.
 */

enum global_setup {
	/* Register, then create the target threads. */
	SETUP_REGISTERED,
	/* Do not register. */
	SETUP_UNREGISTERED,
	/* Create the target threads, then register. */
	SETUP_MULTI_THREAD,
	/*
	 * Each target creates threads which exit. An exiting thread
	 * clears the runqueue membarrier state in exit_mm(). The
	 * scheduler then switches to a thread which has the same mm.
	 */
	SETUP_THREAD_EXIT,
	/* A forked child inherits the registration. */
	SETUP_FORK,
	/* exec() drops the registration. */
	SETUP_EXEC,
};

static int register_global(void)
{
	return sys_membarrier(MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED, 0, 0);
}

static void run_targets(int nr, bool thread_exit)
{
	start_targets(0, nr, thread_exit);
	join_targets(0, nr);
}

static void global_child(enum global_setup setup, int nr)
{
	char fd_str[16], nr_str[16], reg_str[16], cnt_str[16];
	pid_t pid;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
	switch (setup) {
	case SETUP_REGISTERED:
	case SETUP_THREAD_EXIT:
		if (register_global())
			_exit(1);
		atomic_store(&shared->registered, 1);
		run_targets(nr, setup == SETUP_THREAD_EXIT);
		break;
	case SETUP_UNREGISTERED:
		atomic_store(&shared->registered, 1);
		run_targets(nr, false);
		break;
	case SETUP_MULTI_THREAD:
		start_targets(0, nr, false);
		while (atomic_load(&shared->ready) < nr)
			sched_yield();
		if (register_global())
			_exit(1);
		atomic_store(&shared->registered, 1);
		join_targets(0, nr);
		break;
	case SETUP_FORK:
		if (register_global())
			_exit(1);
		atomic_store(&shared->registered, 1);
		pid = fork();
		if (pid < 0)
			_exit(1);
		if (!pid) {
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			run_targets(nr, false);
			_exit(0);
		}
		_exit(child_ok(pid) ? 0 : 1);
	case SETUP_EXEC:
		if (register_global())
			_exit(1);
		atomic_store(&shared->registered, 1);
		snprintf(fd_str, sizeof(fd_str), "%d", shared_fd);
		snprintf(nr_str, sizeof(nr_str), "%d", nr);
		snprintf(reg_str, sizeof(reg_str), "%d", register_fd);
		snprintf(cnt_str, sizeof(cnt_str), "%d", counters_fd);
		execl("/proc/self/exe", "membarrier_test_barrier",
		      "--targets", fd_str, nr_str, reg_str, cnt_str, NULL);
		_exit(1);
	}
	_exit(0);
}

static void test_global(struct __test_metadata *_metadata,
			enum global_setup setup, bool covered)
{
	int nr = nr_targets();
	pid_t pid;

	if (!has_cmd(MEMBARRIER_CMD_GLOBAL_EXPEDITED))
		SKIP(return, "the command is not available");
	reset_shared();
	pid = fork();
	ASSERT_GE(pid, 0);
	if (!pid)
		global_child(setup, nr);
	ASSERT_TRUE(wait_ready(nr));
	EXPECT_EQ(run_caller(MEMBARRIER_CMD_GLOBAL_EXPEDITED, 0, 0, RUN_MS),
		  0);
	atomic_store(&shared->stop, 1);
	EXPECT_TRUE(child_ok(pid));
	EXPECT_CALL_BARRIERS();
	if (covered)
		EXPECT_COVERED(0, nr);
	else
		EXPECT_NO_IPI(0, nr);
}

TEST(global_expedited_registered)
{
	test_global(_metadata, SETUP_REGISTERED, true);
}

TEST(global_expedited_unregistered)
{
	test_global(_metadata, SETUP_UNREGISTERED, false);
}

TEST(global_expedited_multi_thread)
{
	test_global(_metadata, SETUP_MULTI_THREAD, true);
}

TEST(global_expedited_thread_exit)
{
	test_global(_metadata, SETUP_THREAD_EXIT, true);
}

TEST(global_expedited_fork)
{
	test_global(_metadata, SETUP_FORK, true);
}

TEST(global_expedited_exec)
{
	test_global(_metadata, SETUP_EXEC, false);
}

/*
 * Each round starts a process on a first CPU. The process registers on
 * a second CPU. Meanwhile the first CPU is idle and its idle thread
 * borrows the mm of the process. The process then comes back to the
 * first CPU: the scheduler switches from the idle thread to it.
 */
TEST(global_expedited_lazy_tlb)
{
	long deadline;
	int cpus[3];
	pid_t pid;

	if (!has_cmd(MEMBARRIER_CMD_GLOBAL_EXPEDITED))
		SKIP(return, "the command is not available");
	if (allowed_cpus(cpus, 3) < 3)
		SKIP(return, "this test needs 3 allowed CPUs");
	reset_shared();
	ASSERT_EQ(pin_to_cpu(cpus[0]), 0);
	deadline = now_ms() + RUN_MS;
	do {
		struct target target = {
			.idx = 0, .cpu = cpus[1], .ms = ROUND_MS,
		};

		atomic_store(&shared->ready, 0);
		atomic_store(&shared->registered, 0);
		pid = fork();
		ASSERT_GE(pid, 0);
		if (!pid) {
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			if (pin_to_cpu(cpus[1]) || pin_to_cpu(cpus[2]) ||
			    register_global())
				_exit(1);
			atomic_store(&shared->registered, 1);
			run_target(&target);
			_exit(0);
		}
		ASSERT_TRUE(wait_ready(1));
		EXPECT_EQ(run_caller(MEMBARRIER_CMD_GLOBAL_EXPEDITED, 0, 0,
				     ROUND_MS), 0);
		EXPECT_TRUE(child_ok(pid));
	} while (now_ms() < deadline);
	EXPECT_CALL_BARRIERS();
	EXPECT_COVERED(0, 1);
}

static struct shared *map_shared(int fd)
{
	void *p;

	p = mmap(NULL, sizeof(struct shared), PROT_READ | PROT_WRITE,
		 MAP_SHARED, fd, 0);
	return p == MAP_FAILED ? NULL : p;
}

int main(int argc, char **argv)
{
	const char *err;

	/* New program image of the exec test case. See global_child(). */
	if (argc == 6 && !strcmp(argv[1], "--targets")) {
		shared = map_shared(atoi(argv[2]));
		register_fd = atoi(argv[4]);
		counters_fd = atoi(argv[5]);
		if (!shared || !map_all_counters())
			return 1;
		run_targets(atoi(argv[3]), false);
		return 0;
	}

	if (sys_membarrier(MEMBARRIER_CMD_QUERY, 0, 0) < 0)
		ksft_exit_skip("sys membarrier (CONFIG_MEMBARRIER) is disabled.\n");
#ifndef __x86_64__
	ksft_exit_skip("The barrier instructions are only listed for x86-64.\n");
#endif
	err = bpf_setup();
	if (!err && !map_all_counters()) {
		err = "cannot map the counters";
		setup_broken = true;
	}
	if (err && setup_broken)
		ksft_exit_fail_msg("%s\n", err);
	if (err)
		ksft_exit_skip("%s\n", err);
	shared_fd = memfd_create("membarrier_test_barrier", 0);
	if (shared_fd < 0 || ftruncate(shared_fd, sizeof(struct shared)))
		ksft_exit_fail_msg("memfd_create failed\n");
	shared = map_shared(shared_fd);
	if (!shared)
		ksft_exit_fail_msg("mmap failed\n");
	/* This exits. bpf_teardown() then runs. */
	return test_harness_run(argc, argv);
}
