/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* MMURTL memory calls (MEMCODE.ASM), same names, parameters and error codes as MMEMORY.H.

   AllocOSPage / AllocDMAPage: kernel memory in the OS region, visible to every job.
   AllocPage:                  memory in the current job's private (user) half.
   AliasMem / DeAliasMem:      map another job's pages into the current job (pages marked
                               PTE_ALIAS, so DeAllocPage / job teardown never free them).
   Memory is zeroed on allocation. */

#include <doors/mem.h>
#include <doors/x86.h>

static erc_t alloc_run(struct addr_space *as, U64 lo, U64 hi, U32 n, U64 flags, U8 **ret)
{
	U64 va, i;

	if (!n || !ret)
		return ErcBadMemReq;
	if (n > pmm_free_pages())
		return ErcNoMem;
	va = vmm_find_free(as, lo, hi, n);
	if (!va)
		return ErcNoMem;
	for (i = 0; i < n; i++) {
		U64 pa = pmm_alloc();

		if (!pa || vmm_map(as, va + i * PAGE_SIZE, pa, flags)) {
			if (pa)
				pmm_free(pa);
			DeAllocPage((U8 *)(uintptr_t)va, (U32)i);
			return ErcNoMem;
		}
	}
	*ret = (U8 *)(uintptr_t)va;
	return ErcOk;
}

static erc_t AllocOSPage_nolock(U32 nPages, U8 **ppMemRet)
{
	return alloc_run(&kernel_space, OS_BASE, OS_TOP, nPages, PTE_W | PTE_NX, ppMemRet);
}

static erc_t AllocPage_nolock(U32 nPages, U8 **ppMemRet)
{
	/* User pages stay executable until the ELF loader (M6) maps code and data separately. */
	return alloc_run(current_space, USER_BASE, USER_TOP, nPages, PTE_W | PTE_U, ppMemRet);
}

static erc_t AllocDMAPage_nolock(U32 nPages, U8 **ppMemRet, U64 *pPhyMemRet)
{
	U64 pa, va, i;

	if (!nPages || !ppMemRet || !pPhyMemRet)
		return ErcBadMemReq;
	pa = pmm_alloc_contig(nPages, DMA_LIMIT);	/* physically contiguous, below 16 MB */
	if (!pa)
		return ErcNoMem;
	va = vmm_find_free(&kernel_space, OS_BASE, OS_TOP, nPages);
	if (!va)
		goto nomem;
	for (i = 0; i < nPages; i++)
		if (vmm_map(&kernel_space, va + i * PAGE_SIZE, pa + i * PAGE_SIZE, PTE_W | PTE_NX)) {
			DeAllocPage((U8 *)(uintptr_t)va, (U32)i);
			pa += i * PAGE_SIZE;
			nPages -= (U32)i;
			goto nomem;
		}
	*ppMemRet = (U8 *)(uintptr_t)va;
	*pPhyMemRet = pa;
	return ErcOk;
nomem:
	for (i = 0; i < nPages; i++)
		pmm_free(pa + i * PAGE_SIZE);
	return ErcNoMem;
}

static struct addr_space *space_of(U64 va)
{
	return va >= 0xFFFF800000000000ULL ? &kernel_space : current_space;
}

/* As in MMURTL: the low 12 bits of pOrigMem are ignored; deallocates as many valid pages as it
   can, freeing physical pages unless they are aliases. */
static erc_t DeAllocPage_nolock(U8 *pOrigMem, U32 nPages)
{
	U64 va = (U64)(uintptr_t)pOrigMem & PAGE_MASK;
	struct addr_space *as = space_of(va);
	U32 i;

	if (va >= HHDM_BASE && va < OS_BASE)
		return ErcBadLinAdd;		/* never the direct map */
	if (va >= KERNEL_VMA)
		return ErcBadLinAdd;		/* never the kernel image */
	for (i = 0; i < nPages; i++, va += PAGE_SIZE) {
		U64 pte = vmm_unmap(as, va);

		if (!pte)
			return i ? ErcShortMem : ErcBadLinAdd;
		if (!(pte & PTE_ALIAS))
			pmm_free(pte & PTE_ADDR);
	}
	return ErcOk;
}

static erc_t QueryPages_nolock(U32 *pdPagesLeft)
{
	U64 n = pmm_free_pages();

	*pdPagesLeft = n > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (U32)n;
	return ErcOk;
}

static erc_t GetPhyAdd_nolock(U32 JobNum, U8 *LinAdd, U64 *pPhyRet)
{
	struct addr_space *as = job_space(JobNum);
	U64 va = (U64)(uintptr_t)LinAdd;

	if (!as)
		return ErcBadJobNum;
	if (va >= 0xFFFF800000000000ULL)
		as = &kernel_space;
	if (!vmm_translate(as, va, pPhyRet, NULL))
		return ErcBadLinAdd;
	return ErcOk;
}

