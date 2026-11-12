=====================
Restartable Sequences
=====================

Restartable Sequences allow to register a per thread userspace memory area
to be used as an ABI between kernel and userspace for three purposes:

 * userspace restartable sequences

 * quick access to read the current CPU number, node ID and concurrency ID
   from userspace

 * scheduler time slice extensions

The rseq area
-------------

A thread registers one area of memory with the rseq() system call::

    int rseq(struct rseq *rseq, u32 rseq_len, int flags, u32 sig);

``struct rseq`` is defined in ``include/uapi/linux/rseq.h``. The kernel
writes to the area when it returns to user space. User space reads the
fields with plain loads, and stores a pointer in one of them.

A thread can have one registration. The C library usually owns it: glibc
registers an area for each thread since version 2.35. It exports the place
and the size of that area in ``__rseq_offset``, ``__rseq_size`` and
``__rseq_flags``. Code that wants to use rseq must use the area of the C
library when there is one. A second registration fails.

The arguments are:

``rseq``
    The address of the area.

``rseq_len``
    The size of the area, see `Size and alignment of the area`_.

``flags``
    0 to register. ``RSEQ_FLAG_UNREGISTER`` to unregister.
    ``RSEQ_FLAG_SLICE_EXT_DEFAULT_ON`` registers with the time slice
    extension enabled, see `Scheduler time slice extensions`_.

``sig``
    A 32-bit signature. User space places it before each abort handler,
    see `The signature`_. An unregistration must give the same value.

The fields of the area are:

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Field
     - Since
     - Use
   * - 0
     - 4
     - ``cpu_id_start``
     - 4.18
     - The CPU number of the thread. Written by the kernel.
   * - 4
     - 4
     - ``cpu_id``
     - 4.18
     - The CPU number of the thread, or a negative value when the area is
       not registered. Written by the kernel.
   * - 8
     - 8
     - ``rseq_cs``
     - 4.18
     - The address of the descriptor of the critical section that the
       thread runs, or 0. Written by user space. Read and cleared by the
       kernel.
   * - 16
     - 4
     - ``flags``
     - 4.18
     - Feature bits. Written by the kernel.
   * - 20
     - 4
     - ``node_id``
     - 6.3
     - The NUMA node of the CPU. Written by the kernel.
   * - 24
     - 4
     - ``mm_cid``
     - 6.3
     - The concurrency ID of the thread. Written by the kernel.
   * - 28
     - 4
     - ``slice_ctrl``
     - 7.0
     - The request and the grant of a time slice extension. Written by
       both.

The kernel writes the fields with single stores. Another thread may read
the CPU number of a thread from its area, and gets a consistent value.

Size and alignment of the area
------------------------------

The area can grow. A new kernel can add fields at its end. Two entries of
the ELF auxiliary vector say what the running kernel needs:

``AT_RSEQ_FEATURE_SIZE``
    The number of bytes of the area that the kernel uses.

``AT_RSEQ_ALIGN``
    The alignment of an area that is not 32 bytes long. It is the feature
    size rounded up to a power of two.

User space reads both with getauxval(3). The kernel accepts two kinds of
registration:

 * ``rseq_len`` is 32, and the area is aligned on 32 bytes. This is the
   size of the original ``struct rseq``. Every kernel accepts it.

 * ``rseq_len`` is larger than 32. It must be at least
   ``AT_RSEQ_FEATURE_SIZE``, and the area must be aligned on
   ``AT_RSEQ_ALIGN``.

Any other size or alignment fails with ``EINVAL``. So code that allocates
the area must read the auxiliary vector at run time. It cannot use a size
chosen at build time, other than 32. In particular ``sizeof(struct rseq)``
is not a valid size for an object of that type: the type is aligned on 32
bytes, and its size is larger than 32.

An area may be larger than the feature size. The kernel does not touch the
bytes after its feature size.

A field can be used when the registered feature size covers it::

    available = feature_size >= offsetof(struct rseq, field) + sizeof(field)

For the area of glibc, the feature size is ``__rseq_size``. There is one
exception. glibc 2.35 to 2.39 set ``__rseq_size`` to 32 although only the
first 20 bytes were in use. So a value of 32 means 20. The feature size of
the kernel is never 32, to keep this exception the only one.

