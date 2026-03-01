/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_RSEQ_H
#define _UAPI_LINUX_RSEQ_H

/*
 * linux/rseq.h
 *
 * Restartable sequences system call API
 *
 * Copyright (c) 2015-2018 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/types.h>
#include <asm/byteorder.h>

enum rseq_cpu_id_state {
	RSEQ_CPU_ID_UNINITIALIZED		= -1,
	RSEQ_CPU_ID_REGISTRATION_FAILED		= -2,
};

enum rseq_flags {
	RSEQ_FLAG_UNREGISTER			= (1 << 0),
	RSEQ_FLAG_SLICE_EXT_DEFAULT_ON		= (1 << 1),
};

enum rseq_cs_flags_bit {
	/* Historical and unsupported bits */
	RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT	= 0,
	RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT	= 1,
	RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT	= 2,
	/* (3) Intentional gap to put new bits into a separate byte */

	/* User read only feature flags */
	RSEQ_CS_FLAG_SLICE_EXT_AVAILABLE_BIT	= 4,
	RSEQ_CS_FLAG_SLICE_EXT_ENABLED_BIT	= 5,
};

enum rseq_cs_flags {
	RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT	=
		(1U << RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT),
	RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL	=
		(1U << RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT),
	RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE	=
		(1U << RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT),

	RSEQ_CS_FLAG_SLICE_EXT_AVAILABLE	=
		(1U << RSEQ_CS_FLAG_SLICE_EXT_AVAILABLE_BIT),
	RSEQ_CS_FLAG_SLICE_EXT_ENABLED		=
		(1U << RSEQ_CS_FLAG_SLICE_EXT_ENABLED_BIT),
};

/*
 * struct rseq_cs is aligned on 4 * 8 bytes to ensure it is always
 * contained within a single cache-line. It is usually declared as
 * link-time constant data.
 */
struct rseq_cs {
	/* Version of this structure. */
	__u32 version;
	/* enum rseq_cs_flags */
	__u32 flags;
	__u64 start_ip;
	/* Offset from start_ip. */
	__u64 post_commit_offset;
	__u64 abort_ip;
} __attribute__((aligned(4 * sizeof(__u64))));

/**
 * rseq_slice_ctrl - Time slice extension control structure
 * @all:	Compound value
 * @request:	Request for a time slice extension
 * @granted:	Granted time slice extension
 *
 * @request is set by user space and can be cleared by user space or kernel
 * space.  @granted is set and cleared by the kernel and must only be read
 * by user space.
 */
struct rseq_slice_ctrl {
	union {
		__u32		all;
		struct {
			__u8	request;
			__u8	granted;
			__u16	__reserved;
		};
	};
};

/**
 * rseq_rl_cs - Robust list unlock transaction descriptor
 *
 * rseq_rl_cs describes a transaction which begins with a successful
 * robust mutex unlock followed by clearing a robust list pending ops.
 *
 * Userspace prepares for a robust_list unlock transaction by storing
 * the address of a struct rseq_rl_cs descriptor into its per-thread
 * rseq area rseq_rl_cs field. After the transaction is over, userspace
 * clears the rseq_rl_cs pointer.
 *
 * A thread is considered to be within a rseq_rl_cs transaction if
 * either of those conditions are true:
 *
 * - ip >= post_cond_store_ip && ip < post_success_ip && ll_sc_success(pt_regs)
 * - ip >= post_success_ip && ip < post_clear_op_pending_ip
 *
 * If the kernel terminates a process within an active robust list
 * unlock transaction, it should consider the robust list op pending
 * as empty even if it contains an op pending address.
 */
struct rseq_rl_cs {
	/* Version of this structure. */
	__u32 version;
	/* Reserved flags. */
	__u32 flags;
	/*
	 * Address immediately after store which unlocks the robust
	 * mutex. This store is usually implemented with an atomic
	 * exchange, or linked-load/store-conditional. In case it is
	 * implemented with ll/sc, the kernel needs to check whether the
	 * conditional store has succeeded with the appropriate registers
	 * or flags, as defined by the architecture ABI.
	 */
	__u64 post_cond_store_ip;
	/*
	 * For architectures implementing atomic exchange as ll/sc,
	 * a conditional branch is needed to handle failure.
	 * The unlock success IP is the address immediately after
	 * the conditional branch instruction after which the kernel
	 * can assume that the ll/sc has succeeded without checking
	 * registers or flags. For architectures where the the mutex
	 * unlock store instruction cannot fail, this address is equal
	 * to post_cond_store_ip.
	 */
	__u64 post_success_ip;
	/*
	 * Address after the instruction which clears the op pending
	 * list. This store is the last instruction of this sequence.
	 */
	__u64 post_clear_op_pending_ip;
} __attribute__((aligned(4 * sizeof(__u64))));

