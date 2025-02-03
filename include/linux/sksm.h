/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SKSM_H
#define __LINUX_SKSM_H
/*
 * Synchronous memory merging support.
 *
 * This code enables synchronous dynamic sharing of identical pages
 * found in different memory areas, even if they are not shared by
 * fork().
 */

#ifdef CONFIG_SKSM

int sksm_merge(struct vm_area_struct *vma, unsigned long start,
	       unsigned long end);

#else  /* !CONFIG_KSM */

static inline int sksm_merge(struct vm_area_struct *vma, unsigned long start,
			     unsigned long end)
{
	return 0;
}

#endif	/* !CONFIG_KSM */

#endif /* __LINUX_SKSM_H */