Registration and unregistration
-------------------------------

A registration writes the area before it returns:

 * ``cpu_id_start`` and ``cpu_id`` hold the CPU number of the thread.
 * ``node_id`` and ``mm_cid`` hold their current values.
 * ``rseq_cs`` is cleared, whatever it held.
 * ``flags`` holds the feature bits.

An unregistration sets ``cpu_id_start`` to 0, ``cpu_id`` to
``RSEQ_CPU_ID_UNINITIALIZED`` (-1), and ``node_id`` and ``mm_cid`` to 0. It
leaves ``flags`` as it was. The memory of an area can be freed after the
unregistration, or after the thread has exited.

rseq() fails with:

========= ==============================================================
Errorcode Meaning
========= ==============================================================
EINVAL    Unknown flags. A size or an alignment that is not valid. A
          registration with another address or another size than the
          current one. An unregistration with another address or another
          size than the current one, or without a registration.
EPERM     The signature differs from the one of the current registration.
EBUSY     The same area is registered already, with the same signature.
EFAULT    The area is not writable memory of the process.
ENOSYS    The kernel was built without rseq.
========= ==============================================================

The address and the size are checked before the signature.

The area must stay mapped and writable while it is registered, see `When
the kernel sends SIGSEGV`_.

fork(), clone() and execve()
----------------------------

 * A child that gets a copy of the memory, as after fork(), inherits the
   registration. The kernel updates the copy of the child.

 * A child that shares the memory of its parent (``CLONE_VM``) starts
   without a registration. A new thread must register its own area. A
   vfork() child runs on the thread area of its parent until it calls
   execve() or _exit(). The rseq area it sees there is not updated for it.

 * execve() drops the registration.

ptrace() reports the registration of a tracee with
``PTRACE_GET_RSEQ_CONFIGURATION``: the address, the size and the signature.
A checkpoint and restore tool can register the same area again with them.

CPU number, node ID and concurrency ID
--------------------------------------

``cpu_id`` holds the number of the CPU that the thread runs on. Reading it
is cheaper than a system call. The value can be stale as soon as it is
read, unless the read is part of a critical section.

``cpu_id_start`` holds the same number. It differs in an area that is not
registered: it is then 0, a valid index, where ``cpu_id`` is negative. User
space can initialize both fields that way in an area that is not registered
yet. A critical section loads ``cpu_id_start`` first and uses it as an
index. It then compares it with ``cpu_id`` inside the critical section.
Without a registration the comparison fails, and the code takes its
fallback.

``node_id`` holds the NUMA node of that CPU.

``mm_cid`` holds the concurrency ID of the thread. No two threads that run
at the same time in one address space have the same ID. The ID is below the
number of possible CPUs. The kernel also tries to keep the IDs small. With
few threads, or with few allowed CPUs, they stay close to 0. That is a
target and not a bound: an ID can be above the number of threads. Per-CPU
data indexed by ``mm_cid`` is therefore sized for the number of possible
CPUs, and the small IDs keep most of it untouched.

User space must not write these four fields. See `Optimized RSEQ V2`_ for
how the kernel enforces it.

Restartable sequences (per-cpu atomics)
---------------------------------------

Restartable sequences allow userspace to perform update operations on
per-cpu data without requiring heavyweight atomic operations.

A restartable sequence, or critical section, is a short piece of machine
code. It ends with one instruction that commits the update. The kernel
guarantees that the critical section either runs to its end, or is aborted.
It is aborted when the thread is preempted, migrated, or interrupted by one
of its signal handlers in the middle. The kernel then moves the thread to
an abort handler before it runs user code again. The commit did not happen,
and the abort handler can try again.

Each critical section has a descriptor::

    struct rseq_cs {
        __u32 version;              /* 0 */
        __u32 flags;                /* 0 */
        __u64 start_ip;             /* first instruction */
        __u64 post_commit_offset;   /* length, in bytes */
        __u64 abort_ip;             /* abort handler */
    };