static erc_t AliasMem_nolock(U8 *pMem, U64 dcbMem, U32 dJobNum, U8 **ppAliasRet)
{
	struct addr_space *src = job_space(dJobNum);
	U64 va = (U64)(uintptr_t)pMem, off = va & ~PAGE_MASK, n, dst, i;

	if (!src)
		return ErcBadJobNum;
	if (!dcbMem)
		return ErcBadMemReq;
	if (src == current_space || va >= 0xFFFF800000000000ULL) {
		*ppAliasRet = pMem;		/* same job, or kernel memory every job sees */
		return ErcOk;
	}
	n = PAGE_ROUND_UP(off + dcbMem) / PAGE_SIZE;
	dst = vmm_find_free(current_space, USER_BASE, USER_TOP, n);
	if (!dst)
		return ErcNoMem;
	for (i = 0; i < n; i++) {
		U64 pa, pte;

		if (!vmm_translate(src, (va & PAGE_MASK) + i * PAGE_SIZE, &pa, &pte)) {
			DeAliasMem((U8 *)(uintptr_t)dst, i * PAGE_SIZE, dJobNum);
			return ErcBadLinAdd;
		}
		if (vmm_map(current_space, dst + i * PAGE_SIZE, pa & PTE_ADDR,
			    (pte & (PTE_W | PTE_U | PTE_NX)) | PTE_ALIAS)) {
			DeAliasMem((U8 *)(uintptr_t)dst, i * PAGE_SIZE, dJobNum);
			return ErcNoMem;
		}
	}
	*ppAliasRet = (U8 *)(uintptr_t)(dst + off);
	return ErcOk;
}

static erc_t DeAliasMem_nolock(U8 *pAliasMem, U64 dcbAliasBytes, U32 JobNum)
{
	U64 va = (U64)(uintptr_t)pAliasMem, n, i;
	struct addr_space *src = job_space(JobNum);

	if (!src)
		return ErcBadJobNum;
	if (src == current_space || va >= 0xFFFF800000000000ULL)
		return ErcOk;			/* AliasMem returned the original address */
	n = PAGE_ROUND_UP((va & ~PAGE_MASK) + dcbAliasBytes) / PAGE_SIZE;
	va &= PAGE_MASK;
	for (i = 0; i < n; i++) {
		U64 pte;

		if (!vmm_translate(current_space, va + i * PAGE_SIZE, NULL, &pte))
			return ErcBadLinAdd;
		if (!(pte & PTE_ALIAS))
			return ErcBadAlias;
		vmm_unmap(current_space, va + i * PAGE_SIZE);
	}
	return ErcOk;
}

/* ---- public entry points: interrupts off while the page tables and bitmap change, so a
   task switch can't interleave two memory calls (MMURTL serialised them through an exchange).
   Internal calls between them (e.g. a failed allocation undoing itself) nest safely. ---- */

erc_t AllocOSPage(U32 nPages, U8 **ppMemRet)
{
	U64 f = irq_save();
	erc_t e = AllocOSPage_nolock(nPages, ppMemRet);

	irq_restore(f);
	return e;
}

erc_t AllocPage(U32 nPages, U8 **ppMemRet)
{
	U64 f = irq_save();
	erc_t e = AllocPage_nolock(nPages, ppMemRet);

	irq_restore(f);
	return e;
}

erc_t AllocDMAPage(U32 nPages, U8 **ppMemRet, U64 *pPhyMemRet)
{
	U64 f = irq_save();
	erc_t e = AllocDMAPage_nolock(nPages, ppMemRet, pPhyMemRet);

	irq_restore(f);
	return e;
}

erc_t DeAllocPage(U8 *pOrigMem, U32 nPages)
{
	U64 f = irq_save();
	erc_t e = DeAllocPage_nolock(pOrigMem, nPages);

	irq_restore(f);
	return e;
}

erc_t QueryPages(U32 *pdPagesLeft)
{
	U64 f = irq_save();
	erc_t e = QueryPages_nolock(pdPagesLeft);

	irq_restore(f);
	return e;
}

erc_t GetPhyAdd(U32 JobNum, U8 *LinAdd, U64 *pPhyRet)
{
	U64 f = irq_save();
	erc_t e = GetPhyAdd_nolock(JobNum, LinAdd, pPhyRet);

	irq_restore(f);
	return e;
}

erc_t AliasMem(U8 *pMem, U64 dcbMem, U32 dJobNum, U8 **ppAliasRet)
{
	U64 f = irq_save();
	erc_t e = AliasMem_nolock(pMem, dcbMem, dJobNum, ppAliasRet);

	irq_restore(f);
	return e;
}

erc_t DeAliasMem(U8 *pAliasMem, U64 dcbAliasBytes, U32 JobNum)
{
	U64 f = irq_save();
	erc_t e = DeAliasMem_nolock(pAliasMem, dcbAliasBytes, JobNum);

	irq_restore(f);
	return e;
}
