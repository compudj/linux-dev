.. SPDX-License-Identifier: GPL-2.0

.. _sched-max-concurrency:

====================================================
Per-cgroup limit on concurrently used CPUs (design)
====================================================

This document records the design of the ``cpu.concurrency.max`` cgroup
interface and of the scheduler machinery that enforces it. It is written
for reviewers and for whoever picks the work up later. It states the
decisions taken, and why. The last section lists what the implementation
leaves simple.

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
    is what lets a concurrency ID namespace take a ceiling as the maximum
    of its ``ns_cid``, below which the IDs follow the limit (see
    `Cross-process concurrency IDs`_). The bound of ``mm_cid`` follows
    the cgroup a process is moved to, so data indexed by ``mm_cid`` is
    sized by the number of possible CPUs.

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

``cpu.concurrency.stat``
    Read-only flat-keyed file, non-root cgroups only. It counts the
    cgroup and its descendants:

    ``nr_held``
        The tasks held now (see `Holding`_).
    ``nr_holds``
        The holds since the cgroup was created.
    ``held_usec``
        The total time tasks spent held.
    ``nr_rotations``
        The slots taken from running tasks (see `Rotation`_).
    ``rotated_usec``
        The held time of the holds which a rotation ended.

``cpu.concurrency.events``
    Read-only flat-keyed file, non-root cgroups only. It has one key,
    ``rotation``, with the value of ``nr_rotations``. A change raises a
    file notification, as ``memory.events`` does.

A limit is a number of CPUs. It does not depend on the affinity masks of
the tasks, and no operation is refused because of them:

* A write to ``cpu.concurrency.max`` fails with ``EINVAL`` for a
  malformed value, for ``0`` and for a limit above the effective ceiling.
  It fails with ``EBUSY`` when it would leave a SCHED_DEADLINE task below
  it with fewer CPUs than its bandwidth was admitted against (see
  `SCHED_DEADLINE`_). It never fails because of where the tasks run.
* ``sched_setaffinity(2)``, the writes to the cpuset files, cgroup
  migrations, ``fork(2)`` and ``clone3(2)`` with ``CLONE_INTO_CGROUP``
  succeed under a limit as they do without one.
* Two refusals remain. A SCHED_DEADLINE task is refused in a limited
  subtree, except in a deadline domain (see `SCHED_DEADLINE`_). A member
  of a concurrency ID namespace cannot leave the home of the namespace
  (see `Cross-process concurrency IDs`_).

The masks of a cgroup can need more CPUs than its limit. Two tasks pinned
to two different CPUs under a limit of 1 are an example. The kernel then
shares the slots between the tasks over time (see `Rotation`_). The limit
holds at every instant. ``nr_rotations`` tells the administrator that the
cgroup runs in this degraded mode.

The decrease policy is "write, then read back". A manager writes the
limit and reads the effective file. While the eviction is not complete,
the file shows the number of CPUs still used. Rewriting the same value
asks for a retry now. The kernel also retries on its own, on the events
listed in `Retry events`_.

Effective value
---------------

Let ``request(cg)`` be the written limit, with ``max`` treated as
infinity, and ``ceiling(cg)`` the ceiling the cgroup was created with.
Then::

    ceil_eff(cg)  = min(ceiling(cg), ceil_eff(parent(cg)))
    effective(cg) = min(request(cg), effective(parent(cg)), ceil_eff(cg))

Both values are hierarchical: a child is never allowed more than its
parent. A ceiling without a limit is enforced as a limit. Processes size
per-ID data by the ceiling, so the IDs must stay below it.

The effective value does not fold in the size of the cpuset. A cgroup
limited to 8 inside a cpuset of 4 reports 8. The limit is what the
controller enforces, not what the cpuset provides.

After an eviction that could not complete (a "stalled shrink"), the
effective file reports the size actually enforced, that is the number of
CPUs still in use, until the next retry succeeds.

Rotation
--------

Every affinity mask is accepted under a limit. So a cgroup at its limit
can use only CPUs which are outside the mask of one of its tasks. That
task finds no CPU within the limit and is held (see `Holding`_). It must
not wait for as long as the other tasks run. The placement work
therefore takes a slot from running tasks for it
(``conc_rotate_for()``):

* at once, when the tasks of the slot are of a lower scheduling class
  than the held task;
* after one rotation period, when the tasks of the slot are of the same
  class and the slot is itself at least one period old;
* never, when a task of the slot is of a higher class.

The class of a slot is the highest class among the tasks which hold it
(``conc_slot_class()``). Among the slots which can be taken, the oldest
one is taken. Held tasks are served in the order of their holds, so no
task starves. The room which a rotation frees is reserved for the task
it was freed for (``conc_resv_make()``). Otherwise the evicted tasks, or
a wakeup, could take it first.

A slot is taken by holding its tasks (``conc_hold_cpu()``, see
`Eviction`_). A task which runs is held at its next return to user
space. A task which is queued but not on the CPU gives its slot back at
once and keeps the hold pending: it runs no user space code before it is
held. So a task which a higher class keeps off the CPU does not keep its
slot.

The rotation period is 100 ms by default. It is set in the debugfs file
``sched/conc_rotation_ms``. While tasks are held, the placement work
looks again every quarter of a period.

