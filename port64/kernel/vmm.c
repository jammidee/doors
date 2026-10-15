/* 4-level paging and address spaces.

   vmm_init() replaces the boot page tables (boot.asm: first 1 GiB mapped twice with 2 MiB pages)
   with the kernel's own:
     HHDM_BASE + phys   all RAM, 2 MiB pages, RW + NX               (PML4 256..)
     OS_BASE ...        AllocOSPage / AllocDMAPage, 4 KB pages      (PML4 384)
     KERNEL_VMA + phys  kernel image, 4 KB pages: .text RX, .rodata R+NX, .data/.bss RW+NX,
                        and an unmapped guard page under the kernel stack (PML4 511)
   The low identity map is gone afterwards, so a stray NULL-ish pointer now faults.

   Every address space shares the kernel half: its PML4 entries 256..511 are copied from the
   kernel's, whose PDPTs are all created here and never freed, so later kernel mappings are seen
   by every space. That replaces MMURTL's trick of putting the OS in every job's page directory.
   The user half (0 .. USER_TOP) is private per job. */

#include <doors/console.h>
#include <doors/mem.h>
#include <doors/string.h>
#include <doors/x86.h>

struct addr_space kernel_space = { 0, 1 };
struct addr_space *current_space = &kernel_space;

static U64 hhdm = KERNEL_VMA;	/* boot map until vmm_init() switches to HHDM_BASE */

extern U8 _text_start[], _text_end[], _rodata_start[], _rodata_end[];
extern U8 _kernel_start[], _kernel_end[];
extern U8 kernel_stack_guard[];			/* boot.asm */

void *phys_to_virt(U64 phys)
{
	return (void *)(uintptr_t)(phys + hhdm);
}

static inline U64 *table(U64 entry)
{
	return phys_to_virt(entry & PTE_ADDR);
}

static inline void invlpg(U64 va)
{
	__asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/* Next-level table for entry idx of tbl; allocates it if create is set. */
static U64 *next(U64 *tbl, unsigned idx, bool create, bool user)
{
	U64 pa;

	if (tbl[idx] & PTE_P)
		return table(tbl[idx]);
	if (!create)
		return NULL;
	pa = pmm_alloc();
	if (!pa)
		return NULL;
	if (hhdm == KERNEL_VMA && pa >= 0x40000000ULL)
		panic("vmm: page table at %lx is beyond the boot map", pa);
	tbl[idx] = pa | PTE_P | PTE_W | (user ? PTE_U : 0);
	return table(tbl[idx]);
}

#define IDX(va, lvl) ((unsigned)(((va) >> (12 + 9 * (lvl))) & 511))

static U64 *walk(struct addr_space *as, U64 va, bool create)
{
	bool user = va < 0x0000800000000000ULL;
	U64 *t = table(as->pml4);

	if (!(t = next(t, IDX(va, 3), create, user)))
		return NULL;
	if (!(t = next(t, IDX(va, 2), create, user)))
		return NULL;
	if (t[IDX(va, 1)] & PTE_PS)
		return NULL;		/* 2 MiB page: not handled by 4 KB operations */
	if (!(t = next(t, IDX(va, 1), create, user)))
		return NULL;
	return &t[IDX(va, 0)];
}

erc_t vmm_map(struct addr_space *as, U64 va, U64 pa, U64 flags)
{
	U64 *pte = walk(as, va, true);

	if (!pte)
		return ErcNoMem;
	*pte = (pa & PTE_ADDR) | flags | PTE_P;
	if (as == current_space || va >= 0xFFFF800000000000ULL)
		invlpg(va);
	return ErcOk;
}

U64 vmm_unmap(struct addr_space *as, U64 va)
{
	U64 *pte = walk(as, va, false), old;

	if (!pte || !(*pte & PTE_P))
		return 0;
	old = *pte;
	*pte = 0;
	if (as == current_space || va >= 0xFFFF800000000000ULL)
		invlpg(va);
	return old;
}

bool vmm_translate(struct addr_space *as, U64 va, U64 *pa, U64 *pte_out)
{
	U64 *t = table(as->pml4), e;
	int lvl;

	for (lvl = 3; lvl >= 0; lvl--) {
		e = t[IDX(va, lvl)];
		if (!(e & PTE_P))
			return false;
		if (lvl == 1 && (e & PTE_PS)) {		/* 2 MiB page */
			if (pa)
				*pa = (e & PTE_ADDR & ~0x1FFFFFULL) + (va & 0x1FFFFF);
			if (pte_out)
				*pte_out = e;
			return true;
		}
		if (lvl)
			t = table(e);
	}
	if (pa)
		*pa = (e & PTE_ADDR) + (va & 0xFFF);
	if (pte_out)
		*pte_out = e;
	return true;
}

/* First run of n unmapped 4 KB pages in [start, end); 0 if none.
   Linear scan, fine for now (MMURTL also searched its page tables for a run of free PTEs). */
U64 vmm_find_free(struct addr_space *as, U64 start, U64 end, U64 n)
{
	U64 va, run = 0, first = 0;

	for (va = start; va < end; va += PAGE_SIZE) {
		if (vmm_translate(as, va, NULL, NULL)) {
			run = 0;
			continue;
		}
		if (!run++)
			first = va;
		if (run == n)
			return first;
	}
	return 0;
}

static void map_kernel_range(U64 *pml4_unused, U8 *from, U8 *to, U64 flags)
{
	U64 va;

	(void)pml4_unused;
	for (va = (U64)(uintptr_t)from & PAGE_MASK; va < (U64)(uintptr_t)to; va += PAGE_SIZE) {
		if (va == (U64)(uintptr_t)kernel_stack_guard)
			continue;	/* guard page: left unmapped so a stack overflow faults */
		if (vmm_map(&kernel_space, va, va - KERNEL_VMA, flags))
			panic("vmm: out of memory mapping the kernel");
	}
}

void vmm_init(void)
{
	U64 *pml4, pa, max = pmm_max_phys();
	unsigned i;

	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);	/* honour PTE_NX */

	kernel_space.pml4 = pmm_alloc();
	pml4 = table(kernel_space.pml4);

	/* Create every kernel-half PDPT now so all address spaces can share PML4[256..511]. */
	for (i = 256; i < 512; i++)
		if (!next(pml4, i, true, false))
			panic("vmm: out of memory for kernel PDPTs");

	/* Direct map of all RAM with 2 MiB pages. */
	for (pa = 0; pa < max; pa += 0x200000) {
		U64 va = HHDM_BASE + pa;
		U64 *pd = next(next(pml4, IDX(va, 3), true, false), IDX(va, 2), true, false);

		if (!pd)
			panic("vmm: out of memory for the direct map");
		pd[IDX(va, 1)] = pa | PTE_P | PTE_W | PTE_PS | PTE_NX;
	}

	/* Kernel image with section permissions. */
	map_kernel_range(pml4, _text_start, _text_end, 0);			/* RX */
	map_kernel_range(pml4, _rodata_start, _rodata_end, PTE_NX);		/* R */
	map_kernel_range(pml4, _rodata_end, _kernel_end, PTE_W | PTE_NX);	/* RW */

	__asm__ volatile("mov %0, %%cr3" : : "r"(kernel_space.pml4) : "memory");
	/* CR0.WP: without it ring 0 ignores read-only pages, and .text/.rodata would stay writable. */
	__asm__ volatile("mov %0, %%cr0" : : "r"(read_cr0() | (1UL << 16)) : "memory");
	hhdm = HHDM_BASE;
	pmm_remap();
	vga_remap();

	kprintf("Paging:  own 4-level tables, PML4 at %lx; RAM direct-mapped at %lx; NX on; "
		"stack guard at %p\n", kernel_space.pml4, HHDM_BASE, kernel_stack_guard);
}

