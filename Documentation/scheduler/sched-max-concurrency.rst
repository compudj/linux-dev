.. SPDX-License-Identifier: GPL-2.0

.. _sched-max-concurrency:

====================================================
Per-cgroup limit on concurrently used CPUs (design)
====================================================

This document records the design of the ``cpu.concurrency.max`` cgroup
interface and of the scheduler machinery that enforces it. It is written
for reviewers and for whoever picks the work up later; it states the
decisions taken, why they were taken, and which simplifications this
version makes relative to the full design.

Motivation
==========

Userspace increasingly keeps per-CPU data structures: memory allocators,
counters, lock-free rings, restartable sequences (rseq) users. Sizing those
structures by the number of possible CPUs wastes memory on large machines
and gives containers no useful bound: a container limited by ``cpu.max``
to the equivalent of two CPUs may still run on every CPU of the machine
over time, since the quota bounds time, not placement.

Restartable sequences solve part of this with per-mm concurrency IDs
(``mm_cid``): a process is handed IDs in the range ``[0, nr_threads)``
clamped by the number of CPUs it may run on, so a process can size
per-"CPU" data by its own concurrency. That bound is still the affinity
mask. A container that wants to bound concurrency today has to use
``cpuset.cpus``, which pins the workload to named CPUs: topology-bound,
fragile under hotplug, and a poor fit for cloud workloads that are moved
between machines and that should let the scheduler pick the CPUs.

``cpu.concurrency.max`` adds a topology-agnostic bound: the number of CPUs
on which tasks of a cgroup may be present at any instant. The scheduler
remains free to choose *which* CPUs. The bound is then available to
userspace: it caps ``mm_cid`` for processes inside the cgroup, and it
backs the cross-process concurrency ID (see `Cross-process concurrency
IDs`_).

The idea and the early investigation were presented at LPC 2024 and FOSDEM
2025 (slides: "Per-CPU memory in containers", LPC 2024; "A new cgroup
cpu.concurrency.max controller interface file", FOSDEM 2025).

Interface
=========

``cpu.concurrency.max``
    Read-write single value file, non-root cgroups only, ``max`` by
    default. The value *requests* that tasks of this cgroup, including
    descendants, be present on at most that many CPUs at any time;
    ``max`` removes the request. Writes are synchronous: when the write
    returns, the requested value has been recorded, the effective values
    recomputed, and surplus CPUs evicted as far as the configuration
    allows. A write of a limit above the effective ceiling fails with
    ``EINVAL``.

``cpu.concurrency.ceiling``
    Read-only single value file, non-root cgroups only: the ceiling the
    limit may ever be raised to, ``max`` for none.

    The ceiling never changes: a cgroup takes it when it is created,
    from its parent's ``cpu.concurrency.child_ceiling``. A process that
    reads a value and sizes data by it, alone or with others in shared
    memory, must never see it grow, which would overflow the data, nor
    shrink, which would let a reader miss IDs that held data before. That
    is what lets an IPC namespace freeze a ceiling of its ``ns_cid`` on
    it (see `Cross-process concurrency IDs`_), and a process that pins
    itself to its cpu cgroup size its ``mm_cid`` data by it (see `Pinned
    processes`_); otherwise the bound of ``mm_cid`` follows the cgroup a
    process is moved to, so data indexed by ``mm_cid`` is sized by the
    number of possible CPUs.

``cpu.concurrency.child_ceiling``
    Read-write file on every cgroup, the root included, holding the ceiling
    value of the children created from then on, ``max`` by default. A
    child takes it when it is created, or when the cpu controller is
    enabled on it, and keeps it; a write changes nothing for the
    children that exist. A value is set before the cgroup is created, so
    that it is there before any task or namespace is: a container
    runtime that creates the namespaces of a container before writing
    its limits still finds it. Two managers creating children of one
    cgroup each create an intermediate cgroup of their own to set the
    value in. The file is delegatable, listed in
    ``/sys/kernel/cgroup/delegate``, as it distributes the cgroup's own
    ceiling among its children.

``cpu.concurrency.effective``
    Read-only file, non-root cgroups only, holding ``limit ceiling`` as the
    kernel enforces them: the limit in force (see `Effective value`_)
    and the effective ceiling, the smallest ceiling on the path to
    the root. Either is ``max`` when nothing applies.
    ``cgroup_file_notify()`` is raised whenever a value changes so a
    manager can poll it.

A limit is a property of configuration. The invariant the kernel keeps at
every level is::

    width <= effective limit <= effective ceiling

where ``width`` is the width of the affinity masks of the subtree, the
largest number of them no two of which are nested (see `Affinity masks
and width`_). The error semantics follow from which side each change
comes from:

* Writing a limit never fails because of task placement or because
  of the width of the tasks' masks (only a malformed value, ``0`` or a
  limit above the ceiling one fail, with ``EINVAL``, and a limit that
  would start over requests cpuset cuts with ``EBUSY``, see below). The
  request is recorded at
  once, so that from then on no tenant can raise the width above it;
  when the masks already present exceed it, the effective limit is
  *relaxed* to the width until the tasks unpin or leave, and then drops
  to the request by itself. Were the write to fail instead, a user of the
  container could pin threads so as to prevent the administrator from
  ever lowering the limit.
* A ceiling is never written over tasks: a cgroup created with one
  has none yet, and the masks of the tasks that join it are judged by it
  as migrations, refused with ``EBUSY`` beyond it, which is also how the
  tasks of a cgroup are judged when the cpu controller enabled on it
  gives it a ceiling. A ceiling the kernel could not hold would be no
  ceiling.
* ``sched_setaffinity(2)`` fails with ``EINVAL`` when the mask that would
  apply (the requested one within the cpuset) would raise the width
  above the *requested* limit of any ancestor. This is deterministic
  (it depends on configuration only, never on placement or on which CPUs
  are online) and it belongs to the error class the call already
  documents for masks that do not intersect the cpuset.
* Writes to ``cpuset.cpus``, ``cpuset.cpus.exclusive`` and
  ``cpuset.cpus.partition``, and cgroup migrations (writes to
  ``cgroup.procs`` or ``cgroup.threads``, moves between cpusets under one
  cpu cgroup included) and clone3(2) with ``CLONE_INTO_CGROUP``, whose
  child is judged as a task moving there, fail with ``EBUSY`` when they
  would raise the
  width above the *effective* limit of any cgroup they touch. A
  cpuset write is judged by the masks it would leave the tasks of every
  cpuset it changes with: those below the cpuset written and, when it
  moves CPUs between partitions, those beside it (see `Affinity masks
  and width`_). Writes to cpusets therefore never move the limit;
  only the cgroup's own file, and its ancestors', can. A cpuset that
  narrows the CPUs available never changes the effective value either
  (see `Effective value`_): it only compacts the IDs handed out. The
  partition changes the kernel cannot judge that way (the change of a
  partition with partitions below it, mainly) fail with ``EBUSY`` while
  a limited subtree has a cpuset of its own or a task of one lives in a
  cpuset: partitions nested in partitions are set up before such limits.
* Under a limit, cpusets and affinities are taken as they are, never cut
  or replaced: writes to the cpuset files and migrations that would
  leave a cpuset of a limited subtree granting other than what it names,
  or cut the requested mask of a task of one, fail with ``EBUSY``;
  ``sched_setaffinity(2)`` of such a mask fails with ``EINVAL``; a limit
  cannot start over such a cpuset or task (``EBUSY``). See `Requests
  taken as they are`_.
* Exits, migrations out of a subtree, CPU hotplug and the removal of a
  partition root's cgroup are not refused, and need not be: none of them
  can raise the width (see `Affinity masks and width`_ and `Requests
  taken as they are`_).

The decrease policy is therefore "write, then read back": a manager
writes the desired limit, reads the effective file, and if the two
differ it fixes the affinities of the offending tasks (narrowing a
cpuset, moving or stopping tasks) until they match, which happens on its
own at that change. Rewriting the same value is the explicit "retry now"
lever, although the kernel retries on its own on the events listed in
`Retry events`_.

Effective value
---------------

Let ``request(cg)`` be the written limit (``max`` treated as
infinity), ``ceiling(cg)`` the ceiling it was created with and
``width(cg)`` the width of the affinity masks present in the subtree
rooted at ``cg``.
Then::

    ceil_eff(cg)  = min(ceiling(cg), ceil_eff(parent(cg)))
    effective(cg) = max(min(request(cg), effective(parent(cg))), width(cg))

with ``width(cg) <= ceil_eff(cg)`` maintained by the refusals listed
under `Interface`_, so that ``effective(cg) <= ceil_eff(cg)`` follows
(the kernel clamps it to ``ceil_eff(cg)`` all the same).
Both values are hierarchical: a child can never be allowed more than its
parent. The effective value does not fold in the cpuset size: a cgroup
limited to 8 inside a cpuset of 4 reports 8, because the limit is about
what the controller enforces, not about what the cpuset happens to
provide. The ``width`` term is what makes relaxation explicit: a limit
value written below the width cannot be guaranteed without breaking an
affinity, so the kernel enforces the width instead and reports it,
until the tasks unpin or leave. Since every other way of raising the
width above the effective limit is refused, and nothing the kernel
cannot refuse raises it, that is the only relaxation there is.

After an eviction that could not complete (a "stalled shrink"), the
effective file reports the size actually enforced, that is the number of
CPUs still in use, until the next retry succeeds.

Affinity masks and width
------------------------