The code flow is::

                      cpu = rseq->cpu_id_start
    [1]               rseq->rseq_cs = &descriptor
    [start_ip]        ----------------------------
    [2]               if (cpu != rseq->cpu_id)
                              goto abort_ip
    [3]               <commit instruction>
    [post_commit_ip]  ----------------------------

 1. User space stores the address of the descriptor in ``rseq_cs``. This is
    one store. The critical section starts right after it.

 2. It compares the CPU number it loaded with ``cpu_id``. If they differ,
    the thread was migrated before the critical section started.

 3. The last instruction commits. It is a single store, for instance of a
    pointer or of a counter.

The kernel looks at ``rseq_cs`` before it returns to user space after a
preemption, and before it delivers a signal:

 * If the instruction pointer is in [start_ip, start_ip +
   post_commit_offset), the kernel clears ``rseq_cs`` and sets the
   instruction pointer to ``abort_ip``.

 * Otherwise the kernel clears ``rseq_cs``.

User space does not have to clear ``rseq_cs`` after the commit. The kernel
clears it the next time it looks. User space must clear it before the
memory of the descriptor goes away, for instance before it unloads a
shared object.

The rules for a critical section are:

 * It is written in assembly. A compiler does not guarantee where the
   commit instruction ends up.

 * It makes no system call.

 * It does nothing before the commit that cannot be done twice.

 * ``abort_ip`` is outside of the critical section.

 * The signature comes right before ``abort_ip``.

 * ``version`` and ``flags`` are 0. The ``RSEQ_CS_FLAG_NO_RESTART_ON_*``
   flags are not supported since Linux 6.0.

The selftests in ``tools/testing/selftests/rseq`` have critical sections
for each architecture, for instance a per-CPU counter, a per-CPU list and a
per-CPU buffer.

The signature
~~~~~~~~~~~~~

The four bytes before ``abort_ip`` must hold the signature given at
registration. If they do not, the kernel does not jump: it sends SIGSEGV.

This protects a process whose memory an attacker can partly write. Without
the check, the attacker could make a descriptor, point ``rseq_cs`` at it,
and have the kernel jump to any address. With the check, the kernel only
jumps to an address that follows the signature. An architecture picks a
signature that is not a valid instruction, or that traps.

When the kernel sends SIGSEGV
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The kernel sends SIGSEGV when it finds:

 * an area that it cannot read or write;

 * a ``rseq_cs`` that is not the address of readable user memory;

 * an ``abort_ip`` that is not a user address, or without the signature;

 * in the V2 mode, a changed read-only field.

The kernel forces the signal: the thread cannot block or ignore it. A
handler for SIGSEGV runs, as for any other fault. A process without such a
handler is killed.

The debug mode adds checks, see `Debugging`_.

Debugging
---------

The debug mode of rseq makes the kernel check more, at a cost. It is off by
default, unless ``CONFIG_RSEQ_DEBUG_DEFAULT_ENABLE`` is set. It is switched
with the ``rseq_debug=`` boot parameter, or at run time by writing 0 or 1
to ``rseq/debug`` in debugfs. It is a switch for the whole system.

In the debug mode the kernel also sends SIGSEGV to a process when:

 * it makes a system call inside a critical section;

 * a descriptor has a ``version`` or ``flags`` that is not 0;

 * ``abort_ip`` is inside the critical section;

 * the critical section wraps around, or reaches outside the user address
   space;

 * it changed a read-only field, in the legacy mode too.

In the debug mode the kernel also looks at ``rseq_cs`` at each return from
a system call, and clears it when the thread is outside of the critical
section.

With ``CONFIG_RSEQ_STATS``, ``rseq/stats`` in debugfs counts the events:
exits to user space, ID updates, critical sections looked at, cleared and
aborted, and the time slice extensions.

Two tracepoints follow a thread: ``rseq_update`` when the kernel writes the
IDs, and ``rseq_ip_fixup`` when it aborts a critical section.

membarrier()
------------

``MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ`` makes the kernel look at the
``rseq_cs`` of every running thread of the process, or of the threads on
one CPU. A thread inside a critical section is aborted. A process can use
it to update per-CPU data of another CPU: after the call, no thread is
still inside a critical section that started before it.