/* ---- address spaces ---- */

#define MAX_JOBS 64
static struct addr_space spaces[MAX_JOBS];

struct addr_space *job_space(U32 job)
{
	if (job == 1)
		return &kernel_space;
	if (job >= MAX_JOBS || !spaces[job].pml4)
		return NULL;
	return &spaces[job];
}

struct addr_space *vmm_space_create(U32 job)
{
	struct addr_space *as;
	U64 *pml4;

	if (job < 2 || job >= MAX_JOBS || spaces[job].pml4)
		return NULL;
	as = &spaces[job];
	as->pml4 = pmm_alloc();
	if (!as->pml4)
		return NULL;
	as->job = job;
	pml4 = table(as->pml4);
	memcpy(&pml4[256], &table(kernel_space.pml4)[256], 256 * sizeof(U64));
	return as;
}

/* Free the user half: pages (except aliases, which belong to another job) and page tables. */
static void free_level(U64 *t, int lvl)
{
	unsigned i;

	for (i = 0; i < 512; i++) {
		U64 e = t[i];

		if (!(e & PTE_P))
			continue;
		if (lvl == 0) {
			if (!(e & PTE_ALIAS))
				pmm_free(e & PTE_ADDR);
		} else {
			free_level(table(e), lvl - 1);
			pmm_free(e & PTE_ADDR);
		}
		t[i] = 0;
	}
}

void vmm_space_destroy(struct addr_space *as)
{
	U64 *pml4 = table(as->pml4);
	unsigned i;

	if (as == &kernel_space)
		panic("vmm: cannot destroy the kernel address space");
	if (as == current_space)
		vmm_space_switch(&kernel_space);
	for (i = 0; i < 256; i++) {
		if (pml4[i] & PTE_P) {
			free_level(table(pml4[i]), 2);
			pmm_free(pml4[i] & PTE_ADDR);
			pml4[i] = 0;
		}
	}
	pmm_free(as->pml4);
	as->pml4 = 0;
}

void vmm_space_switch(struct addr_space *as)
{
	current_space = as;
	__asm__ volatile("mov %0, %%cr3" : : "r"(as->pml4) : "memory");
}
