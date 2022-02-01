/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VCPU_TYPES_H
#define _LINUX_VCPU_TYPES_H

#include <linux/atomic.h>
#include <linux/cpumask.h>

struct vcpu_domain {
	/**
	 * @users: The number of references to &struct vcpu_domain from
	 * user-space threads.
	 *
	 * Initialized to 1 for the first thread with a reference with
	 * the domain. Incremented for each thread getting a reference to the
	 * domain, and decremented on domain release from user-space threads.
	 * Used to enable single-threaded domain vcpu accounting (when == 1).
	 */
	atomic_t users;
	/*
	 * Layout of vcpumasks:
	 * - vcpumask (cpumask_size()),
	 * - node_alloc_vcpumask (cpumask_size(), NUMA=y only),
	 * - array of nr_node_ids node_vcpumask (each cpumask_size(), NUMA=y only).
	 */
	cpumask_t vcpumasks[];
};

#endif /* _LINUX_VCPU_TYPES_H */