Optimized RSEQ V2
-----------------

On architectures which utilize the generic entry code and generic TIF bits
the kernel supports runtime optimizations for RSEQ, which also enable
enhanced features like scheduler time slice extensions.

To enable them a task has to register the RSEQ region with at least the
length advertised by getauxval(AT_RSEQ_FEATURE_SIZE). On the other
architectures such a registration works, in the legacy mode. No field of
the area tells which mode a registration got.

If existing binaries register with RSEQ_ORIG_SIZE (32 bytes), the kernel
keeps the legacy low performance mode enabled to fulfil the expectations
of existing users regarding the original RSEQ implementation behaviour.

The following table documents the ABI and behavioral guarantees of the
legacy and the optimized V2 mode.

.. list-table:: RSEQ modes
   :header-rows: 1

   * - Nr
     - What

     - Legacy
     - Optimized V2

   * - 1
     - The cpu_id_start, cpu_id, node_id and mm_cid fields (User mode read
       only)
       .. Legacy
     - Updated by the kernel unconditionally after each context switch and
       before signal delivery
       .. Optimized V2
     - Updated by the kernel if and only if they change, i.e. if the task
       is migrated or mm_cid changes

   * - 2
     - The rseq_cs critical section field
       .. Legacy
     - Evaluated and handled when user space was interrupted and was
       scheduled out, and before each signal delivery. A system call is
       not allowed inside a critical section, so there is nothing to
       handle after a context switch in a system call. Architectures
       without the generic entry code evaluate it after each context
       switch.
       .. Optimized V2
     - Evaluated and handled conditionally only when user space was
       interrupted and was scheduled out or before delivering a signal in
       the interrupted context.

   * - 3
     - Read only fields
       .. Legacy
     - No strict enforcement except in debug mode
       .. Optimized V2
     - Strict enforcement

   * - 4
     - membarrier(...RSEQ)
       .. Legacy
     - All running threads of the process are interrupted and the ID fields
       are rewritten and eventually active critical sections are aborted
       before they return to user space.  All threads which are scheduled
       out whether voluntary or not are covered by #1/#2 above.
       .. Optimized V2
     - All running threads of the process are interrupted and eventually
       active critical sections are aborted before these threads return to
       user space. The ID fields are only updated if changed as a
       consequence of the interrupt. All threads which are scheduled out
       whether voluntary or not are covered by #1/#2 above.

   * - 5
     - Time slice extensions
       .. Legacy
     - Not supported
       .. Optimized V2
     - Supported

The legacy mode is obviously less performant as it does unconditional
updates and critical section checks even if not strictly required by the
ABI contract. That can't be changed anymore as some users depend on that
observed behavior, which in turn enables them to violate the ABI and
overwrite the cpu_id_start field for their own purposes. This is obviously
discouraged as it renders RSEQ incompatible with the intended usage and
breaks the expectation of other libraries in the same application.

The ABI compliant optimized v2 mode, which respects the read only fields,
does not require unconditional updates and therefore is way more
performant. The kernel validates the read only fields for compliance. If
user space modifies them, the process is killed. Compliant usage allows
multiple libraries in the same application to benefit from the RSEQ
functionality without disturbing each other. The ABI compliant optimized v2
mode also enables extended RSEQ features like time slice extensions.


Scheduler time slice extensions
-------------------------------

This allows a thread to request a time slice extension when it enters a
critical section to avoid contention on a resource when the thread is
scheduled out inside of the critical section.

The prerequisites for this functionality are:

    * Enabled in Kconfig

    * Enabled at boot time (default is enabled)

    * A rseq userspace pointer has been registered for the thread in
      optimized V2 mode

The thread has to enable the functionality via prctl(2)::

    prctl(PR_RSEQ_SLICE_EXTENSION, PR_RSEQ_SLICE_EXTENSION_SET,
          PR_RSEQ_SLICE_EXT_ENABLE, 0, 0);

A thread can also register with ``RSEQ_FLAG_SLICE_EXT_DEFAULT_ON``. The
functionality is then enabled from the start. The flag does nothing in a
legacy registration, or when the functionality is not available.

