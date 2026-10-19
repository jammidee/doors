/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS memory management: physical pages, 4-level paging, address spaces, and the MMURTL
   memory calls (OSSOURCE/MEMCODE.ASM, MMEMORY.H) with the same names and parameter order.
   Only types widen: pointers, byte counts and physical addresses are 64-bit. */
#ifndef DOORS_MEM_H
#define DOORS_MEM_H

#include <doors/types.h>

#define PAGE_SIZE   4096UL
#define PAGE_MASK   (~(PAGE_SIZE - 1))
#define PAGE_ROUND_UP(x) (((x) + PAGE_SIZE - 1) & PAGE_MASK)

/* Virtual memory layout (canonical higher half = kernel, shared by every address space). */
#define USER_BASE   0x0000000040000000UL	/* 1 GiB: MMURTL user code also started at 0x40000000 */
#define USER_TOP    0x00007FFFFFFFF000UL
#define HHDM_BASE   0xFFFF800000000000UL	/* direct map of all physical RAM (PML4 256) */
#define OS_BASE     0xFFFFC00000000000UL	/* AllocOSPage / AllocDMAPage region (PML4 384) */
#define OS_TOP      0xFFFFC08000000000UL	/* 512 GiB */
#define STACK_BASE  0xFFFFC08000000000UL	/* task kernel stacks, one guarded slot per task (PML4 385) */
#define RQB_UBASE   0xFFFFC10000000000UL	/* read-only user window on the request block pool (PML4 386) */
#define USER_STACK_TOP 0x00007FFFFFFF0000UL	/* initial user stack of a job (grows down) */
#define USER_STACK_PAGES 16			/* 64 KB, with an unmapped guard page below */
/* KERNEL_VMA (types.h) 0xFFFFFFFF80000000: kernel image (PML4 511) */

#define DMA_LIMIT   0x1000000UL		/* ISA DMA: below 16 MB */

/* Page table entry bits. */
#define PTE_P       (1UL << 0)
#define PTE_W       (1UL << 1)
#define PTE_U       (1UL << 2)
#define PTE_PS      (1UL << 7)		/* 2 MiB page in a PD */
#define PTE_ALIAS   (1UL << 9)		/* AVL bit: alias of another job's page (as MMURTL's "A" bit) */
#define PTE_NX      (1UL << 63)
#define PTE_ADDR    0x000FFFFFFFFFF000UL

#include <doors/erc.h>

/* ---- physical memory (kernel/pmm.c) ---- */
struct boot_info;
void pmm_init(U64 mbi_phys, const struct boot_info *bi);
U64  pmm_alloc(void);				/* one zeroed page, 0 if none */
U64  pmm_alloc_contig(U64 n, U64 limit);	/* n contiguous zeroed pages below limit, 0 if none */
void pmm_free(U64 phys);			/* fills the page with PMM_POISON; a double free panics */
void pmm_test_corrupt(void);			/* crash=check only: break the free counter on purpose */
void pmm_remap(void);				/* called by vmm_init() after switching CR3 */
U64  pmm_free_pages(void);
U64  pmm_total_pages(void);
U64  pmm_max_phys(void);

/* ---- virtual memory (kernel/vmm.c) ---- */
struct addr_space {
	U64 pml4;		/* physical address */
	U32 job;		/* owning job number (1 = OS/Monitor) */
};

extern struct addr_space kernel_space;
extern struct addr_space *current_space;

void *phys_to_virt(U64 phys);
void  vmm_init(void);
erc_t vmm_map(struct addr_space *as, U64 va, U64 pa, U64 flags);
U64   vmm_unmap(struct addr_space *as, U64 va);		/* returns the old PTE (0 if none) */
bool  vmm_translate(struct addr_space *as, U64 va, U64 *pa, U64 *pte);
U64   vmm_find_free(struct addr_space *as, U64 start, U64 end, U64 n);	/* 0 if none */
struct addr_space *vmm_space_create(U32 job);
void  vmm_space_destroy(struct addr_space *as);
void  vmm_space_switch(struct addr_space *as);
struct addr_space *job_space(U32 job);			/* until the job manager exists (M6) */

/* ---- MMURTL memory calls (kernel/mem.c) ---- */
erc_t AllocPage(U32 nPages, U8 **ppMemRet);
erc_t AllocOSPage(U32 nPages, U8 **ppMemRet);
erc_t AllocDMAPage(U32 nPages, U8 **ppMemRet, U64 *pPhyMemRet);
erc_t DeAllocPage(U8 *pOrigMem, U32 nPages);
erc_t QueryPages(U32 *pdPagesLeft);
erc_t GetPhyAdd(U32 JobNum, U8 *LinAdd, U64 *pPhyRet);
erc_t AliasMem(U8 *pMem, U64 dcbMem, U32 dJobNum, U8 **ppAliasRet);
erc_t DeAliasMem(U8 *pAliasMem, U64 dcbAliasBytes, U32 JobNum);

/* ---- safe access probes (arch/x86_64/probe.asm): return false if the access faulted ---- */
bool probe_read(const void *addr, U64 *val);
bool probe_write(void *addr, U64 val);

#endif