Tasks which share one mask never rotate: the ``nr_rotations`` of their
cgroup stays at 0. Masks which overlap in part are more complex. The
placement work first moves the tasks which the cgroup has on one of its
CPUs to a CPU of the held task's mask (``conc_make_reachable()``). It
rotates when no such move is possible. It does not search for an
arrangement which changes several CPUs, so it can rotate where one
would fit.

A cgroup which rotates pays these costs, measured in QEMU guests with
tasks which spin in user space:

* Each task gets its fair share of the slots, within 4 points:
  50% for two tasks over one slot.
* A hand-over leaves the slot unused for 0.25 to 1.1 ms. That is about
  1% of the capacity at 100 ms.
* A fair task which wakes up with no slot in its mask waits up to one
  period, about 105 ms. A SCHED_FIFO task in that position runs about
  1 ms after its wakeup, 3 ms at most.

Each rotation is reported by the tracepoint ``sched_conc_rotate``, with
the task served, its mask, the CPU of the slot taken and the cgroup ID
of the level which was at its limit.

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
used set. Idle slots are reclaimed only when room is needed, by the
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
room. Adding a slot propagates one level up (see `Hierarchy`_).

Accounting is class agnostic: SCHED_FIFO and SCHED_RR tasks are bounded
exactly like fair tasks. SCHED_DEADLINE tasks are kept out of a counting
subtree while deadline admission is on: their bandwidth is admitted
against every CPU of their root domain (``sched_dl_overflow()``), which
a limit would take from them unseen, as an affinity narrower than the
root domain would, and the deadline class refuses that
(``dl_task_check_affinity()``). ``sched_setattr(2)`` to the deadline
policy fails with ``EPERM`` in a counting group (``conc_dl_admit()``),
and a migration of a deadline task into one, or a limit starting over
one, with ``EBUSY`` (``conc_dl_task()``), but in a deadline domain, a
partition root whose limit the admission is made against (see
`SCHED_DEADLINE`_). A deadline task cannot fork as one. The deadline
class is still reached through priority inheritance, and with admission
off; those tasks are bounded like the rest. The
hooks live in the core and in each class's migration paths. Only the
placement of a held task differs: an rt or deadline task, a lock owner
boosted by priority inheritance among them, places itself, at its own
priority (see `Holding`_).

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
    again when it returns through the wakeup path. ``find_proxy_task()``
    never runs a held lock owner (see `Holding`_).

group change
    ``sched_move_task()`` releases from the old group and claims in the
    new one; when the new group has no room the task goes without a slot
    and is held at its next return to user space (see `Holding`_).

Selection
---------

When the first active level from the task's group up has no room
(``nr_used >= effective``),
the classes search only the CPUs of that level's used set. The fair
class does so through a per-CPU mask, which ``conc_select_prepare()``
narrows to the used set and which every search of a wakeup, fork or
exec uses (``task_select_cpus()``), from the fork and exec group search
(``sched_balance_find_dst_group()``) down to ``select_idle_cpu()``. The
rt and deadline classes narrow what ``cpupri`` and ``cpudl`` found
(``find_lowest_rq()``, ``find_later_rq()``). A search that strays
outside the used set gets its add refused, and a refused add asks the
cleaner for room: if the group search saw every CPU, a fork into a
group whose CPUs are all busy would go to the idle SMT sibling of one of
them, the cleaner would free some idle slot in exchange, and the used set
would wander off one CPU per core onto both siblings of fewer cores.

While the level has room, its slots go one per SMT core first. The
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
spreads nothing. Of nested levels with room the innermost one that
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
and there is no room), the task goes without a slot: it takes no
concurrency ID, finishes in kernel mode what it was woken for and is
held at its return to user space (see `Holding`_); a held task being
placed again stays held instead.

Eviction
--------

Eviction empties a CPU of a group's presence and is needed on limit
decrease, on attach into a limited group when the task sits on a CPU the
group does not use, on CPU hot-unplug, and when a rotation takes a slot
(see `Rotation`_). Innermost limited descendants are evicted first. A slot
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
  would moments later); preferably one the parent uses;
  and a housekeeping CPU of the scheduling domains rather than one outside
  them (``isolcpus=domain``, an isolated partition), which the scheduler
  fills only by a task's affinity, taken only when the masks leave no
  other (``conc_first_dest()``). Arrivals are stopped first by making the
  slot values negative, an
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
  no room and is no excess, and an ancestor's slot on the source that
  the move leaves without entity is set draining under the stopper and
  removed right after. A wakeup that finds no legal CPU during a set-move
  lands on the evicting slot, where the IDs still are, and the stopper
  picks it up; the stopper holds a lock that such late claims take, so a
  claim either lands before the count check or finds the slot gone and
  selects again. cgroup migrations, affinity changes and set-moves exclude
  each other: ``can_attach()`` registers a pending migration (under the
  configuration mutex once a group counts, without a lock before),
  ``__set_cpus_allowed_ptr()`` prepares the change under that mutex
  (``conc_prepare_affinity()``) and counts as pending until the change is
  applied, and a set-move waits for
  none to be pending before it starts, since a migration claims slots
  thread by thread on the threads' current CPUs and an affinity change
  migrates to the CPU it prepared. Two do not wait: one asked for by
  the migrating task itself, whose cpuset phase changes the threads' masks
  before the cpu controller's attach claims anything, and one that makes
  room for another set-move or a reservation, which waited and has
  held the mutex since: dropping it there would expose reserved slots
  whose entity above is not claimed yet.