The state belongs to the registration. A fork() child keeps it. A new
registration starts disabled.

prctl() returns 0 on success or otherwise with the following error codes:

========= ==============================================================
Errorcode Meaning
========= ==============================================================
EINVAL	  Functionality not available or invalid function arguments.
          Note: arg4 and arg5 must be zero
ENOTSUPP  Functionality was disabled on the kernel command line, the
          kernel was built without it, or the rseq user struct is
          registered in the legacy mode. Note: this is error number 524,
          which the C library has no name for.
ENXIO	  Available, but no rseq user struct registered
========= ==============================================================

The state can be also queried via prctl(2)::

  prctl(PR_RSEQ_SLICE_EXTENSION, PR_RSEQ_SLICE_EXTENSION_GET, 0, 0, 0);

prctl() returns ``PR_RSEQ_SLICE_EXT_ENABLE`` when it is enabled or 0 if
disabled. Otherwise it returns with the following error codes:

========= ==============================================================
Errorcode Meaning
========= ==============================================================
EINVAL	  Functionality not available or invalid function arguments.
          Note: arg3 and arg4 and arg5 must be zero
========= ==============================================================

The availability and status is also exposed via the rseq ABI struct flags
field via the ``RSEQ_CS_FLAG_SLICE_EXT_AVAILABLE_BIT`` and the
``RSEQ_CS_FLAG_SLICE_EXT_ENABLED_BIT``. These bits are read-only for user
space and only for informational purposes. An unregistration leaves them
as they were.

If the mechanism was enabled via prctl(), the thread can request a time
slice extension by setting rseq::slice_ctrl::request to 1. If the thread is
interrupted and the interrupt results in a reschedule request in the
kernel, then the kernel can grant a time slice extension and return to
userspace instead of scheduling out. The length of the extension is
determined by debugfs:rseq/slice_ext_nsec. The default value is 5 usec; which
is the minimum value. It can be incremented to 50 usecs, however doing so
can/will affect the minimum scheduling latency.

Any proposed changes to this default will have to come with a selftest and
rseq-slice-hist.py output that shows the new value has merit.

The kernel indicates the grant by clearing rseq::slice_ctrl::request and
setting rseq::slice_ctrl::granted to 1. If there is a reschedule of the
thread after granting the extension, the kernel clears the granted bit to
indicate that to userspace.

If the request bit is still set when leaving the critical section,
userspace can clear it and continue.

If the granted bit is set, then userspace invokes rseq_slice_yield(2) when
leaving the critical section to relinquish the CPU. The kernel enforces
this by arming a timer to prevent misbehaving userspace from abusing this
mechanism.

If both the request bit and the granted bit are false when leaving the
critical section, then this indicates that a grant was revoked and no
further action is required by userspace.

The required code flow is as follows::

    rseq->slice_ctrl.request = 1;
    barrier();  // Prevent compiler reordering
    critical_section();
    barrier();  // Prevent compiler reordering
    rseq->slice_ctrl.request = 0;
    if (rseq->slice_ctrl.granted)
        rseq_slice_yield();

As all of this is strictly CPU local, there are no atomicity requirements.
Checking the granted state is racy, but that cannot be avoided at all::

    if (rseq->slice_ctrl.granted)
      -> Interrupt results in schedule and grant revocation
        rseq_slice_yield();

So there is no point in pretending that this might be solved by an atomic
operation.

If the thread issues a syscall other than rseq_slice_yield(2) within the
granted timeslice extension, the grant is also revoked and the CPU is
relinquished immediately when entering the kernel. This is required as
syscalls might consume arbitrary CPU time until they reach a scheduling
point when the preemption model is either NONE or VOLUNTARY and therefore
might exceed the grant by far.

The preferred solution for user space is to use rseq_slice_yield(2) which
is side effect free. The support for arbitrary syscalls is required to
support onion layer architectured applications, where the code handling the
critical section and requesting the time slice extension has no control
over the code within the critical section.

The kernel enforces flag consistency and terminates the thread with SIGSEGV
if it detects a violation.