Every task takes part through the mask it is *configured* to run on:
its cpuset's *configured CPUs*, the CPUs it would grant with every CPU
online (``cpuset_task_cpus_configured_rcu()``), masked by its requested
mask when they intersect, as a cpuset change masks it. The configured
CPUs follow two rules. No hotplug event changes them, or a CPU coming
back could raise the width behind the admission's back. And their online
part holds only CPUs the cpuset's tasks can run on, whatever is online:
a task counted on a CPU it cannot use has its class hit by a slot it
cannot take, so no headroom is kept for the CPUs it can, and it waits for
as long as that slot is busy.

Cpuset keeps them per cpuset (``conc.cpus``), recomputed from what was
configured rather than from the effective masks (``cpuset_conc_refresh()``),
since hotplug moves CPUs between partitions:

* a member gets its ``cpuset.cpus`` within its parent's configured CPUs,
  or these whole when that leaves nothing, as cgroup v2 inherits;
* a partition root that can take CPUs keeps what it claims (its
  ``effective_xcpus``, which keep offline CPUs, or what
  ``compute_excpus()`` would grant it while it is invalid), but what
  another claim of its domain overlaps, within its ``cpuset.cpus`` when
  they intersect (where its tasks run while it is invalid), and less what
  its own partitions claim;
* the top cpuset gets every possible CPU but the claims of its partition
  children and of the remote partitions.

A partition keeps its claim while hotplug invalidates it: cpuset
invalidates a partition whose CPUs all go offline while it has tasks, or
whose parent would be left with none, and returns its CPUs to its parent
until they come back. Counting them anywhere else meanwhile would put
them, once back, in the masks of tasks that cannot run on them; leaving
them out of the masks of the cpusets that may not get them only narrows
those masks, which costs refusals, never the reverse. A write to
``cpuset.cpus``, ``cpuset.cpus.exclusive`` or the partition state
recomputes them, and the tasks of every cpuset whose configured CPUs
changed are registered again (``cpuset_conc_reregister()``), even when
their effective CPUs did not change (an offline CPU added to ``cpuset.cpus``);
a change of a task's requested mask registers that task again. A mask
that restricts nothing (every possible CPU) adds no constraint and is
not kept. A fallback widening drops the requested mask
(``set_cpus_allowed_force()`` clears ``user_cpus_ptr``), and the task's
class with it: its mask restricts nothing until it is set again. A
change refused once its class was registered ahead (a mask with no
valid CPU, its only CPU going offline) registers the mask the task keeps
again (``conc_task_affinity_changed()``): the stale class would count in
the width, and keep headroom for CPUs the task cannot run on, which the
task itself, held, could wait for.

A write to these files is judged before it is applied
(``cpuset_conc_judge()``): the configured CPUs of every cpuset are
computed as the write would leave them, into a second copy
(``conc_trial``), and every counting group with a task in a cpuset whose
configured CPUs would change is counted with them
(``conc_cpuset_check()``). The tasks are looked for in the cpusets below
the one written too, since a cpuset with cpusets below it holds none of
their tasks. A partition root claims what ``compute_excpus()`` grants it,
valid or not, which depends on what was configured and on what its
siblings hold exclusive, so a write changes the claims of the cpuset
written and of its invalid sibling partitions. Whether the cpuset
written ends valid matters only to those siblings that have no exclusive
CPUs, when it has none either: both outcomes are tried then. The write
cannot be judged that way, and fails with ``EBUSY`` while a limited
subtree has a cpuset of its own or a task of one lives in a cpuset, when
cpuset's walks could change
exclusive CPUs that claims depend on:

* a partition root lies below the cpuset written: its validity and its
  claim follow the write in ways only cpuset's partition engine works
  out;
* a valid remote partition root without exclusive CPUs of its own sits
  beside the cpuset written, or claims what ``compute_excpus()`` no
  longer grants it (a sibling changed since): a walk reaching it
  recomputes its claim;
* an invalid partition root still holds the exclusive CPUs it had when
  hotplug invalidated it, kept for when its CPUs come back, beside the
  cpuset written or beside an invalid partition root without exclusive
  CPUs: a walk reaching it drops them.

Under ``CONFIG_SCHED_MAX_CONCURRENCY_DEBUG``, every write judged by trial
is cross-checked once applied (``cpuset_conc_verify()``): the configured
CPUs must be those the trial computed, or a warning names a cpuset that
differs.

A limit ``L`` is feasible when some ``L`` CPUs hit every mask, the
smallest such number being the minimum hitting set of the masks. That is
not what the kernel admits by. Admission must also hold across the
events it cannot refuse: a task exits or leaves the subtree (a mask goes
away), a CPU goes offline (every mask loses that CPU, and a mask left
with none of its CPUs drops out as its tasks are widened), a CPU comes
back. And placement keeps one free slot per *class*, a minimal mask that
no used CPU hits yet (see below), so the number of classes, not the
hitting set, is what must stay within the limit. Both requirements lead
to the same measure:

* The classes of whatever set of tasks the subtree is left with are
  pairwise un-nested masks of the subtree. Exits can leave exactly a
  largest set of pairwise un-nested masks; so the worst the classes can
  reach by exits is the *width*, the size of such a largest set.
* A CPU going offline removes it from every mask alike, which keeps
  nested masks nested: two masks un-nested afterwards were un-nested
  before, so hotplug never raises the width. Since the masks are judged
  as configured, a CPU coming back changes nothing either.

The kernel therefore admits a change when the width stays within the
limit, and the classes then stay within it whatever follows. The width
is computed from the distinct masks by Dilworth's theorem, as their
number minus a maximum matching of strict inclusion (``conc_masks_width()``,
one augmenting-path search per mask). Once the width is within ``L``,
the masks split into ``L`` chains of at most one mask per CPU each, so
there are at most ``L`` times the CPU count of them: the work is bounded
by the limit, not by the tenant.

The limitation, documented to users: masks that overlap without being
nested count apart even when fewer CPUs would serve them all, whatever
exits or goes offline. Under a limit of 2, tasks pinned to {1,2} and to
{2,3} are admitted, but a third pinned to {1,3} is refused: the three
masks are pairwise un-nested, a width of 3, although any two of CPUs 1,
2 and 3 hit all three, and two CPUs still do with any of them offline.
Sliding windows are the costliest shape: thirteen windows of four
consecutive CPUs on 16 CPUs have a width of 13, while 4 CPUs hit them
all and 6 always do, whatever goes offline. Not every refusal of the
width is such a loss. Under a limit of 1, a task pinned to {1} and
another to {1,2} are admitted, and a third pinned to {1,3} is refused;
CPU 1 serves all three, but only while it is online: with CPU 1 offline,
{1,2} and {1,3} need two CPUs, and no criterion that holds across
hotplug admits the third task. Masks derived from the topology (SMT
siblings, cores, caches, nodes, "all but CPU 0") are nested or disjoint;
for them the width, the number of classes and the minimum hitting set
are equal, and nothing is refused that a hitting set would admit. The
exact condition for a limit to survive every exit and hotplug sequence
is a *maximum induced matching* of the CPU-mask incidence graph (``L+1``
CPUs each of which is the only one of them in some mask), NP-ceiling in
general; admitting by it would also need placement driven by a hitting
set rather than by classes. A simulation of random event streams found
the width refusing, on masks that overlap, a few percent of the affinity
changes that criterion admits, and the minimum hitting set alone, judged
on the online CPUs, unsound under hotplug in both directions.

The masks are tracked incrementally: every counting group keeps the
distinct masks of its subtree's tasks with a task count, under its lock.
A task points at the entry it is counted under in its own group
(``conc_class``); the counting ancestors hold the same mask. A task's mask
is registered in its group and the counting ancestors when it is set
(``set_cpus_allowed_common()``, before the mask is copied, with entries
preallocated by the process-context caller), inherited at fork (after
``cpuset_fork()`` applied the cpuset), moved with the task between
cgroups, and dropped when the task is released. A group starts counting
along with its counting ancestors, which the configuration walk visits
first: it registers its own tasks at every counting level of their chain
(``conc_classes_build()``), so its list is never merged on top, which
would count its tasks twice above it and leave, after an exit, a class
no task has. A child that was counting already, under a limit of its
own, is merged into every counting level from its parent up when the
parent starts counting it, and unmerged when the parent stops, which
drops the parent's list. A registration takes the task's lock and each
level's in turn, not the configuration mutex, while a merge copies the
child's list under the child's lock: whether a level is merged into its
parent is read under the level's own lock, and a task's walk goes up
from a level only when it is (``conc_chain_unlock()``), so that the task's
entry is either copied by the merge or added above by the walk, never
both, and an unregistration removes the copies there are. A group
created under a counting parent counts from its creation
(``conc_tg_online()``), before any task can join it: its tasks would
otherwise escape the limit. The mask is computed afresh at
every registration, from the cpuset as it is then, so that a task
registered while a cpuset change is reaching the tasks one at a time
(forked, migrated, or reached by the pass) gets the new mask; a cpuset
change registers the mask each task will have for all of them at once,
under the configuration mutex, before the per-task pass applies it
(``conc_cpuset_update()``), so that the old and the new masks are never
both counted with the limit relaxed to their width while the pass is
under way. Since the registrations can still be behind the tasks for an
instant, a relaxation that the registered masks ask for is confirmed by
a recount of the tasks' masks (``conc_recount_width()``) before it is
applied. The ``sched_setaffinity(2)`` check (``conc_check_affinity()``)
computes the width of the group's list with the task's mask replaced, at
every counting level; cpuset writes and migrations recount from the
tasks with the change in place. A migration is judged by the
cpuset the task moves to, which may sit below its cpu group: the cpu
controller's ``can_attach()`` reads it from the migration
(``cgroup_taskset_cur_dst_css()``), and a move between cpusets under one
cpu group, for which only the cpuset controller is called, is judged
from ``cpuset_can_attach()`` (``conc_cpuset_attach_check()``). A task on
its way out is never registered, since its release, which unregisters,
may have run already by the time a cgroup iterator hands it out; and the
release unregisters whatever the state of the task's group, which may be
going away with it.