* *hold*: the tasks of the subtree still present on the CPU, which
  could neither join nor be set-moved, are held at their next return to
  user space, with arrivals on the CPU refused meanwhile, until the slot
  is empty and drained (``conc_hold_cpu()``, see `Holding`_). A task
  which is queued but not on the CPU gives its slot back at once. A
  shrink uses it after joining, hot-unplug after set-moving. A rotation
  uses it alone.

Set-move serves the cases where a CPU must be vacated or a task needs a
CPU its group does not use: CPU hot-unplug, from
``sched_cpu_deactivate()`` while the CPU is still active and before
``balance_push()`` starts forcing tasks away, with the present tasks
pinned to the dying CPU widened first as ``select_fallback_rq()`` widens
them once ``balance_push()`` pushes them (sleeping ones keep their
masks); and an affinity change that
leaves a task no used CPU, from ``__set_cpus_allowed_ptr()`` before the
mask changes, for the mask that will apply (for a change from the
kernel side, the cpuset's masked by the requested one). For a task that
is asleep at that moment, the new mask is made reachable instead:
room is made for a CPU of it, or a presence of the group is
set-moved onto one, so that the wakeup finds a CPU. When neither is
possible, the task moves without a slot and is held, and the rotation
gives it a slot.
The affinity change of a task that needs a CPU of its own reserves one
after making room for it at every level that needs it, by draining
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
shares its SMT core with another used CPU, so that the room made goes
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
2. At its next return to user space, in the work that return runs
   (``resume_user_mode_work()``, notified by ``conc_hold_request()``
   through ``TIF_NOTIFY_RESUME`` and kicked at a running task like a
   signal), ``conc_hold_resume()`` either lets it go on, when a slot was
   found for it meanwhile (a load-balancer move, a set-move that merged it,
   a group change, or room on its CPU by then), refreshing its IDs; or
   *holds* it: dequeued from its scheduling class but left queued as far
   as the core is concerned (``p->on_rq``, PSI, uclamp, the load it
   migrates with), in ``TASK_RUNNING``, listed on ``conc_held_list`` with
   a reference, off its CPU there until it is placed, before the rseq
   update writes its IDs. The per-task CFS bandwidth throttle parks tasks
   the same way, for the same reason: a parked task must still be able to
   block, migrate and change class or group. It does so from a task work,
   which ``task_work_run()`` runs for callers other than the return to
   user space too, io_uring's SQPOLL thread under its ring's lock among
   them; ``resume_user_mode_work()``, where memcg's over-high throttling
   sleeps as well, runs nowhere a lock is held but a vCPU's mutex at a KVM
   guest entry, which that throttling sleeps under too. With proxy
   execution, a task blocked on that mutex waits for it as it does
   without: the walk to the lock's owner treats a held one as an owner
   that cannot run (``find_proxy_task()``), since running it as the
   execution context would run it out of its class, without a slot. Its
   class saw it go to sleep, and a sleep followed by a wakeup is all the
   class ever sees of a hold: a held task that blocks on its way out of
   the kernel becomes a plain sleeper, whose
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
   An rt or deadline task, a lock owner boosted by priority inheritance
   among them, is not left to the placement work below, whose kworker,
   of the fair class, the tasks it keeps waiting may keep from running:
   its return to user space places it, at its own priority, with its hold
   left pending so that it takes no ID meanwhile (``conc_place_self()``). It
   moves to a used CPU of its group in its mask through a stopper, as
   ``sched_exec()`` moves a running task, the move claiming the slot;
   with none, it first makes the repair the placement work would make
   (``conc_make_reachable()``, below). What a few attempts do not settle
   is held as for any task.
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
   (room, or a set-move onto the mask) and tries once more: the
   presence set-moved is the task's group's own, or that of a level above
   with no CPU in the mask, whose slots the subtrees beside the group may
   hold at its limit, out of reach of anything below it. When the task
   is still held after that, the work takes a slot from running tasks
   for it, by the rules of `Rotation`_, and reserves the room freed for
   it. A held
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
it costs: a task may wait, runnable, until the repair places it. The
placement work first uses the machinery of an affinity change (an idle
slot drained, a busy one joined or set-moved onto the waiter's mask),
retried at every event. Where one such move serves the waiter, the wait
is the transient of a change. Where none does, the held task waits for
its turn: at most one rotation period behind tasks of its own class, and no
time behind tasks of a lower class (see `Rotation`_). A wait lasts
longer only while a repair cannot proceed (a migration-disabled task, a
task slow to reach its return to user space), until the next event.
Kernel threads follow the limit like any
task, placed onto the used set, joined at evictions, carried by
set-moves; but they never return to user space and are
never held. One that finds no CPU within the bound runs on without a slot
until its next wakeup, which places it again: a transient excess, bounded
by the thread's own blocking, that no count sees. Uncounted on purpose: a
kernel thread takes no concurrency ID (no mm, and the initial concurrency ID
namespace), and a counted excess would raise the reported value and with it
the ID bound of the mms and namespaces next to it, letting a user thread
take an ID equal to the configured limit for a CPU that the kernel thread,
not it, occupies. The threads the kernel runs for a process, io_uring's
and vhost's (``PF_USER_WORKER``), take no concurrency ID either
(``sched_mm_cid_fork()``): they never return to user space, have no rseq
area, and an ID of theirs would be one fewer for the threads that report
theirs. vhost's workers are held like the rest of their process, at the
top of their loop, where they run the work of the return to user space
with nothing locked. io_uring's (``PF_IO_WORKER``) follow the limit as
kernel threads do: the SQPOLL thread runs that work under its ring's lock
as it tears the ring down, and would be parked holding it. A task on its
way out is not held either and takes no ID either: ``do_exit()`` makes
it leave its domains (``sched_mm_cid_exit()``)
before it sets ``PF_EXITING``, which is what ``conc_hold_request()`` turns it
away by, so that it neither takes an ID below the bound from the threads
that need one nor could show one to user space, which it never returns to;
before that point a hold already requested keeps it from taking one
(``conc_task_unplaced()``). Its excess is a kernel thread's.

SIGKILL ends a hold, and keeps one from starting: a held task would
otherwise die only once a slot let it in, as late as a repair that
cannot proceed makes it. ``signal_wake_up_state()`` calls
``conc_fatal_signal()`` for every SIGKILL (``kill(2)``, a group exit, the
other threads of a core dump, the OOM killer, ``cgroup.kill``). The
return to user space does not hold a task with SIGKILL pending
(``conc_hold_resume()``), and the placement work returns a held one to
its class where it is, without a slot, serialized with the hold by
``p->pi_lock``. Either way its hold stays pending, so
that it takes no ID, and it goes to ``do_exit()`` with what it has: a
slot, counted, or none, its excess then a task's on its way out. The
exit to user space delivers the signal before it leaves. The rseq update
it may run before that, at the end of ``resume_user_mode_work()`` on
architectures without the generic entry code, skips a task with SIGKILL
pending (``rseq_slowpath_update_usr()``), which never returns to user
space. A held task never reaches that update before it is placed: it
leaves its CPU in ``conc_hold_resume()``, which runs before the update,
and a move that asks for the hold again notifies the return once more.

A held task counts as runnable for PSI, as a throttled one does, and the
freezer sees it as a running task until the slots free and it reaches its
own freezing point.

Retry events
------------

A stalled shrink is re-evaluated on the next event that can let it go
on. The only periodic work is the look of the placement work while
tasks are held (see `Rotation`_):

* task affinity change in the subtree (``__set_cpus_allowed_ptr()`` for
  the non-migrate-disable flags, which covers ``sched_setaffinity(2)``,
  ``cpuset.cpus`` propagation, cpuset attach, and user mask restore),
  only when a shrink is stalled. A cpuset applying
  its CPUs to its tasks (a ``cpuset.cpus`` write, its propagation, CPU
  hotplug) retries each topmost counting group once, at the end of the
  pass, rather than after each task;
* task exit (new ``exit`` callback of the cpu controller), only when a
  limit covering the group is stalled;
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
group and retries the groups the unplug left over their
limit. Tasks still pushed off a dying CPU by ``balance_push()`` are
moved without a slot when the limit refuses the destination, so that
hotplug never hangs on a limit, and held at their return to user space.
Only present tasks pinned to the dying CPU are widened, as
``balance_push()`` and ``select_fallback_rq()`` would widen them; a
sleeping one keeps its mask, also across a suspend. CPU online needs
nothing: the used sets grow on demand.

Cpuset changes
--------------

A change of ``cpuset.cpus`` is applied to the tasks one at a time by
``cpuset_update_tasks_cpumask()``, each with an immediate migration when
its CPU is no longer allowed. Each change is prepared like any affinity
change (``conc_prepare_affinity()``, see `Eviction`_). Three pieces
serve the pass:

* the retries which the changes ask for are done once per topmost
  counting group, at the end of the pass (``conc_pass_begin()``,
  ``conc_pass_end()``), not once per task;
* set-move destinations and the stopper take the tasks by their
  entitled mask, read through ``cpuset_task_cpus_rcu()``, a lockless
  snapshot of the cpuset's effective CPUs for callers that hold a raw
  lock;
* a task waking up to no usable CPU whose mask is behind its cpuset is
  brought up to date in the wakeup path (``conc_task_mask_behind()``) and
  selects again.

A cpuset narrower than the limit leaves the group with fewer CPUs than
its limit. A task whose new mask holds no CPU with room is held, and
the rotation serves it.

SCHED_DEADLINE
--------------

The deadline class admits a task's bandwidth against every CPU of its
root domain and schedules its tasks by global EDF over them. A limit of
N over some of a root domain's tasks would give them fewer CPUs than
they were admitted against, which is why a counting group refuses them
(see `Slow paths`_). Where a limit applies to all of a root domain's
tasks at once, the model holds with N in place of the root domain's CPU
count: global EDF on N CPUs that move within it. That is a *deadline
domain*: a valid partition root of the load-balanced kind, a root domain
of its own, carrying a limit, with no counting group above it, on CPUs of
equal capacity.

Its root domain is capped at its effective value (``conc_dl_set_cap()``,
``dl_conc_set_cpus()``, the ``conc_dl_cpus`` field of the root domain),
and the deadline class's own checks see the cap beside
``__dl_overflow()`` (``__dl_conc_overflow()``): the admission of a task
(``sched_dl_overflow()``), the bandwidth a cpuset move reserves
(``dl_bw_manage()``), a CPU going offline (``dl_bw_deactivate()``), a
partition shrinking (``dl_cpuset_cpumask_can_shrink()``), a fair server's
parameters and ``sched_rt_runtime_us``. The capped check compares the
tasks' bandwidth alone, the total less the servers of the active CPUs,
with N CPUs each keeping the share of the largest server, N bounded by
the capacity checked against: only the servers of the CPUs the tasks use
compete with them. Counting every CPU's server against N would leave a
wide partition with a small limit nearly nothing. The caps are set when
a limit is written and again after every rebuild of the root domains
(``conc_dl_rebuild_caps()``, from ``dl_rebuild_rd_accounting()``), both
under ``cpuset_mutex``.

A deadline task may be in a counting group only within a deadline domain
(``conc_dl_allowed()``): the task in the domain's root domain, not in a
partition below it; and no group between its own and the domain bounding
it to fewer CPUs. The masks of the other tasks of the domain are not
checked. A task with a narrower mask which finds no slot takes the slot
of a lower class at once, and never the slot of a deadline task from a
lower class (see `Rotation`_).

While a domain holds deadline bandwidth (``dl_conc_rd_busy()``) it is
pinned:

* a limit write lowering its effective value under the bandwidth
  admitted fails with ``EBUSY`` (``conc_set_request()``, checked by
  setting the cap first); raising or removing the limit is always safe,
  nothing else sharing the CPUs;
* a limit inside it bounding a group that holds a deadline task below the
  domain's value fails with ``EBUSY`` (``conc_subtree_dl()``);
* ``cpuset.cpus.partition`` of the domain cannot leave ``root``
  (``conc_dl_domain_busy()``, from ``cpuset_partition_write()``).

A limit starting over a subtree holding deadline tasks makes it a domain
when it can be one, its tasks' bandwidth fitting the new value, and fails
with ``EBUSY`` otherwise (``conc_subtree_dl()``). What cannot be
refused is the partition losing its root domain for other reasons, its
CPUs all going offline or its parent's changing under it: its deadline
tasks are then left in a counting group no domain admits, as the deadline
class leaves a partition's tasks in the root domain it falls back to,
without a check. Priority inheritance and proxy execution lend a
deadline task's bandwidth to a lock owner elsewhere, as they do across
root domains.

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
   bound below the user count cannot provide. No ID is handed out at or
   above the bound. A thread which keeps running while the bound is
   lowered below its ID does not schedule in. It is flagged and its CPU
   is interrupted (``cid_kick_over()``). It then drops that ID and takes
   a new one before it runs user space code again. A thread that finds
   none free below the bound at its
   schedule in (``cid_wait()``) takes one at its return to user space,
   or waits there for one (``sched_cid_resume()``, after a pending hold):
   it takes an ID under its runqueue lock as the schedule in would;
   failing that, gives the domain's cached IDs back from the CPUs that
   run no user of it (``cid_reclaim()``), to the waiters in order, itself
   maybe; failing that, sleeps queued as a waiter, killable as a hold is,
   so that a user's schedule out hands its CPU's ID to the first waiter
   rather than caching it (``cid_give()``) and the domain's irq work
   wakes the waiter, as a user's leave, an applied bound and an ID
   dropped back in the pool by a slot change (``cid_kick_waiters()``)
   wake them all to try again. The case is the hold's own window: a
   thread moved into the group while it runs on a CPU the group does
   not use keeps its ID until its return to user space, where it is
   held, and a sibling scheduling in on a used CPU meanwhile finds the
   pool short, and waits for that return. In the kernel, the thread's
   ``mm_cid`` reads ``MM_CID_UNSET`` while it waits, which a user of the
   ID there can test for; user space never sees it, the thread returns
   with its IDs or goes to its exit.

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
   ``cid_get()`` would fall back above it. The bound an mm gets is the
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

``ns_cid`` is a second ID. It is exposed through a new 32-bit ``struct
rseq`` field placed after ``slice_ctrl``. The field replaces the reserved
byte, as the comment of that byte foresaw. The ID is unique among the
tasks of a *concurrency ID namespace* that are running at the same
instant. It is below a maximum which is fixed when the namespace is
created. It lets processes of a container index shared memory by
concurrency, with tables sized once.

The namespace says *who shares an ID space*; the cgroup says *how many of
them may run at once*. The namespace lives in a cgroup, its *home*. The
home is the nearest limited cgroup at or above the cgroup where its first
member joins. The namespace takes two numbers from the cgroups at that
join: a hard maximum, copied once, and a soft limit, which the home keeps
up to date. Otherwise the namespace does not depend on the cgroups. Its
membership follows the namespace alone. A member keeps its ID wherever it
is moved within the home's subtree, which it cannot leave.

Decisions:

* The home is a namespace type of its own, ``CLONE_NEWCID``, with the
  nsfs type, ``/proc/<pid>/ns/cid``, ``setns(2)`` support and the
  ``max_cid_namespaces`` ucount the other types have. A namespace's kind
  is set by its type, not by a creation flag of another one, and a
  namespace is the only kernel object that says who shares something:
  no cgroup controller hands a task a number. The privilege is
  ``CAP_SYS_ADMIN`` in the user namespace that owns the new namespace,
  which a container runtime holds anyway. The flag bit lies in
  ``CSIGNAL``, as ``CLONE_NEWTIME``'s does: the bits above it are all
  spent, ``clone(2)`` reads the low byte as its exit signal and masks
  it out of its flags, so the bit is usable with ``clone3(2)``,
  ``unshare(2)`` and ``setns(2)``, which refused it before.
* *The home.* The nearest cgroup with a limit or a ceiling at or above the
  cgroup the namespace is created in, for the life of the namespace.
  That cgroup is the caller's for ``unshare(2)``, and the new child's
  for ``clone3(2)``. With ``clone3(CLONE_NEWCID | CLONE_INTO_CGROUP)`` a
  container runtime creates the container's init in the container's
  cgroup, whose ceiling and limit are the ones meant, not the runtime's
  own. No member may be in a cgroup outside the home's subtree: a
  migration out of it, a ``clone3(2)`` into a cgroup outside it and a
  ``setns(2)`` from outside it are refused with ``EBUSY``. That is what
  keeps the members
  placed by the limit that bounds their IDs. The home is decided at the
  creation, for good. A namespace created where no limit and no ceiling
  applies has no home, no soft limit and no boundary, and a member which joins later
  where a limit applies does not give it one. The home cannot be
  removed while the namespace exists: ``rmdir(2)`` of its cgroup, and
  a write to ``cgroup.subtree_control`` which disables the cpu
  controller for it, fail with ``EBUSY``. The file
  ``cpu.concurrency.namespaces`` of the home lists the namespaces
  living in it, and the ``NS_GET_CID_CGROUP`` ioctl on a namespace file
  descriptor returns the ID of the home. A namespace exists as long as
  a process uses it or keeps it for its children, or a file descriptor
  or a bind mount refers to it.
* *The maximum.* At the creation, the effective ceiling of the nearest
  cgroup with a ceiling at or above the cgroup of creation, or the
  number of possible CPUs when there is none, or the ceiling is no
  tighter. The nearest ceiling and not the outermost: limits nest, a
  host capping all its containers at 60 CPUs and each container at 2,
  and the ceiling the processes of a container size their tables by is
  the container's. Copied into the namespace, it never changes
  afterwards, as ceilings never do: the maximum is meant to behave like
  the possible-CPU count, which is what user space sizes its tables by,
  and it is read once, with ``prctl(PR_GET_NS_CID_MAX)``, which needs no
  mounted filesystem.
* *The soft limit.* The effective limit in force at the home, capped by
  the maximum; the maximum when the namespace has no home. Unlike the
  maximum it moves: the home feeds the namespace its effective value
  whenever it changes (``conc_ns_feed()``), so a limit written to the
  cgroup reaches the IDs the way it reaches ``mm_cid``, once the cgroup
  is within it, and raising it back widens them again. No ID is ever
  handed out at or above the soft limit: user space may read the home's
  effective file as the live size of the ID space, below the table it
  sized once by the maximum. There is one exception. A thread which
  runs when the soft limit is lowered keeps its ID until it is next
  scheduled in. For that short time it can read an ID at or above the
  new soft limit. That ID is still below the maximum, so it indexes the
  table within its size.
* *The join.* Time-namespace semantics: ``unshare(CLONE_NEWCID)`` and
  ``setns(2)`` set what the *children* get (``cid_ns_for_children`` in
  ``nsproxy``), committed at ``fork(2)`` and ``execve(2)`` the way
  ``timens_on_fork()`` does; ``setns(2)`` joins the caller as well and
  requires a single-threaded process (``EUSERS`` otherwise), as the time
  namespace does. Two consequences: a running program never sees its
  maximum change under a table it has already sized, and the threads of
  a process are never in different ID spaces. The namespace is flat: one
  space per task, no nesting, no translation, as with IPC and UTS.
* *Autonomy.* The members are the tasks with an mm whose ``nsproxy``
  names the namespace. The membership changes at ``fork(2)``, at the
  commit of a namespace switch and at exit, and nowhere else. A cgroup
  migration within the home's subtree neither tests nor touches it, and a
  member keeps its ID wherever it is moved. The concurrency of the members
  is bounded by the placement of their cgroups, as for every task. The IDs
  are bounded by the allocator, which hands out nothing at or above the
  soft limit. The two bounds agree. Every member is under the home, and
  the placement keeps the tasks of the home at or below its effective
  limit, which is the soft limit. So a member finds an ID free, except in
  two transition windows of the placement: a task is counted in a group
  before it is held, or a task is moved while it runs. Either can leave a
  member with no ID at its schedule in. That member takes one at its
  return to user space, or waits there for one. A member's schedule out
  hands the ID it frees to the waiters, in their order. An ID put in the
  pool would go to whichever member schedules in first. A member's leave,
  a raised limit or an ID put back in the pool wakes the waiters to try
  again. The wait is killable and as short-lived as a hold. A thread of a
  bounded mm takes the same path in the same window. Kernel
  threads are not members. Nor are the user workers of a process
  (io_uring's, vhost's), which never return to user space and take no
  ID of either kind. A member holds a reference on its namespace, so
  that ``task_struct::ns_dom`` stays valid whatever the order of the
  exit and namespace paths. ``task_struct`` caches the domain pointer so
  that the hot path never walks ``nsproxy``.
* The initial namespace has no engine. Its ID is the CPU number and its
  maximum is ``nr_cpu_ids``. The contract holds there too, because two
  tasks running at once are on different CPUs. This gives three things.
  The cost per context switch for a task that never asked is a static
  key, then a pointer test. User space has one code path everywhere: size
  by the maximum, index by the ID. A process can tell whether it is in
  the initial namespace: it compares ``/proc/self/ns/cid`` with
  ``/proc/1/ns/cid``, or reads the maximum.
* The engine is ``struct mm_mm_cid`` generalised into a cid domain
  (bitmap, per-CPU storage, task-owned and CPU-owned modes, the same
  ``max_cids`` formula with ``users`` being the member count) that the
  mm embeds and the namespace embeds. The bound of a namespace's domain
  is its soft limit. The bounds walk of the cgroup writes each new value
  and applies it as it visits the members. The domain's work applies it
  for a domain whose members the walk does not visit. The bound of an
  mm's domain follows the cgroups of its threads instead. A namespace's
  domain keeps no common ancestor group and asks the cgroups nothing at
  a fork, an exit or a migration. The
  hot path is a second schedin/schedout call for the members. A joining
  task widens the domain's allowed CPUs with its own mask, since the
  members of a namespace come from anywhere. The IDs a namespace caches
  on CPUs are given back on demand, by a member that found none free,
  rather than at every slot change of a cgroup; while a member waits,
  the members' schedule outs give their CPU's ID back instead of caching
  it.
* ``struct rseq`` and ``AT_RSEQ_FEATURE_SIZE`` are extended the way
  ``node_id`` and ``mm_cid`` were; the field is written for every
  registration large enough to hold it, which is what the feature size
  advertises, on every architecture. The RSEQ ABI V2 mode, which the
  architecture decides and not user space, gates the time slice
  extension because that needs the generic entry code; it must not gate
  an ID, or ``ns_cid`` would stay 0 on the architectures that have rseq
  without it while the feature size claims the field.

The contract. The kernel guarantees: the maximum never changes for the
life of a program; the IDs are unique among the concurrently running
members of a namespace, below the maximum, and below the soft limit in
force when the member was last scheduled in; a member in user space
always has one; a member is never outside
the home's subtree; ``mm_cid`` is untouched by all of this. User space
keeps: one table per namespace, sized once by the maximum; never index
it from two namespaces. The data sized by the maximum lives in shared
memory, and the kernel can enforce that the memory and the namespace
coincide: a memfd created with ``MFD_CID_NS``, with or without
``MFD_HUGETLB``, or a System V segment created with ``SHM_CID_NS``, is
bound to its creator's namespace, and only the namespace's processes map
it. The binding is the
inode's, made before any descriptor exists, so no mapping from outside
can race it. A map from outside fails with ``EPERM``, whoever holds the
descriptor; a mapping of a bound object is never inherited by a
``fork(2)`` child, which inherits the descriptor and maps the object for
itself (the mapping has the ``MADV_DONTFORK`` property from its creation,
and ``MADV_DOFORK`` is refused on it), so a child born into another
namespace after ``unshare(CLONE_NEWCID)`` has nothing to lose;
``setns(2)`` fails with ``EBUSY`` while the caller maps a bound object,
which it unmaps first; ``execve(2)`` starts from a fresh mm. So a mapping
of a bound object belongs to a member at every instant user code runs,
and a table sized by the maximum is never indexed from another space.
Kernel-side access to a member's memory, ``ptrace(2)`` or
``process_vm_writev(2)`` from a debugger outside, is not a mapping and
keeps working: the hazard is a user instruction indexing with an ID of
another space. Binding to the initial namespace is allowed, and means
initial-namespace processes only. No fault path is involved: the checks
are at ``mmap`` and ``madvise``, the one walk of an mm is at ``setns(2)``.

How it is built:

* ``kernel/sched/cid_namespace.c`` is the namespace type, modelled on
  ``kernel/time/namespace.c``: ``copy_cid_ns()`` sets the domain up with
  no bound, ``cidns_on_fork()`` commits the children's namespace,
  ``cidns_install()`` is ``setns(2)``, refused from outside the home's
  subtree; the membership is in ``kernel/sched/cid.c``.
* Membership is driven by the namespace switch itself.
  ``switch_task_namespaces()`` leaves the old domain and joins the new
  one before the nsproxy changes hands. Exit leaves with the mm. A
  ``fork(2)`` child is made a member in ``sched_post_fork()``. The first
  join gives the namespace its home, its maximum and its soft limit
  (``conc_ns_home()``), before any ID is taken. In per-task mode a
  joining user takes its ID in the join. In per CPU mode it takes none:
  it borrows the ID of the CPU it runs on at its next schedule in. A task
  that joins while it is running reaches user space before that. So it
  takes its ID in the join itself, the way a schedule in would. The
  running joiners are ``setns(2)``, and ``execve(2)`` after
  ``unshare(CLONE_NEWCID)``.
* The refusals are made in the cpu controller: a migration in
  ``conc_attach_check()``, from the cpu controller's ``can_attach``, and
  a child born into a cgroup, or a plain one born to a parent moved
  since, in ``conc_fork_check()``, from ``sched_cgroup_fork()``, both
  against the task's namespace and the task group it would be in
  (``conc_ns_may_live()``).
* The soft limit is fed from the cgroup's bounds walk, the same pass
  that refreshes the ``mm_cid`` bounds of the tasks in the subtree: the
  home keeps the namespaces living in it on a list, under a lock of
  its own, and the walk writes each its new value, then applies the
  domain's bound (``cid_apply_bound()``) as it visits the members, the
  way it applies an mm's, before the effective file shows the new
  value; a domain the walk finds no member of applies it from its work.
  The removal of the home takes the namespaces off the list.
* A member that found no ID free at its schedule in takes the path a
  thread of a bounded mm takes (``cid_wait()``, ``sched_cid_resume()``):
  the same waiters, hand-offs and wake-ups, on the namespace's domain.
* The memory binding is a pointer in ``shmem_inode_info`` and in
  ``hugetlbfs_inode_info`` holding a namespace reference, set by
  ``memfd_alloc_file()`` and ``newseg()`` (``cid_ns_bind_file()``) and
  dropped at the inode's eviction; ``shmem_mmap_prepare()`` and ``hugetlbfs_file_mmap()``
  ask ``cid_ns_may_map()``, which System V segments reach through their
  underlying file, and give the mapping ``VMA_DONTCOPY``, which
  ``madvise(MADV_DOFORK)`` refuses to clear on a bound mapping;
  ``cidns_install()`` walks the caller's mm and refuses while it maps a
  bound object.
* ``ns_cid`` is written to the rseq area of every registration larger
  than the original 32 bytes, the only lengths ``rseq(2)`` admits being
  that one and the ones that hold the whole field, validated like
  ``mm_cid`` in debug mode, reset at unregistration, and carried in
  ``struct rseq_ids`` so that a change raises the IDs update; a
  namespace switch and a wait for an ID force one.
* The namespace and the cpu controller say different things. The
  namespace says who shares and the cgroup says how many may run. The
  cpu controller gives the namespace its maximum and its soft limit, and
  keeps its members in one cgroup subtree. The maximum is copied once.
  The soft limit is fed through a list the cgroup owns. The boundary is
  checked in the controller's ``can_attach``. No lock is
  taken across the subsystems and no reference is held either way.

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
  domain whose group has no slot on it (``conc_debug_cid_kept()``).

The end of a wait for a slot also reports, at the info level, the
longest one so far from 10 ms on. A wait has no upper bound under
rotation, so no check warns about it.

The first two are what a lost slot claim leaves (see `Lockless fast
paths`_), the second when the task leaves the CPU without being switched
there again: the group present on more CPUs than its used set, beyond its
limit, its IDs past the bound. The third is an ID that nothing drops any
more, cached outside the used set. They show well before an ID above the
bound is handed out, which takes the space below it exhausted as well.

Simplifications
===============

The implementation is kept simple in these ways:

* The fair class's wakeup, fork and exec placement is constrained to the
  used set through a per-CPU selection mask; the RT and deadline searches
  are constrained in ``find_lowest_rq()`` and ``find_later_rq()``, after
  ``cpupri`` and ``cpudl`` chose a priority level. The post-filter in
  ``select_task_rq()`` remains as the safety net (see `Selection`_).
* The spread of slots over SMT cores is applied by the class searches and
  the load balancer, not at the claim: a wakeup's previous CPU taken as
  is, the CPU a task attaches on and
  an rt or deadline search left with siblings only may still add a slot
  on the sibling of a used CPU. The cleaner frees such slots first when it
  makes room.
* A task that finds no used CPU in its mask, and no room, goes
  without a slot and is held at its return to user space, see `Holding`_.
  During a set-move that case lands on the evicting source, where the IDs
  still are, as a late arrival the stopper picks up. A task pinned to
  CPUs going offline alone that wakes up before
  the unplug widens it is widened at that wakeup, the way
  ``select_fallback_rq()`` would widen it later, rather than placed without
  a slot on a dying CPU. An affinity change to a mask whose only active
  CPUs are going offline is still honoured there, as it would be without a
  limit, and the unplug holds the task; no room is made for it, nor for a
  change to a mask with no active CPU, which ``sched_setaffinity(2)``
  refuses as it does without a limit. A set-move chooses its
  destination by the tasks the stopper moves, the queued ones, and may land
  on a CPU the group uses already, merging the presence, which is what a
  cpuset change needs when the applied masks lag behind it and no task can
  join by its own mask.
* The class of a slot is computed by a walk of the tasks of the groups
  which use its CPU (``conc_slot_class()``). The walk is done for a slot
  which is due for a rotation, or when the held task is above the lowest
  class. It costs nothing on the paths which claim and release slots.
* The rotation period is one value for the system, in debugfs.
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
  fails only when it still differs from the limit.
