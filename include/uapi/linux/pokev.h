/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
/*
 * Header file for the pokev interface.
 *
 * Copyright (C) 2025 Mathieu Desnoyers
 */
#ifndef LINUX_IO_URING_H
#define LINUX_IO_URING_H

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POKE_MAX_LEN	16

struct pokevec {
	__u8 insn[POKE_MAX_LEN];
	__u64 ptr;	/* User pointer */
	__u8 len;
} __attribute__((packed));

#ifdef __cplusplus
}
#endif

#endif