/*
 * struct rseq is aligned on 4 * 8 bytes to ensure it is always
 * contained within a single cache-line.
 *
 * A single struct rseq per thread is allowed.
 */
struct rseq {
	/*
	 * Restartable sequences cpu_id_start field. Updated by the
	 * kernel. Read by user-space with single-copy atomicity
	 * semantics. This field should only be read by the thread which
	 * registered this data structure. Aligned on 32-bit. Always
	 * contains a value in the range of possible CPUs, although the
	 * value may not be the actual current CPU (e.g. if rseq is not
	 * initialized). This CPU number value should always be compared
	 * against the value of the cpu_id field before performing a rseq
	 * commit or returning a value read from a data structure indexed
	 * using the cpu_id_start value.
	 */
	__u32 cpu_id_start;
	/*
	 * Restartable sequences cpu_id field. Updated by the kernel.
	 * Read by user-space with single-copy atomicity semantics. This
	 * field should only be read by the thread which registered this
	 * data structure. Aligned on 32-bit. Values
	 * RSEQ_CPU_ID_UNINITIALIZED and RSEQ_CPU_ID_REGISTRATION_FAILED
	 * have a special semantic: the former means "rseq uninitialized",
	 * and latter means "rseq initialization failed". This value is
	 * meant to be read within rseq critical sections and compared
	 * with the cpu_id_start value previously read, before performing
	 * the commit instruction, or read and compared with the
	 * cpu_id_start value before returning a value loaded from a data
	 * structure indexed using the cpu_id_start value.
	 */
	__u32 cpu_id;
	/*
	 * Restartable sequences rseq_cs field.
	 *
	 * Contains NULL when no critical section is active for the current
	 * thread, or holds a pointer to the currently active struct rseq_cs.
	 *
	 * Updated by user-space, which sets the address of the currently
	 * active rseq_cs at the beginning of assembly instruction sequence
	 * block, and set to NULL by the kernel when it restarts an assembly
	 * instruction sequence block, as well as when the kernel detects that
	 * it is preempting or delivering a signal outside of the range
	 * targeted by the rseq_cs. Also needs to be set to NULL by user-space
	 * before reclaiming memory that contains the targeted struct rseq_cs.
	 *
	 * Read and set by the kernel. Set by user-space with single-copy
	 * atomicity semantics. This field should only be updated by the
	 * thread which registered this data structure. Aligned on 64-bit.
	 *
	 * 32-bit architectures should update the low order bits of the
	 * rseq_cs field, leaving the high order bits initialized to 0.
	 */
	__u64 rseq_cs;

	/*
	 * Restartable sequences flags field.
	 *
	 * This field was initially intended to allow event masking for
	 * single-stepping through rseq critical sections with debuggers.
	 * The kernel does not support this anymore and the relevant bits
	 * are checked for being always false:
	 *	- RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT
	 *	- RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL
	 *	- RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE
	 */
	__u32 flags;

	/*
	 * Restartable sequences node_id field. Updated by the kernel. Read by
	 * user-space with single-copy atomicity semantics. This field should
	 * only be read by the thread which registered this data structure.
	 * Aligned on 32-bit. Contains the current NUMA node ID.
	 */
	__u32 node_id;

	/*
	 * Restartable sequences mm_cid field. Updated by the kernel. Read by
	 * user-space with single-copy atomicity semantics. This field should
	 * only be read by the thread which registered this data structure.
	 * Aligned on 32-bit. Contains the current thread's concurrency ID
	 * (allocated uniquely within a memory map).
	 */
	__u32 mm_cid;

	/*
	 * Time slice extension control structure. CPU local updates from
	 * kernel and user space.
	 */
	struct rseq_slice_ctrl slice_ctrl;

	/*
	 * Restartable sequences rseq_rl_cs field.
	 *
	 * Contains NULL when no robust list unlock transaction is
	 * active for the current thread, or holds a pointer to the
	 * currently active struct rseq_rl_cs.
	 *
	 * Updated by user-space, which sets the address of the currently
	 * active rseq_rl_cs at some point before the beginning of the
	 * transaction, and set to NULL by user-space at some point
	 * after the transaction has completed.
	 *
	 * Read by the kernel. Set by user-space with single-copy
	 * atomicity semantics. This field should only be updated by the
	 * thread which registered this data structure. Aligned on
	 * 64-bit.
	 *
	 * 32-bit architectures should update the low order bits of the
	 * rseq_cs field, leaving the high order bits initialized to 0.
	 */
	__u64 rseq_rl_cs;

	/*
	 * Flexible array member at end of structure, after last feature field.
	 */
	char end[];
} __attribute__((aligned(4 * sizeof(__u64))));

#endif /* _UAPI_LINUX_RSEQ_H */
