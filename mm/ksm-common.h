// SPDX-License-Identifier: GPL-2.0-only
/*
 * Memory merging support, common code.
 */
#ifndef _KSM_COMMON_H
#define _KSM_COMMON_H

#include <linux/ksm.h>

static bool vma_ksm_compatible(struct vm_area_struct *vma)
{
	if (vma->vm_flags & (VM_SHARED  | VM_MAYSHARE   | VM_PFNMAP  |
			     VM_IO      | VM_DONTEXPAND | VM_HUGETLB |
			     VM_MIXEDMAP| VM_DROPPABLE))
		return false;		/* just ignore the advice */

	if (vma_is_dax(vma))
		return false;

#ifdef VM_SAO
	if (vma->vm_flags & VM_SAO)
		return false;
#endif
#ifdef VM_SPARC_ADI
	if (vma->vm_flags & VM_SPARC_ADI)
		return false;
#endif

	return true;
}

static u32 calc_checksum(struct page *page)
{
	u32 checksum;
	void *addr = kmap_local_page(page);
	checksum = xxhash(addr, PAGE_SIZE, 0);
	kunmap_local(addr);
	return checksum;
}

static int write_protect_page(struct vm_area_struct *vma, struct folio *folio,
			      pte_t *orig_pte)
{
	struct mm_struct *mm = vma->vm_mm;
	DEFINE_FOLIO_VMA_WALK(pvmw, folio, vma, 0, 0);
	int swapped;
	int err = -EFAULT;
	struct mmu_notifier_range range;
	bool anon_exclusive;
	pte_t entry;

	if (WARN_ON_ONCE(folio_test_large(folio)))
		return err;

	pvmw.address = page_address_in_vma(folio, folio_page(folio, 0), vma);
	if (pvmw.address == -EFAULT)
		goto out;

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm, pvmw.address,
				pvmw.address + PAGE_SIZE);
	mmu_notifier_invalidate_range_start(&range);

	if (!page_vma_mapped_walk(&pvmw))
		goto out_mn;
	if (WARN_ONCE(!pvmw.pte, "Unexpected PMD mapping?"))
		goto out_unlock;

	anon_exclusive = PageAnonExclusive(&folio->page);
	entry = ptep_get(pvmw.pte);
	if (pte_write(entry) || pte_dirty(entry) ||
	    anon_exclusive || mm_tlb_flush_pending(mm)) {
		swapped = folio_test_swapcache(folio);
		flush_cache_page(vma, pvmw.address, folio_pfn(folio));
		/*
		 * Ok this is tricky, when get_user_pages_fast() run it doesn't
		 * take any lock, therefore the check that we are going to make
		 * with the pagecount against the mapcount is racy and
		 * O_DIRECT can happen right after the check.
		 * So we clear the pte and flush the tlb before the check
		 * this assure us that no O_DIRECT can happen after the check
		 * or in the middle of the check.
		 *
		 * No need to notify as we are downgrading page table to read
		 * only not changing it to point to a new page.
		 *
		 * See Documentation/mm/mmu_notifier.rst
		 */
		entry = ptep_clear_flush(vma, pvmw.address, pvmw.pte);
		/*
		 * Check that no O_DIRECT or similar I/O is in progress on the
		 * page
		 */
		if (folio_mapcount(folio) + 1 + swapped != folio_ref_count(folio)) {
			set_pte_at(mm, pvmw.address, pvmw.pte, entry);
			goto out_unlock;
		}

		/* See folio_try_share_anon_rmap_pte(): clear PTE first. */
		if (anon_exclusive &&
		    folio_try_share_anon_rmap_pte(folio, &folio->page)) {
			set_pte_at(mm, pvmw.address, pvmw.pte, entry);
			goto out_unlock;
		}

		if (pte_dirty(entry))
			folio_mark_dirty(folio);
		entry = pte_mkclean(entry);

		if (pte_write(entry))
			entry = pte_wrprotect(entry);

		set_pte_at(mm, pvmw.address, pvmw.pte, entry);
	}
	*orig_pte = entry;
	err = 0;