Requests taken as they are
--------------------------

Cpuset v2, and the scheduler below it, put something else in place of a
request they cannot grant: a member cpuset gets its ``cpuset.cpus``
within its parent's CPUs, or its parent's whole when that leaves nothing;
a partition root gets what it may claim of its exclusive CPUs; a task
gets its requested mask within its cpuset's CPUs, or the cpuset's whole
when that leaves nothing. Each substitution undoes itself when the CPUs
it lacked come back, and one way they come back cannot be refused: the
removal of a partition root's cgroup, whose CPUs return to its parent.
The masks the limit admitted then become ones it never judged. Two
cpusets naming CPUs 3 and 4, one admitted as {3} within {1,2,3} while a
partition held CPU 4, end up disjoint under a limit of 1, and a task of
one waits for as long as a task of the other runs.

Within a counting subtree, every request is therefore taken as it is
(``cpuset_conc_literal()``, ``conc_request_literal()``):

* a member cpuset names no CPU, inheriting its parent's, or only CPUs of
  its parent's configured CPUs: none outside its parent, none a partition
  holds; offline CPUs count, as everywhere the configured CPUs are used;
* a partition root claims all it asks for, its exclusive CPUs, else its
  ``cpuset.cpus``, and keeps all of it but what its own partitions take;
* a task's requested mask lies within its cpuset's configured CPUs, or
  names every CPU the task can run on; or its cpuset is a member of the
  subtree naming CPUs (``cpuset_conc_fixed()``), whose configured CPUs are
  the ones it names by the first rule, changed by judged writes only:
  the request is then taken cut, as the intersection
  (``conc_request_taken()``), and stays so whatever the cpuset is made to
  name, but the cpuset cannot be made to name nothing, inheriting, while
  a request is cut.

What would break this is refused. A write to ``cpuset.cpus``,
``cpuset.cpus.exclusive`` or ``cpuset.cpus.partition``, anywhere, is
judged on its trial (see `Affinity masks and width`_): it fails with
``EBUSY`` when a cpuset of a counting subtree that takes its request as
it is would stop doing so (``cpuset_conc_check_literal()``), or when it
would cut the requested mask of a counting task whose cpuset it changes
(``conc_cpuset_check()``); a write whose effect the trial cannot compute
(``cpuset_conc_untried()``) fails where it matters: the cpusets it could
change are those taking their CPUs from the cpuset a claim moves CPUs
from, and those below a partition root whose claim follows, and of them
those the CPUs it could move reach (``conc.affected``); the write fails
while one of these is a cpuset of a counting subtree naming CPUs, or
holds a counting task. A write of ``cpuset.cpus.partition`` making a
cgroup a partition root is never refused: the partition root is created
invalid, ``PERR_CONC``, claiming nothing (``cpuset_conc_compute()`` takes
it as a member), and stays so until the file is written again, which
cpuset's engine never does on its own for this reason
(``update_parent_effective_cpumask()``). A migration into
a counting group of a task whose requested mask its new cpuset would cut
fails with ``EBUSY`` (``__conc_attach_check()``), as does a clone3(2)
with ``CLONE_INTO_CGROUP`` whose child would be such a task there
(``conc_fork_check()``), and ``sched_setaffinity(2)`` of such a mask with
``EINVAL``
(``conc_check_affinity()``). A write to ``cpu.concurrency.max`` that starts
a subtree counting fails with ``EBUSY`` when a cpuset or a task of the
subtree does not take its request as it is (``conc_subtree_literal()``,
``cpuset_conc_check_subtree()``). Only the cpusets of the subtree's own
cgroups are checked: a cgroup without a cpuset of its own has none below
it, and the cpuset above it, which all its tasks share, holds the widest
of their masks, whatever it becomes.

None of these checks can be overtaken by another:

* a cpuset write holds ``cpuset_mutex`` from its judgement to its end,
  and a limit write from its first check to the start of the limit;
* a migration is judged by the cpu controller and counted as pending in
  one section under ``conc_mutex``, itself under ``cpuset_mutex``
  (``conc_attach_check()``, ``conc_attach_begin()``), and a clone3(2) with
  ``CLONE_INTO_CGROUP`` likewise, pending until its child is registered
  (``conc_fork_check()``); a cpuset write's judgement and a limit write wait
  for the pending migrations and forks (``conc_wait_judged()``), and check
  their tasks where they land. Set-moves wait for migrations only: a fork
  claims no slot before its child first wakes up;
* ``sched_setaffinity(2)`` on a task of a counting group judges the
  requested mask and applies it under ``cpuset_mutex``
  (``conc_affinity_lock()``). On a task of a group that does not count, it
  judges nothing and takes no global lock, so that a limit elsewhere
  costs it nothing: its group stays one that does not count until the
  mask is applied, or a limit started over it meanwhile, which the
  application sees, as every start bumps a generation
  (``conc_limit_gen``) that the judgement recorded and that a stale
  judgement is retried on (``conc_judged_stale()``);
* the call judges by the task's group, and a migration of the task by its
  requested mask, and no lock both take orders the two: the threadgroup
  rwsem the call reads (``cgroup_threadgroup_change_begin()``) is not
  taken by a thread migrating alone by itself (``0`` written to its own
  ``cgroup.threads``). The cpu controller's judgement of a migration makes
  the task's ``conc_move_seq`` odd, under ``p->pi_lock`` where it reads the
  mask, until the migration ends (``conc_attach_finish()``). The call waits
  for an odd count to end before it reads the group, and records it. A
  judgement since read the old mask and changed the count: the change
  backs out where it is applied, under ``p->pi_lock``
  (``__set_cpus_allowed_ptr_locked()``), and is judged again in the group
  the task landed in.

Whoever waits under ``cpuset_mutex`` for migrations to be applied takes it
settled (``cpuset_lock_settled()``), with no migration between cpusets in
flight, as a cpuset write does: such a migration needs it in
``cpuset_attach()``, which the cpuset controller runs before the cpu
controller's ``attach()`` ends the pending count.

The removal of a partition root's cgroup then needs no check. No cpuset
of a counting subtree names the CPUs it returns, so every mask built
from named CPUs keeps its own. The masks it widens are those inherited
from the parent: a cpuset naming no CPU, and a task requesting every
CPU, within it. Each is the widest of its level before and after, and
holds every other mask of the level: nested masks stay nested, and the
width stays. A partition that hotplug invalidates keeps its claim in the
configured CPUs (see `Affinity masks and width`_), which do not change.
Placement within the bound therefore always exists, whatever the kernel
cannot refuse (see `Holding`_).

