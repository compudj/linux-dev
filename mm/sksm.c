// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synchronous memory merging support.
 *
 * This code enables synchronous dynamic sharing of identical pages
 * found in different memory areas, even if they are not shared by
 * fork().
 *
 * Userspace must explicitly request for pages within specific address
 * ranges to be merged with madvise MADV_MERGE. Those should *not*
 * contain secrets, as side-channel timing attacks can allow a process
 * to learn the existence of a known content within another process.
 *
 * The synchronous memory merging performs the memory merging
 * synchronously within madvise. There is no global scan and no need
 * for background daemon.
 *
 * The anonymous pages targeted for merge are write-protected and
 * checksummed. They are then compared to other pages targeted for
 * merge.
 *
 * The mergeable pages are added to a hash table indexed by checksum of
 * their content. The hash value is derived from the page content
 * checksum, and its comparison function is based on comparison of
 * the page content.
 *
 * If a page is written to after being targeted for merge, a COW will be
 * triggered, and thus a new page will be populated in its stead.
 *
 * The typical usage pattern expected from userspace is:
 *
 * 1) Userspace writes non-secret content to a MAP_PRIVATE page, thus
 *    triggering COW.
 *
 * 2) After userspace has completed writing to the page, it issues
 *    madvise MADV_MERGE on a range containing the page, which
 *    write-protect, checksum, and add the page to the sksm hash
 *    table. It then merges this page with other mergeable pages
 *    that have the same content.
 *
 * 3) It is typically expected that this page's content stays invariant
 *    for a long time. If userspace issues writes to the page after
 *    madvise MADV_MERGE, another COW will be triggered, which will
 *    populate a new page copy into the process page table and release
 *    the reference to the old page.
 */

#include <linux/mutex.h>
#include <linux/cleanup.h>
#include <linux/mm_types.h>
#include <linux/hashtable.h>
#include <linux/highmem.h>
#include <linux/xxhash.h>
#include <linux/rmap.h>
#include <linux/mm.h>
#include <linux/pagewalk.h>
#include <linux/sksm.h>
#include <linux/swap.h>
#include <linux/mm_inline.h>

#include "internal.h"
#include "ksm-common.h"

#define SKSM_HT_BITS	16

static DEFINE_MUTEX(sksm_lock);

/*
 * The hash is derived from the page checksum.
 */
static DEFINE_HASHTABLE(sksm_ht, SKSM_HT_BITS);

void __sksm_page_remove(struct page *page)
{
	guard(mutex)(&sksm_lock);
	hash_del(&page->sksm_node);
}

static int sksm_merge_page(struct vm_area_struct *vma, struct page *page)
{
	struct folio *folio = page_folio(page);
	pte_t orig_pte = __pte(0);
	struct page *kpage;
	int err = 0;

	folio_lock(folio);

	if (folio_test_large(folio)) {
		if (split_huge_page(page))
			goto out_unlock;
		folio = page_folio(page);
	}

	/* Write protect page. */
	if (write_protect_page(vma, folio, &orig_pte) != 0)
		goto out_unlock;

	/* Checksum page. */
	page->checksum = calc_checksum(page);

	guard(mutex)(&sksm_lock);

	/* Merge page with duplicates. */
	hash_for_each_possible(sksm_ht, kpage, sksm_node, page->checksum) {
		if (page->checksum != kpage->checksum || !pages_identical(page, kpage))
			continue;
		if (!get_page_unless_zero(kpage))
			continue;
		err = replace_page(vma, page, kpage, orig_pte);
		put_page(kpage);
		if (!err)
			goto out_unlock;
	}

	/*
	 * This page is not linked to its address_space anymore because it
	 * can be shared with other processes and replace pages originally
	 * associated with other address spaces.
	 */
	page->mapping = (void *) PAGE_MAPPING_ANON;

	/* Add page to hash table. */
	hash_add(sksm_ht, &page->sksm_node, page->checksum);
out_unlock:
	folio_unlock(folio);
	return err;
}

static struct page *get_vma_page_from_addr(struct vm_area_struct *vma, unsigned long addr)
{
	struct page *page = NULL;
	struct folio_walk fw;
	struct folio *folio;

	folio = folio_walk_start(&fw, vma, addr, 0);
	if (folio) {
		if (!folio_is_zone_device(folio) &&
		    folio_test_anon(folio)) {
			folio_get(folio);
			page = fw.page;
		}
		folio_walk_end(&fw, vma);
	}
	if (page) {
		flush_anon_page(vma, page, addr);
		flush_dcache_page(page);
	}
	return page;
}

/* Called with mmap write lock held. */
int sksm_merge(struct vm_area_struct *vma, unsigned long start,
	       unsigned long end)
{
	unsigned long addr;
	int err = 0;

	if (!PAGE_ALIGNED(start) || !PAGE_ALIGNED(end))
		return -EINVAL;
	if (!vma_ksm_compatible(vma))
		return 0;

	/*
	 * A number of pages can hang around indefinitely in per-cpu
	 * LRU cache, raised page count preventing write_protect_page
	 * from merging them.
	 */
	lru_add_drain_all();

	for (addr = start; addr < end && !err; addr += PAGE_SIZE) {
		struct page *page = get_vma_page_from_addr(vma, addr);

		if (!page)
			continue;
		err = sksm_merge_page(vma, page);
		put_page(page);
	}
	return err;
}

static int __init sksm_init(void)
{
	struct page *zero_page = ZERO_PAGE(0);

	zero_page->checksum = calc_checksum(zero_page);
	/* Add page to hash table. */
	hash_add(sksm_ht, &zero_page->sksm_node, zero_page->checksum);
	return 0;
}
subsys_initcall(sksm_init);
