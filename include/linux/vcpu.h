/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VCPU_H
#define _LINUX_VCPU_H

#include <linux/cpumask.h>
#include <linux/nodemask.h>
#include <linux/vcpu_types.h>

#ifdef CONFIG_VCPU_DOMAIN
static inline unsigned int vcpu_domain_vcpumask_size(void)
{
	return cpumask_size();
}
static inline cpumask_t *vcpu_domain_vcpumask(struct vcpu_domain *vcpu_domain)
{
	return vcpu_domain->vcpumasks;
}

# ifdef CONFIG_NUMA
static inline unsigned int vcpu_domain_node_vcpumask_size(void)
{
	if (num_possible_nodes() == 1)
		return 0;
	return (nr_node_ids + 1) * cpumask_size();
}
static inline cpumask_t *vcpu_domain_node_alloc_vcpumask(struct vcpu_domain *vcpu_domain)
{
	unsigned long vcpu_bitmap = (unsigned long)vcpu_domain_vcpumask(vcpu_domain);

	/* Skip vcpumask */
	vcpu_bitmap += cpumask_size();
	return (struct cpumask *)vcpu_bitmap;
}
static inline cpumask_t *vcpu_domain_node_vcpumask(struct vcpu_domain *vcpu_domain, unsigned int node)
{
	unsigned long vcpu_bitmap = (unsigned long)vcpu_domain_node_alloc_vcpumask(vcpu_domain);

	/* Skip node alloc vcpumask */
	vcpu_bitmap += cpumask_size();
	vcpu_bitmap += node * cpumask_size();
	return (struct cpumask *)vcpu_bitmap;
}
static inline void vcpu_domain_node_init(struct vcpu_domain *vcpu_domain)
{
	unsigned int node;

	if (num_possible_nodes() == 1)
		return;
	cpumask_clear(vcpu_domain_node_alloc_vcpumask(vcpu_domain));
	for (node = 0; node < nr_node_ids; node++)
		cpumask_clear(vcpu_domain_node_vcpumask(vcpu_domain, node));
}
# else /* CONFIG_NUMA */
static inline unsigned int vcpu_domain_node_vcpumask_size(void)
{
	return 0;
}
static inline void vcpu_domain_node_init(struct vcpu_domain *vcpu_domain) { }
# endif /* CONFIG_NUMA */

static inline unsigned int vcpu_domain_size(void)
{
	return offsetof(struct vcpu_domain, vcpumasks) + vcpu_domain_vcpumask_size() +
	       vcpu_domain_node_vcpumask_size();
}
static inline void vcpu_domain_init(struct vcpu_domain *vcpu_domain)
{
	atomic_set(&vcpu_domain->users, 1);
	cpumask_clear(vcpu_domain_vcpumask(vcpu_domain));
	vcpu_domain_node_init(vcpu_domain);
}
#else /* CONFIG_VCPU_DOMAIN */
static inline unsigned int vcpu_domain_size(void)
{
	return 0;
}
static inline void vcpu_domain_init(struct vcpu_domain *vcpu_domain) { }
#endif /* CONFIG_VCPU_DOMAIN */

#endif /* _LINUX_VCPU_H */
