/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Concurrency IDs read through the rseq selftest library, see
 * cpu_concurrency_rseq.c.
 */
#ifndef CPU_CONCURRENCY_RSEQ_H
#define CPU_CONCURRENCY_RSEQ_H

/* The concurrency IDs of the rseq area, the "kind" of the functions below. */
#define LIB_RSEQ_MM_CID	0
#define LIB_RSEQ_NS_CID	1

int lib_rseq_register(void);
void lib_rseq_unregister(void);
int lib_rseq_has_id(int kind);
int lib_rseq_glibc_hides_ns_cid(void);
unsigned int lib_rseq_id(int kind);
int lib_rseq_count_by_id(int kind, unsigned long *table, unsigned int n, unsigned int *idp);

#endif