out_unlock:
	page_vma_mapped_walk_done(&pvmw);
out_mn:
	mmu_notifier_invalidate_range_end(&range);
out:
	return err;
}

/**
 * replace_page - replace page in vma by new ksm page
 * @vma:      vma that holds the pte pointing to page
 * @page:     the page we are replacing by kpage
 * @kpage:    the ksm page we replace page by
 * @orig_pte: the original value of the pte
 *
 * Returns 0 on success, -EFAULT on failure.
 */
static int replace_page(struct vm_area_struct *vma, struct page *page,
			struct page *kpage, pte_t orig_pte)
{
	struct folio *kfolio = page_folio(kpage);
	struct mm_struct *mm = vma->vm_mm;
	struct folio *folio = page_folio(page);
	pmd_t *pmd;
	pmd_t pmde;
	pte_t *ptep;
	pte_t newpte;
	spinlock_t *ptl;
	unsigned long addr;
	int err = -EFAULT;
	struct mmu_notifier_range range;

	addr = page_address_in_vma(folio, page, vma);
	if (addr == -EFAULT)
		goto out;

	pmd = mm_find_pmd(mm, addr);
	if (!pmd)
		goto out;
	/*
	 * Some THP functions use the sequence pmdp_huge_clear_flush(), set_pmd_at()
	 * without holding anon_vma lock for write.  So when looking for a
	 * genuine pmde (in which to find pte), test present and !THP together.
	 */
	pmde = pmdp_get_lockless(pmd);
	if (!pmd_present(pmde) || pmd_trans_huge(pmde))
		goto out;

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm, addr,
				addr + PAGE_SIZE);
	mmu_notifier_invalidate_range_start(&range);

	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!ptep)
		goto out_mn;
	if (!pte_same(ptep_get(ptep), orig_pte)) {
		pte_unmap_unlock(ptep, ptl);
		goto out_mn;
	}
	VM_BUG_ON_PAGE(PageAnonExclusive(page), page);
	VM_BUG_ON_FOLIO(folio_test_anon(kfolio) && PageAnonExclusive(kpage),
			kfolio);

	/*
	 * No need to check ksm_use_zero_pages here: we can only have a
	 * zero_page here if ksm_use_zero_pages was enabled already.
	 */
	if (!is_zero_pfn(page_to_pfn(kpage))) {
		folio_get(kfolio);
		folio_add_anon_rmap_pte(kfolio, kpage, vma, addr, RMAP_NONE);
		newpte = mk_pte(kpage, vma->vm_page_prot);
	} else {
		/*
		 * Use pte_mkdirty to mark the zero page mapped by KSM, and then
		 * we can easily track all KSM-placed zero pages by checking if
		 * the dirty bit in zero page's PTE is set.
		 */
		newpte = pte_mkdirty(pte_mkspecial(pfn_pte(page_to_pfn(kpage), vma->vm_page_prot)));
		ksm_map_zero_page(mm);
		/*
		 * We're replacing an anonymous page with a zero page, which is
		 * not anonymous. We need to do proper accounting otherwise we
		 * will get wrong values in /proc, and a BUG message in dmesg
		 * when tearing down the mm.
		 */
		dec_mm_counter(mm, MM_ANONPAGES);
	}

	flush_cache_page(vma, addr, pte_pfn(ptep_get(ptep)));
	/*
	 * No need to notify as we are replacing a read only page with another
	 * read only page with the same content.
	 *
	 * See Documentation/mm/mmu_notifier.rst
	 */
	ptep_clear_flush(vma, addr, ptep);
	set_pte_at(mm, addr, ptep, newpte);

	folio_remove_rmap_pte(folio, page, vma);
	if (!folio_mapped(folio))
		folio_free_swap(folio);
	folio_put(folio);

	pte_unmap_unlock(ptep, ptl);
	err = 0;
out_mn:
	mmu_notifier_invalidate_range_end(&range);
out:
	return err;
}

#endif /* _KSM_COMMON_H */