The cost is refusals of requests cpuset would have cut, where the cpuset
inherits its CPUs: a mask built from the topology that straddles such a
cpuset, such as a NUMA node's CPUs for a cpuset holding half of the
node, is refused; a task pinned outside it cannot be moved into it under
a limit; and the cpuset it inherits from cannot be narrowed off a CPU a
task names, other than with a mask naming every CPU, the administrator
narrowing the tasks' masks first. Where the cpuset names its CPUs, the
common case of a container's cpuset, the cut is taken (`Requests taken
as they are`_) and none of these is refused: the cpuset can be narrowed,
cutting further, but not emptied, to inherit, while a cut stands. The
refusals are reported by the ``sched_conc_refuse`` tracepoint, with the
task judged, the operation and the reason, and the requested masks by
``Cpus_requested`` in ``/proc/<pid>/status``.

Classes
-------

Placement works from the *live part* of every registered mask, the CPUs
of it that may be used now (online and not going offline), kept beside
the mask in its entry. The minimal live parts are the *classes*; equal
live parts make one class. Since the live parts are the configured masks
restricted alike, their classes never outnumber the width, hence the
limit. The minimal flags and the number of classes that no used CPU
hits (``unhit``) are maintained at the events that change them; the
hotplug hooks recompute the live parts and these for every counting
group (``conc_refresh_live()``). An entry whose
live part is empty is *dormant*: no class, since no slot could serve
it. Its sleeping tasks keep their masks, as they would without a limit;
one that wakes up before its CPUs come back is widened by
``select_fallback_rq()``, and its mask, around which other masks may
have become classes, becomes a class again when the CPUs return.

An entry is dormant too while every one of its tasks runs outside it,
its applied mask missing the registered one: each entry counts those
tasks (``conc_task_set_outside()``). Hotplug never changes a registered
mask, but it changes what cpuset applies: a cpuset whose CPUs are all
offline gives its tasks its parent's CPUs, and one whose CPUs miss a
task's whole requested mask applies its own and keeps the request
aside. When the CPUs come back, the live parts get them at once, and
cpuset gives them back to the cpusets one at a time, in a pass that can
take hundreds of milliseconds with many cpusets. Meanwhile a live class
whose tasks cannot reach it would keep headroom that none of them can
take, and the rule below would keep them off every other CPU too: held
while their group uses no CPU. The class wakes up when cpuset's pass
reaches the cpuset of one of its tasks, ahead of the task's mask, as the
registered masks move with a cpuset change (``conc_cpuset_update()``).

Static feasibility is not sufficient on its own because the set of CPUs in
use drifts as tasks wake and sleep. One rule (``__conc_room_for()``) keeps
headroom free for the unhit classes: a slot is added on a CPU only when
``nr_used + unhit' < effective``, where ``unhit'`` is the number of
unhit classes that CPU would not hit; a slot inside an unhit class hits
it and takes the headroom kept for it, whoever claims it. The claim
slow path, slot reservations, headroom making (``conc_make_headroom()``,
which drains idle slots and evicts joinable ones until the slot fits)
and the cleaner all apply it, and a class that appears unhit while the
rule no longer holds (a thread pinned while asleep, a limit written over
pinned threads, a CPU coming back) kicks the cleaner, which drains idle
slots until it holds again. The selection post-filter steers a task
that finds no used CPU onto a CPU inside an unhit class that its mask
allows, before it goes without a slot, so that an unconstrained task
uses the headroom kept for the sleepers rather than a CPU beside it. A
sleeping constrained thread thus always finds a CPU when it wakes,
whether the constraint comes from ``sched_setaffinity(2)`` or from its
cpuset. The cleaner does not drain a slot that is the only used CPU
hitting some class, since the headroom it would free is reserved for
that class anyway.

Enforcement model
=================

Vocabulary
----------

present
    A task is *present* on a CPU when it is queued on that CPU's runqueue
    (including delayed-dequeue and bandwidth-throttled tasks, which keep
    their runqueue) or when it has been selected for that CPU by a wakeup
    or fork and has not been enqueued yet ("in flight").

slot
    A (task group, CPU) pair with a non-negative presence count. The set
    of slots of a group is its *used set*; its size is ``nr_used``.

claim / release / move
    The three primitives: claim a slot for a task on a CPU, release it,
    or move it from one CPU to another atomically.

Per-group state
---------------

Every task group carries a ``tg_concurrency`` structure:

* ``present[cpu]`` (per-CPU ``atomic_t``): the presence count encoded
  with a state in its sign:

  - ``>= 0``: the CPU is in the used set and holds that many entities;
  - ``-1``: the CPU is not in the used set;
  - ``-2``: *draining*, an idle slot the cleaner is about to remove;
  - ``<= -BIAS``: *evicting*, a busy slot being emptied; the count is
    ``value + BIAS``.

* ``used`` (cpumask) and ``nr_used``: the used set, redundant with the
  signs of ``present`` but needed for scans.
* ``unused_cores`` (cpumask): the CPUs of the SMT cores on which no CPU
  is used, kept with the used set; a CPU coming online brings its
  siblings back into the core masks, and its core is updated in every
  group then (see `Selection`_).
* ``effective``: the enforced limit, ``0`` when unlimited.
* ``active``: counting is enabled for this group because it or an
  ancestor is limited. Groups outside any limited subtree pay nothing.
* ``lock``: a raw spinlock protecting the slow paths.

Lockless fast paths
-------------------

A claim on a CPU whose slot already exists is a single
``atomic_inc_unless_negative()`` on ``present[cpu]``: it succeeds iff the
value is non-negative, which by construction means the CPU is in the used
set. A release is ``atomic_dec()``. Neither takes a lock, and a sleep
followed by a wakeup on a CPU the group already uses never touches shared
state beyond that one counter.

Slot release is *sticky*: a count dropping to zero leaves the slot in the
used set. Idle slots are reclaimed only when headroom is needed, by the
cleaner, which first turns the value into ``-2`` with ``cmpxchg`` so that
a concurrent lockless claim either beats it (and keeps the slot) or sees a
negative value and falls to the slow path.

The rule holds for every transition out of a non-negative value, under the
group lock or not, since the lock does not exclude the lockless claim: one
atomic step, a ``cmpxchg`` from the exact value or an add of the
difference, never a read followed by a store. A store over a value the
claim just raised loses that claim: its task runs accounted on a slot that
is gone, the group on more CPUs than its limit and its IDs past the
bound. So a reservation its task did not fill goes from its reserved value
straight to ``-1`` (``conc_resv_finish()``), never through ``0``; an
ancestor's slot that a set-move leaves without entity is set draining by
``cmpxchg`` from ``0`` (``conc_ancestors_moved()``); and the set-move's
relabel adds the moved presence to the destination slot, which may be
live, rather than storing the sum.

Slow paths
----------

A claim on a CPU that has no slot takes the group lock and, if the group
is limited and ``nr_used >= effective``, fails. On failure the wakeup is
redirected to a CPU already in use (see `Selection`_) and the cleaner is
queued to reclaim an idle slot asynchronously so the next arrival finds
headroom. Adding a slot propagates one level up (see `Hierarchy`_).

Accounting is class agnostic: SCHED_FIFO, SCHED_RR and SCHED_DEADLINE
tasks are bounded exactly like fair tasks. The hooks live in the core and
in each class's migration paths.

Hierarchy
---------

Only limited subtrees count. Every group inside a limited subtree is
*active* and keeps its own counts. A group's ``present[cpu]`` counts
*entities*: its own tasks present on the CPU plus each child group that
has the CPU in its used set. Adding a CPU to a child's used set therefore
claims one entity on the parent, and removing it releases one. This keeps
``used(child)`` a subset of ``used(parent)`` and lets every limited level
enforce its own ``effective`` on its own ``nr_used``.

Locks nest child to parent on the upward claim. The cleaner, which must
look downward to find slots that are idle through the whole hierarchy,
never holds a parent lock while taking a child lock; it drains the
children first, one lock at a time, then the parent.

When a group becomes the top of a new limited subtree, the subtree is
*activated*: groups are marked active in pre-order, present tasks that
carry no accounting are claimed where they stand, and any previously
active subtree encountered is attached as a set of entities and not
descended into. Deactivation is the reverse, the children first: a group
that stops counting first stops its children from propagating into it
(``conc_detach_children()``), so that a child's claim that finds it inactive
finds its own propagation stopped too, and claims nothing above. The
per-task ``conc_accounted``
bit is what makes this race free: a claim, a release and the walker all
serialise on ``task_rq_lock()`` and the bit records whether the task's
counts exist.

Hooks
-----

claim
    ``select_task_rq()`` for ``WF_TTWU`` and ``WF_FORK`` after the class's
    choice, and the ``p->on_cpu`` wakelist shortcut in ``try_to_wake_up()``
    (which claims the task's current CPU and falls through to selection
    when the claim fails).

release
    ``__block_task()`` before the store of ``p->on_rq = 0`` (so that a
    concurrent wakeup observes the release before it can claim) and the
    exit path through the same function. ``ttwu_runnable()`` needs no
    hook: a task still queued keeps its slot.

move
    ``move_queued_task_locked()`` (RT and deadline push and pull, core
    migration, core scheduling), ``detach_task()`` in the fair load
    balancer (which becomes a predicate the callers must honour),
    ``migrate_swap_stop()`` (NUMA swap of two tasks, both moves committed
    or neither), and ``__migrate_task()`` through ``move_queued_task()``.
    A move that would violate the limit is refused and the caller leaves
    the task where it is.

proxy execution
    A blocked donor migrated by ``proxy_migrate_task()`` never runs as
    itself on the target CPU: it is released at proxy migration and claims
    again when it returns through the wakeup path.

group change
    ``sched_move_task()`` releases from the old group and claims in the
    new one; when the new group has no room the task goes without a slot
    and is held at its next return to user space (see `Holding`_).

Selection
---------

When the first active level from the task's group up has no headroom,
the classes search only the CPUs of that level's used set. The fair
class does so through a per-CPU mask, which ``conc_select_prepare()``
narrows to the used set and which every search of a wakeup, fork or
exec uses (``task_select_cpus()``), from the fork and exec group search
(``sched_balance_find_dst_group()``) down to ``select_idle_cpu()``. The
rt and deadline classes narrow what ``cpupri`` and ``cpudl`` found
(``find_lowest_rq()``, ``find_later_rq()``). A search that strays
outside the used set gets its add refused, and a refused add asks the
cleaner for headroom: if the group search saw every CPU, a fork into a
group whose CPUs are all busy would go to the idle SMT sibling of one of
them, the cleaner would free some idle slot in exchange, and the used set
would wander off one CPU per core onto both siblings of fewer cores.

While the level has headroom, its slots go one per SMT core first. The
siblings of a core share its execution resources, and slots stay: a slot
added on the idle sibling of a used CPU while a core the group does not
use at all was there to take leaves the group running on less than its
limit allows, for as long as the slot lasts. So while some core with no
used CPU is among the active CPUs a task may run on, its searches leave
out the unused siblings of the used CPUs (``conc_spread()``): the fair mask
holds the used set and the unused cores, the load balancer does not pull
onto such a sibling (``conc_can_move()``), and the rt and deadline searches
drop the siblings when that leaves a CPU, an rt or deadline task taking a
sibling rather than waiting. Once every core the task may use has a used
CPU, the siblings are taken like any CPU, and a limit at or above the
number of CPUs the task may run on, which has room for all of them,
spreads nothing. Of nested levels with headroom the innermost one that
spreads narrows the search; a full level above it narrows it further to
its used set. The spread is per core only: which last-level cache a slot
lands in is left to the classes' own placement, which keeps communicating
tasks close, where spreading over the caches would add to their
communication delay.

``select_task_rq()`` then post-filters: if the
class's choice cannot be claimed (the used set changed meanwhile, a
wakeup's previous CPU was taken as is, or an rt or deadline search found
no used CPU at the priority level it looked at), the task is placed on a
CPU of its innermost active group's used set that it is allowed to run
on, preferring its previous CPU, then an idle one, then any. A task
whose mask is behind its cpuset (a cpuset change reaches the tasks one
at a time, see `Cpuset changes`_) is brought up to date here and selects
again. If no such CPU exists (the mask does not intersect the used set
and there is no headroom), the task goes without a slot: it takes no
concurrency ID, finishes in kernel mode what it was woken for and is
held at its return to user space (see `Holding`_); a held task being
placed again stays held instead.

Eviction
--------

Eviction empties a CPU of a group's presence and is needed on limit
decrease, on relaxation being undone (a class disappears), on attach into
a limited group when the task sits on a CPU the group does not use, and on
CPU hot-unplug. Innermost limited descendants are evicted first. A slot
with no entity is drained at once; a waking task claims slots without the
group's lock and may take it first, and the slot is then evicted like any
busy one.

Four mechanisms, tried in this order and chosen so that a group is never
present on more CPUs than before, which is what keeps concurrency IDs
below their bound (see `Per-mm concurrency IDs`_):

* *join*: a task is migrated with ``migration_cpu_stop()`` to a CPU that
  its own group already uses, within the target mask when the eviction
  has one. No slot is added.
* *consolidate*: a child subtree whose tasks cannot join within the child
  is set-moved onto a CPU the evicting level already uses, which adds
  nothing at that level. A shrink uses these two, then *hold* (below),
  since relocating a presence to a new CPU would not reduce the count;
  a victim whose tasks cannot be placed any of these ways is skipped and
  the shrink stalls when none can.
* *set-move*: the whole presence of a subtree on a CPU is relocated to one
  new CPU under ``stop_two_cpus()``. The destination is a CPU that every
  task still on the source may run on by its *entitled* mask, its cpuset's
  masked by its requested mask, which is ahead of its current mask while a
  cpuset change is reaching the tasks (the stopper applies the entitled
  mask to a task it moves outside its current one, as the cpuset's own pass
  would moments later); among those, one inside every class that only the
  source CPU hits when there is one, so that a sleeping member of such a
  class does not wake up to no usable CPU; and preferably one the parent
  uses. Arrivals are stopped first by making the slot values negative, an
  expedited grace period lets claims already made finish their enqueue (a
  claim and its enqueue run with interrupts disabled) and the stopper
  flushes the wakelist. The stopper enumerates the source runqueue itself,
  class by class, verifies that what it finds accounts for the whole
  presence at every level, hands the CPU-owned ``mm_cid`` of every mm of
  the moved groups from the old CPU to the new one (the mms are collected
  afresh before each attempt, and one whose threads all left the moved
  subtree meanwhile, since a cgroup migration does not wait for a set-move,
  has its ID dropped instead of handed over), moves the tasks and relabels
  the slots, all under both runqueue locks while neither CPU runs anything
  else. The destination is reserved beforehand at every level up to the top
  of the active chain, counted against the limit but refusing arrivals, so
  that the ancestors are exact too: an ancestor gains an entity on the
  destination only when the slot of the level below it there is new, the
  top group's for its parent, and a slot that was there already is
  counted above it, so that the claims stop at the first, a reservation
  that takes the place of the source slot the move empties (a *swap*) needs
  no headroom and is no excess, and an ancestor's slot on the source that
  the move leaves without entity is set draining under the stopper and
  removed right after. A wakeup that finds no legal CPU during a set-move
  lands on the evicting slot, where the IDs still are, and the stopper
  picks it up; the stopper holds a lock that such late claims take, so a
  claim either lands before the count check or finds the slot gone and
  selects again. cgroup migrations, affinity changes and set-moves exclude
  each other: ``can_attach()`` registers a pending migration (under the
  configuration mutex once a group counts, without a lock before),
  ``__set_cpus_allowed_ptr()`` judges the used sets under that mutex and
  counts as pending until the change is applied, and a set-move waits for
  none to be pending before it starts (except one asked for by the
  migrating task itself, whose cpuset phase changes the threads' masks
  before the cpu controller's attach claims anything), since a migration
  claims slots thread by thread on the threads' current CPUs and an
  affinity change migrates to the CPU it judged usable.
* *hold*: the tasks of the subtree still present on the CPU, which
  could neither join nor be set-moved, are held at their next return to
  user space, with arrivals on the CPU refused meanwhile, until the slot
  is empty and drained (``conc_hold_cpu()``, see `Holding`_). A shrink
  uses it after joining, hot-unplug after set-moving.

Set-move serves the cases where a CPU must be vacated or a task needs a
CPU its group does not use: CPU hot-unplug, from
``sched_cpu_deactivate()`` while the CPU is still active and before
``balance_push()`` starts forcing tasks away, with the present tasks
pinned to the dying CPU widened first as ``select_fallback_rq()`` widens
them once ``balance_push()`` pushes them (sleeping ones keep their masks,
see `Classes`_); and
an affinity change that
leaves a task no used CPU, from ``__set_cpus_allowed_ptr()`` before the
mask changes, judged on the mask that will apply (for a change from the
kernel side, the cpuset's masked by the requested one). For a task that
is asleep at that moment, the new mask is made reachable instead:
headroom is made for a CPU of it, or a presence of the group is
set-moved onto one, so that the wakeup finds a CPU.
The affinity change of a task that needs a CPU of its own reserves one
after making headroom for it at every level that needs it, by draining
an idle slot or by evicting a used CPU whose tasks can all join; the
task's arrival fills the reservation (``conc_resv_cpu``), every other
arrival on a reserved, draining or evicting slot is refused. The task's
``conc_resv_cpu`` is cleared before the reservation is settled, so that a
reserved slot found empty then stays empty: nothing else lands there,
forced claims hold the configuration mutex, and the lockless claim
refuses a negative value. The IDs that an arrival which came and went,
leaving by migration rather than by schedule-out, may have cached on the
CPU are dropped at that point, and only then is the slot removed
(``conc_remove_reserved()``), so that none outlives it.
Transients, such as a thread in its last microseconds of exit that a
cgroup migration left behind with its slot, are waited out for a bounded
number of jiffies before the migration is allowed to go without a slot.

Migrate-disabled tasks are retried for a bounded number of rounds; when
they or a task with no possible destination remain, the eviction is
reverted and the group is left in the *stalled* state.

Idle slots are removed by the *cleaner*, a per-group work item queued
from the slow path through ``irq_work``. It frees first an idle slot that
shares its SMT core with another used CPU, so that the headroom made goes
to a core of its own (see `Selection`_). It drops CPU-owned ``mm_cid``
values on the slot's CPU for every mm of the subtree before removing the
slot, so that a CPU outside the used set never holds a concurrency ID. A
drain continues upward: an ancestor slot whose last entity was the
drained slot is removed with it, so that no idle sticky slot of an
ancestor refuses the next claim.

Holding
-------

A task that finds no CPU within its group's bound is never run in excess
of it. Three things happen instead, in order:

1. The task goes *without a slot* (``conc_hold_request()``, reason
   ``CONC_HOLD_UNPLACED``): the wakeup, migration or group change that
   found no CPU completes on the CPU it chose, with the task unaccounted
   and ``conc_hold_pending`` set. In that state the task takes no
   concurrency ID (``conc_task_unplaced()`` short-circuits the schedule-in
   of both domains), so that neither it nor, through a bit it would take
   from the pool, a legitimately placed task can expose an ID at or above
   the bound. It runs on in kernel mode only: the part of its wakeup it
   was woken for, where it may hold locks the repair itself needs
   (``conc_mutex``, held across a retry loop that sleeps, is one), the
   return through a stopper after a migration, the remainder of the
   ``cgroup.procs`` write that moved it.
2. At its next return to user space a task work (``TWA_RESUME``, queued
   by ``conc_hold_request()`` and kicked at a running task like a signal)
   either lets it go on, when a slot was found for it meanwhile (a
   load-balancer move, a set-move that merged it, a group change, or room
   on its CPU by then), refreshing its IDs; or *holds* it: dequeued from
   its scheduling class but left queued as far as the core is concerned
   (``p->on_rq``, PSI, uclamp, the load it migrates with), in
   ``TASK_RUNNING``, listed on ``conc_held_list`` with a reference. The
   per-task CFS bandwidth throttle parks tasks the same way at the same
   point, for the same reason: a task is never parked while it may hold
   a kernel lock, and a parked task must still be able to block, migrate
   and change class or group. Its class saw it go to sleep, and a sleep
   followed by a wakeup is all the class ever sees of a hold: a held task
   that blocks on its way out of the kernel becomes a plain sleeper, whose
   wakeup places it; anything else the core does with it, a migration
   (``balance_push()``, an affinity change, a NUMA swap) or a change of
   class, priority or group, starts by returning it to its class where it
   is, as woken there (``conc_unhold()``, from ``dequeue_task()``), and asks
   for the hold again. The class's own bookkeeping, the deadline class's
   bandwidth and timers and the load tracking of the fair class, then
   follows the change as it does for any queued task; a deadline task's
   replenishment timer treats a held task as one that is not queued. A
   plain wakeup leaves a held task alone, it is not asleep; signals wait
   with it, but SIGKILL (see below).
3. The *placement work* (``conc_hold_work_fn()``, a work item queued
   through ``irq_work`` from any context) runs a wakeup's selection for
   each held task, which refuses to go without a slot and leaves the task
   held when nothing within the bound is found, returns it to its class
   where it is and moves it to the CPU found as a queued task is moved;
   the task gets the slot found under its runqueue lock, while it is
   still held (``conc_select_account()``): the selection may widen its mask
   on the way (``select_fallback_rq()``, a task pinned to a CPU going
   offline), and that change, made to a queued task, returns it to its
   class where it is and asks for the hold again, as any other does; the
   slot then goes back. When nothing is found it makes a CPU of the task's
   mask reachable the way an affinity change does for a sleeping task
   (headroom, or a set-move onto the mask) and tries once more: the
   presence set-moved is the task's group's own, or that of a level above
   with no CPU in the mask, whose slots the subtrees beside the group may
   hold at its limit, out of reach of anything below it. A held
   task whose mask is left with no active CPU, the last one gone offline
   while it was out of its class, where neither ``balance_push()`` nor
   the unplug's widening sees it, is widened as ``select_fallback_rq()``
   widens a queued task there, pushed off a runqueue no longer active,
   and held again at its return to user space, then placed by its new
   mask; its cpuset's hotplug pass would give it the cpuset's CPUs only
   later, and none in the top cpuset. The work runs
   after every event that can make room: a slot that empties
   (``conc_release()``) or goes (``__conc_remove_slot()``), a reservation
   settled, a set-move done, a retry, a change of the held task's mask or
   group, CPU online and offline.

Holding is also an eviction mechanism (``conc_hold_cpu()``, reason
``CONC_HOLD_EVICT``): the tasks of a subtree present on a CPU that could
neither join nor be set-moved are asked to hold, with the slots on the
CPU marked evicting so that nothing joins meanwhile, for a bounded number
of jiffies, until the CPU is idle and drained. A shrink uses it after
joining, hot-unplug after set-moving. A task asked to hold this way holds
even if its CPU still has a slot when it reaches its return to user
space.

What this buys: no task placed by a wakeup, a migration or a group change
ever runs user-mode code, nor takes a concurrency ID, in excess of its
group's bound; a task that is running when the bound drops under it (a
limit written over it, a cpuset change, a move into a full group) is held
at its next return to user space, microseconds away, and
``cpu.concurrency.effective`` reports that excess only until then. What
it costs: a task may wait, runnable, until the repair places it. Holding is
not a policy for a limit the tasks cannot fit under: the refusals under
`Interface`_ keep the width of the masks, hence the classes, within the
ceiling at every level, so a placement that gives every class a CPU
always exists, and a held task waits for the slots to get there, not for
another task to block. The placement work makes that happen with the
machinery of an affinity change (an idle slot drained, a busy one joined or
set-moved onto the waiter's mask), retried at every event, so the wait is
the transient of a change; it lasts longer only while a repair cannot
proceed (a migration-disabled task, a task slow to reach its return to user
space), until the next event. The one change admitted without a check, the
removal of a partition root's cgroup, leaves the width as it was (see
`Requests taken as they are`_). Kernel threads follow the limit like any
task, placed onto the used set, joined at evictions, carried by set-moves,
their masks counted as classes; but they never return to user space and are
never held. One that finds no CPU within the bound runs on without a slot
until its next wakeup, which places it again: a transient excess, bounded
by the thread's own blocking, that no count sees. Uncounted on purpose: a
kernel thread takes no concurrency ID (no mm, and the initial IPC
namespace), and a counted excess would raise the reported value and with it
the ID bound of the mms and namespaces next to it, letting a user thread
take an ID equal to the configured limit for a CPU that the kernel thread,
not it, occupies. The threads the kernel runs for a process, io_uring's
workers and vhost's (``PF_USER_WORKER``), are held like the rest of their
process: they never return to user space either, but run the work queued
for that return at the top of their loops. A task on its way out is not held either and takes no ID
either: ``do_exit()`` makes it leave its domains (``sched_mm_cid_exit()``)
before it sets ``PF_EXITING``, which is what ``conc_hold_request()`` turns it
away by, so that it neither takes an ID below the bound from the threads
that need one nor could show one to user space, which it never returns to;
before that point a hold already requested keeps it from taking one
(``conc_task_unplaced()``). Its excess is a kernel thread's.

SIGKILL ends a hold, and keeps one from starting: a held task would
otherwise die only once a slot let it in, as late as a repair that
cannot proceed makes it. ``signal_wake_up_state()`` calls
``conc_fatal_signal()`` for every SIGKILL (``kill(2)``, a group exit, the
other threads of a core dump, the OOM killer, ``cgroup.kill``). The task
work does not hold a task with SIGKILL pending, and the placement work
returns a held one to its class where it is, without a slot, serialized
with the hold by ``p->pi_lock``. Either way its hold stays pending, so
that it takes no ID, and it goes to ``do_exit()`` with what it has: a
slot, counted, or none, its excess then a task's on its way out. The
exit to user space delivers the signal before it leaves. The rseq update
it may run before that, right after the task works on architectures
without the generic entry code, skips a task with SIGKILL pending
(``rseq_slowpath_update_usr()``), which never returns to user space. A
held task never reaches that update: it leaves its CPU within
``task_work_run()``, at the ``preempt_enable()`` that follows the hold or
at the ``cond_resched()`` between task works, and ``task_work_run()``
runs the hold again when a move asks for it.

A held task counts as runnable for PSI, as a throttled one does, and the
freezer sees it as a running task until the slots free and it reaches its
own freezing point.

Retry events
------------

There is no periodic work. A stalled shrink or a relaxed limit is
re-evaluated on the next event that can change feasibility:

* task affinity change in the subtree (``__set_cpus_allowed_ptr()`` for
  the non-migrate-disable flags, which covers ``sched_setaffinity(2)``,
  ``cpuset.cpus`` propagation, cpuset attach, and user mask restore);
* task exit (new ``exit`` callback of the cpu controller), only when the
  group is relaxed or stalled;
* cgroup migration out of and into the subtree (both sides of
  ``cpu_cgroup_attach()``);
* writes to ``cpu.concurrency.max`` on the group or an ancestor;
* CPU offline.

Fork, exec, wakeups, sleeps, load-balancer and proxy moves,
``migrate_enable()`` and writes on descendants do not retry.

Hotplug
-------

A dying CPU is vacated by ``conc_cpu_deactivate()`` while it is still
active, so that a task of the group waking up with no other CPU lands on
it as a late arrival that the set-move picks up, instead of without a
slot on a new CPU. From then until the runqueue is empty the CPU is
*offlining*: no group adds a slot on it, and no eviction chooses it as a
destination; what joining and set-moving leave on it is held (see
`Holding`_). ``sched_cpu_wait_empty()`` then drains it from every active
group and retries the groups the unplug left relaxed or over their
limit. Tasks still pushed off a dying CPU by ``balance_push()`` are
moved without a slot when the limit refuses the destination, so that
hotplug never hangs on a limit, and held at their return to user space.
Only present tasks pinned to the dying CPU are widened, as
``balance_push()`` and ``select_fallback_rq()`` would widen them; a
sleeping one keeps its mask, also across a suspend, and its entry goes
dormant (see `Classes`_). CPU online needs nothing more than the live
parts coming back, used sets growing on demand; an entry whose tasks
all run outside it stays dormant until cpuset's pass gives them its
CPUs back (see `Classes`_).

Cpuset changes
--------------

A change of ``cpuset.cpus`` is applied to the tasks one at a time by
``cpuset_update_tasks_cpumask()``, each with an immediate migration when
its CPU is no longer allowed. Taken one task at a time, a group whose
presence must leave the old cpuset could not be set-moved while the
other tasks still carried the old mask, and a task asleep at that moment
could wake up to a presence outside its new mask. Three pieces make a
cpuset change the group-level affinity change it is:

* ``conc_cpuset_update()``, called by cpuset before it reaches the tasks,
  set-moves the presence of every counting subtree under the cpuset into
  the new mask as a whole (``conc_evict_cpu()`` with the new mask as the
  target), so that each task's own change then finds a used CPU of its
  mask;
* set-move destinations and the stopper judge the tasks by their
  entitled mask, read through ``cpuset_task_cpus_rcu()``, a lockless
  snapshot of the cpuset's effective CPUs for callers that hold a raw
  lock;
* a task waking up to no usable CPU whose mask is behind its cpuset is
  brought up to date in the wakeup path (``conc_task_mask_behind()``) and
  selects again.

The registered masks are the configured ones, which a cpuset write
changes and a hotplug event does not: ``conc_cpuset_update()`` registers
them ahead of the pass, which then finds them in place, and the masks a
set-move or a hotplug event applies leave them as they are. A write that
changes a cpuset's configured CPUs but not its effective ones reaches its
tasks through ``cpuset_conc_reregister()``, which calls
``conc_cpuset_update()`` for that cpuset.
``conc_cpuset_update()`` collects the topmost counting groups of the
cpuset's tasks in an array it grows as needed.

Per-mm concurrency IDs
======================

The ``mm_cid`` hot path (``mm_cid_schedin()`` / ``mm_cid_schedout()``) is
unchanged. Three things change in the slow path:

1. The bound enters the formula::

       opt_cids = min(nr_cpus_allowed, users, bound)
       max_cids = min(opt_cids + opt_cids / 4, num_possible_cpus(), bound)

   where ``bound`` is the effective limit of the innermost limited group
   containing every thread of the mm (the lowest common ancestor of the
   threads' groups) when there is one, and otherwise, for threads spread
   over several limited subtrees, the sum of the limits of the topmost
   limited groups covering them, each of which bounds its own subtree;
   a thread in no limited group leaves the mm unbounded. It is recomputed
   on fork, on cgroup attach, on exit of a thread outside the ancestor
   group, and on limit writes. The topmost groups are collected in a
   per-mm buffer that only process context grows: a scan under the raw
   lock that outgrows it keeps the bound as it is and defers the recount
   to the work item, which grows the buffer first. The
   threshold for leaving per-CPU mode follows the bound, and the mm never
   returns to per-task mode while it has more users than IDs, which a
   bound below the user count cannot provide. A one-shot warning with a
   dump of the per-CPU ID state fires if an ID is ever handed out at or
   above the bound.

2. CPU-owned IDs are dropped when a CPU leaves a group's used set, by the
   cleaner and by eviction, under the CPU's runqueue lock; when the last
   present task of a group leaves a CPU, at schedule-out; when a task of a
   bounded mm exits, since a slot the group gives up later may have no task
   of the mm left in the group to visit; and around every cgroup migration:
   before the threads leave, on the CPUs where their group's slot is idle;
   for the migrating mms, on every CPU whose slot is drained while the
   migration is in flight, since the threads leave the source groups'
   css_sets before the cpu controller switches their task_group and a drain
   in that window finds no task of theirs; and after they arrived, on the
   CPUs the new group does not use (first thing in the bound refresh,
   before its fixups allocate). Steady per-CPU mode never drops them
   otherwise, so without this the stale caches of CPUs that cycled through
   the used set would exhaust the ID space below the bound and
   ``mm_get_cid()`` would fall back above it. The bound an mm gets is the
   value enforced so far (what ``cpu.concurrency.effective`` reports):
   a decrease reaches the IDs once the group is within the new limit, not
   while its excess is still being evicted.

3. A bound decrease runs a fixup pass that drops CPU-owned IDs at or above
   the new ``max_cids`` on CPUs not currently running the mm. Task-owned
   IDs above the bound converge at the next schedule-in, and so does an ID
   handed over there between a task and its CPU, or the one a CPU kept
   found by a thread with none, such as one created after the decrease:
   under a bound, ``max_cids`` is no target for a later schedule-in. A
   decrease that leaves fewer IDs than users switches the mm to per-CPU
   ownership first, under the bound as it was, the writing thread's own ID
   transferred as a fork transfers the forking thread's, and lowers the
   bound after (``cid_apply_bound()``): under the new bound the switch
   would run short, a user that is not running keeping its ID until the
   fixup reaches it while another, scheduling in, needs one. A decrease
   is applied that way only, between configuration walks: a thread
   joining the mm meanwhile raises the bound when its group adds to it,
   never lowers it, and a deferred update, from a move or an exit, waits
   for the walk to be over (``conc_bounds_lock()``), since in the middle of
   one a group counts before its running tasks have a slot or a hold, and
   the ID a CPU caches for such a task would be kept past the bound
   applied. The userspace
   contract is: a thread's ID is below the bound in force when it was last
   scheduled in, where the bound in force is what the effective file
   reports: a decrease that stalls behind a cpuset change or an eviction in
   flight keeps it at the number of CPUs still in use for that moment. The
   file shows a drop of that value only once the bounds follow it: the
   bounds of a subtree are applied one domain after the other, and an mm
   reached late would otherwise keep IDs at or above a value already shown,
   for as long as the walk takes over the tasks before it. A drop records
   the value it leaves as a floor of what the file shows
   (``conc_report_drop()``), cleared once the bounds of the subtree are
   applied (``conc_bounds_apply()``): by the limit write or the retry that
   lowered the value, before it returns, or by the group's cleaner when an
   excess drains on its own, one CPU in use fewer (``conc_used_del()``). The
   stress selftest checks its IDs against the largest value it saw the file
   report during the epoch.

A thread pool whose size hovers around the limit switches modes more
often than before because the slack is capped; the existing hysteresis
limits the cost.

Cross-process concurrency IDs
=============================

A second ID, ``ns_cid``, exposed through a new 32-bit ``struct rseq``
field placed after ``slice_ctrl`` (replacing the reserved byte, as that
byte's comment foresaw), unique among the tasks of a *cid domain* that
are running at the same instant and below the domain's bound. It lets
processes of a container index shared memory by concurrency.

Decisions:

* The domain is an IPC namespace created with a creation flag, a new
  clone3/unshare flag valid only together with ``CLONE_NEWIPC``. The
  privilege is that of ``CLONE_NEWIPC``: ``CAP_SYS_ADMIN`` in the user
  namespace that owns the new namespace, so an unprivileged user gets it
  through a user namespace and a container runtime holds it anyway. The
  IPC namespace is flat, every runtime creates one per container,
  ``setns(2)`` and ``unshare(2)`` already define joining and leaving, and
  it is the kernel's definition of who can share memory: System V shared
  memory, POSIX message queues and the runtimes' ``/dev/shm`` are scoped
  by it. A dedicated cid namespace type would decouple the two at the
  cost of a clone flag, an nsfs type, ``setns`` support, ucount limits and
  runtime support; the engine is written against ``struct cid_domain``
  with membership maintained where ``nsproxy`` changes, so that promotion
  stays mechanical.
* The choice is made at creation and holds for the namespace's lifetime.
  There is no runtime toggle, and ns_cid never switches meaning under a
  running workload. A namespace created with the flag always runs the
  engine: ns_cid is an engine ID below the namespace's bound, sized like
  mm_cid by the union of the members' allowed CPUs (what a cpuset
  narrows), the member count and the cgroup bound, the engine's internal
  owner-mode switches staying invisible. A namespace created without the
  flag, the initial namespace included, reports the CPU number, a valid
  concurrency ID below the CPU count that is already maintained for
  ``struct rseq``, and costs nothing: the context switch does no ns_cid
  work for its tasks.
* Membership is every task with an mm whose ``nsproxy`` names the
  namespace, maintained at fork, exit, ``setns(2)`` and ``unshare(2)``;
  kernel threads are not members. The domain keeps the member list the
  engine needs for its fixup passes, and ``task_struct`` caches the
  domain pointer so the hot path never walks ``nsproxy``.
* The bound of a domain comes from the cgroup limits that apply to its
  members, never from a configuration of its own. Two values, as for a
  cgroup. The *ceiling* is taken once, at creation: the effective
  ceiling of the creator's cgroup, or the number of possible CPUs
  when no ceiling applies there. It never changes afterwards, which
  the fixed ceilings make possible without tracking: the
  members are kept in the subtree of the nearest cgroup with a ceiling
  value, the creator's own or an ancestor, where no effective ceiling
  is larger, and the only ways to outgrow the bound, a member moving out
  of that subtree, or ``setns(2)`` into the namespace from outside it,
  are refused with ``EBUSY``. The nearest such cgroup and not the
  outermost: limits nest, a host capping all its containers at 60 CPUs
  and each container at 2, and the ceiling the processes of a container
  size their tables by is the container's. The *current target* is the
  cgroup bound of the members, computed as for an mm, at most the ceiling
  bound; it moves as the limits do, and the IDs in use are
  compacted further by the cpuset union and the member count. A
  namespace sized by the CPU count behaves like an mm whose threads sit
  in no limited cgroup. The ceiling is taken at creation, and a
  cgroup has its ceiling from its own creation, before any namespace
  is created in it.
* Userspace reads the ceiling of its own namespace from
  ``/proc/sys/kernel/ns_cid_max`` and the current current target from
  ``/proc/sys/kernel/ns_cid_cur``, both read-only, and sizes its tables
  by the former. For ``mm_cid`` the ceiling stays the number of
  possible CPUs, since a process moves between cgroups freely, unless
  the process gives that up (see `Pinned processes`_); its current target
  is the effective limit of its cgroup as before.
* The engine is ``struct mm_mm_cid`` generalised into a cid domain
  (bitmap, per-CPU storage, task-owned and CPU-owned modes, the same
  ``max_cids`` formula with ``users`` being the member count) that the
  mm embeds and the namespace embeds. The hot path becomes a second
  schedin/schedout call for tasks whose domain pointer is set. The
  set-move stopper hands over, and slot removals drop, the IDs of both
  domains of the tasks concerned.
* ``struct rseq`` and ``AT_RSEQ_FEATURE_SIZE`` are extended the way
  ``node_id`` and ``mm_cid`` were; the field is written for every
  registration large enough to hold it, which is what the feature size
  advertises, on every architecture. The RSEQ ABI V2 mode, which the
  architecture decides and not user space, gates the time slice
  extension because that needs the generic entry code; it must not gate
  an ID, or ``ns_cid`` would stay 0 on the architectures that have rseq
  without it while the feature size claims the field.

How it is built:

* The flag is ``CLONE_IPC_CID`` for clone3(2), and ``UNSHARE_IPC_CID``
  for unshare(2), which aliases ``CLONE_CHILD_SETTID`` the way
  ``UNSHARE_EMPTY_MNTNS`` aliases ``CLONE_PARENT_SETTID``; both need
  ``CLONE_NEWIPC`` (EINVAL otherwise). The domain, ``struct ns_cid``, is
  allocated with the namespace and freed with it; a namespace without
  one costs a NULL pointer.
* Membership is driven by the namespace switch itself:
  ``switch_task_namespaces()`` leaves the old domain and joins the new
  one before the nsproxy changes hands, ``sched_post_fork()`` joins the
  child, exit leaves with the mm. A member holds a reference on its
  namespace, so that ``task_struct::ns_dom`` stays valid whatever the
  order of the exit and namespace paths, and a joining task widens the
  domain's allowed CPUs with its own mask, since the members of a
  namespace come from anywhere.
* The ceiling is frozen at the *first admission*, which is the
  creation in practice: for clone3(2) in ``sched_cgroup_fork()``, where
  the child's cgroup is final (``CLONE_INTO_CGROUP``) and the child is
  not visible yet; for unshare(2) when the creator joins. It is the
  effective ceiling of that cgroup, which its innermost ancestor
  with a ceiling, the cgroup itself included, has too; the domain pins that
  ancestor with a css reference. With no ceiling on the path, it is
  the number of possible CPUs, and then nothing is ever refused.
  The first admission, the setns(2) admissions and the migrations'
  checks hold ``conc_mutex``. A setns(2) admission and an unshare(2)
  join, which no cgroup lock orders against a migration of the task,
  first wait for the migrations judged before to be applied
  (``conc_wait_attach()``): one in flight did not see the admission and
  would move the task out of the subtree afterwards; waited for, the
  cgroup the admission reads is the one the task lands in, and a
  migration judged later sees ``ns_pending`` or the membership. A
  fork(2) child of a namespace whose bound
  is frozen, every new thread of a member included, is admitted without
  it, since nothing that admission reads can change: the child's cgroup
  is final, no migration sees the child yet, and a frozen bound stays
  as it is. The frozen flag is set with release semantics after the
  bound it publishes, and read with acquire semantics there and by the
  sysctls. A leave holds no ``conc_mutex``: the last member's leave waits
  for the domain's work, which takes it for a deferred bound update.
* The refusals: ``conc_attach_check()`` refuses a migration that would
  take a task out of the frozen subtree of a namespace it is a member
  of, is joining, or was born into and joins once forked (a child
  between ``cgroup_post_fork()`` and ``sched_post_fork()``). A setns(2)
  is admitted in the namespace's install step, before the point of no
  return, which marks the task with ``ns_pending`` so that a migration
  in the window up to the switch is checked against the new namespace
  too; the mark is cleared when the switch joins, or when the setns
  fails. A clone3(2) with ``CLONE_INTO_CGROUP`` outside the subtree
  fails with EBUSY.
* The bound of the domain is the cgroup bound computed for an mm
  (innermost counting group above the members' common ancestor, else
  the sum over the topmost ones), clamped to the ceiling, so that it
  never reads 0 (no bound) in a ceiling-bounded namespace; ``cid_get()``'s
  violation check thus covers the ceiling too.
* The sysctls are entries of the per-namespace ``kernel`` table of IPC
  sysctls, read-only, resolved through the reader's namespace like
  ``shmmax``; both are exclusive upper bounds, and a namespace without a
  domain reports ``nr_cpu_ids`` for both since ``ns_cid`` is the CPU
  number there.
* ``ns_cid`` is written to the rseq area of every registration larger
  than the original 32 bytes, the only lengths ``rseq(2)`` admits being
  that one and the ones that hold the whole field, validated like
  ``mm_cid`` in debug mode, reset at unregistration, and carried in
  ``struct rseq_ids`` so that a change raises the IDs update; a
  namespace switch forces one.
* A task that joins a domain while it is running, which only
  ``setns(2)`` does, takes its ID in the join itself, the way a schedule
  in would: in per CPU mode a joining user takes none, borrowing the ID
  of the CPU it runs on at its next schedule in, and a running joiner
  reaches user space before that. ``unshare(2)`` and ``execve(2)`` join
  the domain they create, where the first user takes an ID of its own,
  and a ``fork(2)`` child does not run before its first schedule in.

Pinned processes
================

A process sizing per-ID data by ``mm_cid`` needs the ceiling of the IDs
to stay put for as long as the data lives, and a process can be moved to
a cgroup whose ceiling is larger. ``prctl(PR_SET_CONCURRENCY_PIN, 1)``
gives that up, for good: the process pins itself to its cpu cgroup, and
``prctl(PR_GET_CONCURRENCY_PIN)`` returns the ceiling its ``mm_cid``
stays below from then on, or 0 when it is not pinned. The pin is opt-in
because it takes a liberty from whoever manages the cgroups, so it is
for the program that controls its own placement; a library that pins
on behalf of its application takes it from the application's manager.

* No task using the pinned mm changes cpu cgroup any more: a write to
  ``cgroup.procs`` or ``cgroup.threads``, a change of the controllers
  that would move it to another cpu css (the cpu controller turned off
  above it), or ``clone3(2)`` with ``CLONE_INTO_CGROUP`` into another
  cpu cgroup, fail with ``EBUSY``. Moves between cgroups that share a
  cpu css, the cpu controller not enabled there, change no limit and
  are allowed. A new thread joins its creator's group.
* The ceiling is the effective ceiling of the lowest common ancestor
  of the groups of the mm's threads when the pin is taken, or the number
  of possible CPUs when no ceiling applies there. That ancestor only
  descends from then on, as threads exit and none moves, and counts
  whenever it has a ceiling (a ceiling starts counting below it):
  the bound of the mm (see `Per-mm concurrency IDs`_) is then the
  effective limit of that ancestor or a descendant, never above
  its effective ceiling, which never changes. The ceiling is
  recorded rather than recomputed, since the ancestor descending would
  make it shrink.
* The pin and its ceiling live in the mm (``mm_struct::conc_pin``):
  ``fork(2)`` copies them with the memory sized by them, ``execve(2)``
  starts an mm of its own, unpinned. Unpinning fails with ``EPERM``.
* ``cgroup_mutex`` orders the pin against the changes of cgroup, which
  hold it from their check to the change: ``can_attach()`` to
  ``attach()`` for a migration (``conc_pin_check()``), and
  ``cgroup_css_set_fork()`` to ``cgroup_post_fork()`` for
  ``CLONE_INTO_CGROUP`` (``conc_pin_fork_check()``). The checks cost
  nothing until a process pinned itself.

Invariant checks
================

``CONFIG_SCHED_MAX_CONCURRENCY_DEBUG`` adds checks that each warn once, at
a few loads per context switch and per slot release:

* a task its group counts on a CPU (``conc_accounted``) is switched in, or
  switched out while it keeps its slot, while the group has no slot on
  that CPU (``conc_debug_switch()``);
* a release (``conc_release()``) finds no entity to remove: the slot is
  unused, draining, or holds none;
* after a schedule-out, the CPU still owns a concurrency ID of a bounded
  domain whose group has no slot on it (``conc_debug_cid_kept()``);
* a task waits a second for a slot, from its first hold to its placement,
  its blocking or its death, across the holds a move ends and asks for
  again (``conc_debug_hold_watch_fn()``, a watchdog the placement work arms
  while tasks are held). The end of a wait also reports, at the info
  level, the longest one so far from 10 ms on.

The first two are what a lost slot claim leaves (see `Lockless fast
paths`_), the second when the task leaves the CPU without being switched
there again: the group present on more CPUs than its used set, beyond its
limit, its IDs past the bound. The third is an ID that nothing drops any
more, cached outside the used set. They show well before an ID above the
bound is handed out, which takes the space below it exhausted as well.
The fourth is a placement that never came: the masks always have one
within the bound (see `Requests taken as they are`_), so a wait lasts as
long as a repair takes.

Simplifications
===============

This version differs from the full design in these ways:

* The fair class's wakeup, fork and exec placement is constrained to the
  used set through a per-CPU selection mask; the RT and deadline searches
  are constrained in ``find_lowest_rq()`` and ``find_later_rq()``, after
  ``cpupri`` and ``cpudl`` chose a priority level. The post-filter in
  ``select_task_rq()`` remains as the safety net (see `Selection`_).
* The spread of slots over SMT cores is applied by the class searches and
  the load balancer, not at the claim: a wakeup's previous CPU taken as
  is, the CPU a task attaches on, the CPU of a class no used CPU hits and
  an rt or deadline search left with siblings only may still add a slot
  on the sibling of a used CPU. The cleaner frees such slots first when it
  makes headroom.
* The configured CPUs are recomputed for the whole hierarchy on every
  write to the cpus, exclusive or partition files, with one more walk per
  remote partition, since cpuset keeps no list of them. Readers copy them
  under RCU alone, as the ``cpuset.cpus`` values were read before: a
  registration racing with a write can read a mask half old, half new,
  which the write's own registration pass then replaces. A write is
  judged by one more such computation, and one that moves CPUs between
  partitions walks every task of the system to find those whose
  configured CPUs change. The writes whose effect a trial cannot compute
  (see `Affinity masks and width`_) are refused outright under a limit
  rather than replayed through cpuset's partition engine, the partition
  changes nested in a partition in particular.
* A task that finds neither a used CPU nor a CPU of an unhit class goes
  without a slot and is held at its return to user space, see `Holding`_;
  during a set-move that case lands on the evicting source, where the IDs
  still are, as a late arrival the stopper picks up. With the classes made
  of the live parts of the configured masks, every sleeping task's
  constraint is a class and has its headroom; the case is left for tasks
  whose constraint appeared after the headroom was consumed. An affinity
  change registers the class of the mask that will apply before it makes
  headroom for the task, so that a wakeup in between cannot take that
  headroom; a task pinned to CPUs going offline alone that wakes up before
  the unplug widens it is widened at that wakeup, the way
  ``select_fallback_rq()`` would widen it later, rather than placed without
  a slot on a dying CPU. An affinity change to a mask whose only active
  CPUs are going offline is still honoured there, as it would be without a
  limit, and the unplug holds the task; no room is made for it, nor for a
  change to a mask with no active CPU, which is refused: there is none to
  make, and retrying would sleep under ``conc_mutex``. A set-move judges its
  destination by the tasks the stopper moves, the queued ones, and may land
  on a CPU the group uses already, merging the presence, which is what a
  cpuset change needs when the applied masks lag behind it and no task can
  join by its own mask.
* The concurrency ID a CPU caches for a domain is dropped with the
  group's slot there, unless a task of the domain runs on the CPU. A task
  that blocked is still the CPU's current task while the runqueue picks
  its successor with the lock dropped: its slot is given back already,
  so the drop no longer waits for its switch-out, and a transition
  completed at a switch-out checks the group's presence like the plain
  path does.
* While a group is over its limit, ``cpu.concurrency.effective``
  reports the number of CPUs in use, so a read can see the limit plus
  one for the duration of a set-move's swap (the reserved destination and
  the draining source both count for a moment) or of a shrink whose
  tasks have not reached their return to user space yet. The stress
  selftest reads the value again 50 ms later and
  fails only when it stays above the limit, which is what a leaked
  parent-level entity caused: a reservation made for a task whose group
  had just gained the slot through a lockless claim kept the entity it
  had claimed for the ancestors although the slot had propagated its
  own, and the parent could never drain that CPU again.
