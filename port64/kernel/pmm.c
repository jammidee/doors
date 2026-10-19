/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Physical page allocator: one bit per 4 KB page (1 = in use), built from the Multiboot2 memory
   map. MMURTL kept the same kind of bitmap (its PAM, MEMCODE.ASM); here it covers all RAM.

   Zones: pages below 16 MB are kept for ISA DMA (AllocDMAPage). Ordinary allocations search
   from 16 MB upwards first and only fall back to low memory when nothing else is left.
   Not yet thread-safe: tasks arrive in M4 (MMURTL serialised memory calls through an exchange). */

#include <doors/console.h>
#include <doors/mem.h>
#include <doors/multiboot2.h>
#include <doors/string.h>
#include <doors/x86.h>

extern U8 _kernel_end[];	/* linker.ld (virtual) */

static U64 *bitmap;		/* bit set = page used */
static U64 bitmap_phys;
static U64 npages;		/* pages covered by the bitmap */
static U64 nfree;
static U64 ntotal;		/* usable pages reported by the firmware */
static U64 hint = DMA_LIMIT / PAGE_SIZE;

static inline bool used(U64 pg) { return bitmap[pg / 64] & (1ULL << (pg % 64)); }
static inline void set_used(U64 pg) { bitmap[pg / 64] |= 1ULL << (pg % 64); }
static inline void set_free(U64 pg) { bitmap[pg / 64] &= ~(1ULL << (pg % 64)); }

static void mark(U64 start, U64 end, bool use)
{
	U64 pg;

	for (pg = start / PAGE_SIZE; pg < end / PAGE_SIZE && pg < npages; pg++) {
		if (use && !used(pg)) {
			set_used(pg);
			nfree--;
		} else if (!use && used(pg)) {
			set_free(pg);
			nfree++;
		}
	}
}

static const struct mb2_tag_mmap *find_mmap(U64 mbi_phys)
{
	const struct mb2_info *mbi = PHYS_TO_VIRT(mbi_phys);
	const U8 *p = (const U8 *)mbi + 8, *end = (const U8 *)mbi + mbi->total_size;

	while (p < end) {
		const struct mb2_tag *t = (const struct mb2_tag *)p;

		if (t->type == MB2_TAG_END)
			break;
		if (t->type == MB2_TAG_MMAP)
			return (const struct mb2_tag_mmap *)t;
		p += (t->size + 7) & ~7u;
	}
	return NULL;
}

#define FOR_EACH_AVAIL(m, e) \
	for (const U8 *_p = (const U8 *)(m)->entries; \
	     _p < (const U8 *)(m) + (m)->size; _p += (m)->entry_size) \
		if ((e = (const struct mb2_mmap_entry *)_p)->type == MB2_MEM_AVAILABLE)

void pmm_init(U64 mbi_phys, const struct boot_info *bi)
{
	U32 mbi_size = bi->mbi_size, i;
	const struct mb2_tag_mmap *m = find_mmap(mbi_phys);
	const struct mb2_mmap_entry *e;
	U64 max = 0, bytes, kend = (U64)(uintptr_t)_kernel_end - KERNEL_VMA, at = 0;

	if (!m)
		panic("pmm: no memory map from the boot loader");

	FOR_EACH_AVAIL(m, e)
		if (e->base + e->length > max)
			max = e->base + e->length;
	npages = max / PAGE_SIZE;
	bytes = PAGE_ROUND_UP((npages + 63) / 64 * 8);

	/* Put the bitmap in the first available run above the kernel and the MBI, below 1 GiB
	   (the boot page tables only map the first 1 GiB). */
	FOR_EACH_AVAIL(m, e) {
		U64 s = PAGE_ROUND_UP(e->base), end = (e->base + e->length) & PAGE_MASK;

		if (s < kend)
			s = PAGE_ROUND_UP(kend);
		if (s < mbi_phys + mbi_size && s + bytes > mbi_phys)
			s = PAGE_ROUND_UP(mbi_phys + mbi_size);
		for (i = 0; i < bi->nmods; i++)		/* and clear of the boot modules */
			if (s < bi->mods[i].end && s + bytes > bi->mods[i].start)
				s = PAGE_ROUND_UP(bi->mods[i].end);
		if (s + bytes <= end && s + bytes <= 0x40000000ULL) {
			at = s;
			break;
		}
	}
	if (!at)
		panic("pmm: no room for the page bitmap (%lu bytes)", bytes);
	bitmap_phys = at;
	bitmap = PHYS_TO_VIRT(at);
	memset(bitmap, 0xFF, bytes);		/* everything used ... */
	nfree = 0;

	FOR_EACH_AVAIL(m, e) {			/* ... except available RAM */
		U64 s = PAGE_ROUND_UP(e->base), end = (e->base + e->length) & PAGE_MASK;

		if (end <= s)
			continue;		/* region smaller than a page */
		mark(s, end, false);
		ntotal += (end - s) / PAGE_SIZE;
	}

	mark(0, 0x100000, true);			/* real-mode IVT, BDA, EBDA, VGA, BIOS */
	mark(0x100000, PAGE_ROUND_UP(kend), true);	/* kernel image (boot + higher half) */
	mark(at, at + bytes, true);			/* the bitmap itself */
	mark(mbi_phys & PAGE_MASK, PAGE_ROUND_UP(mbi_phys + mbi_size), true);
	for (i = 0; i < bi->nmods; i++)		/* program images stay where GRUB put them */
		mark(bi->mods[i].start & PAGE_MASK, PAGE_ROUND_UP(bi->mods[i].end), true);

	kprintf("Memory:  %lu KB free of %lu KB (page bitmap %lu KB at %lx)\n",
		nfree * 4, ntotal * 4, bytes >> 10, at);
}

static U64 search(U64 from, U64 to, U64 n)
{
	U64 pg, run = 0;

	for (pg = from; pg < to; pg++) {
		run = used(pg) ? 0 : run + 1;
		if (run == n)
			return pg - n + 1;
	}
	return (U64)-1;
}

/* A freed page is filled with this; when it is handed out again it must still be all poison,
   or something wrote to it after it was freed (a use after free). Pages never freed are not
   poisoned (their first word differs) and aren't checked. */
#define PMM_POISON 0xF8EEDF8EEDF8EEDFULL

static void check_poison(U64 pg)
{
	const U64 *w = phys_to_virt(pg * PAGE_SIZE);
	U64 i;

	if (w[0] != PMM_POISON)
		return;
	for (i = 1; i < PAGE_SIZE / 8; i++)
		if (w[i] != PMM_POISON)
			panic("pmm: page %lx was written after it was freed (use after free: byte %lu)",
			      pg * PAGE_SIZE, i * 8);
}

static U64 take(U64 first, U64 n)
{
	U64 i;

	for (i = 0; i < n; i++) {
		check_poison(first + i);
		set_used(first + i);
	}
	nfree -= n;
	memset(phys_to_virt(first * PAGE_SIZE), 0, n * PAGE_SIZE);
	return first * PAGE_SIZE;
}

static U64 pmm_alloc_nolock(void)
{
	U64 lo = DMA_LIMIT / PAGE_SIZE, pg;

	if (!nfree)
		return 0;
	pg = search(hint, npages, 1);
	if (pg == (U64)-1)
		pg = search(lo, hint, 1);
	if (pg == (U64)-1)
		pg = search(0, lo, 1);		/* last resort: the DMA zone */
	if (pg == (U64)-1)
		return 0;
	hint = pg + 1 < npages ? pg + 1 : lo;
	return take(pg, 1);
}

static U64 pmm_alloc_contig_nolock(U64 n, U64 limit)
{
	U64 pg, top = limit / PAGE_SIZE;

	if (!n || n > nfree)
		return 0;
	if (top > npages)
		top = npages;
	pg = search(0, top, n);
	return pg == (U64)-1 ? 0 : take(pg, n);
}

static void pmm_free_nolock(U64 phys)
{
	U64 pg = phys / PAGE_SIZE;

	U64 *w, i;

	if (pg >= npages || !used(pg))
		panic("pmm_free: page %lx is not allocated (a double free?)", phys);
	w = phys_to_virt(pg * PAGE_SIZE);
	for (i = 0; i < PAGE_SIZE / 8; i++)
		w[i] = PMM_POISON;
	set_free(pg);
	nfree++;
	if (pg >= DMA_LIMIT / PAGE_SIZE && pg < hint)
		hint = pg;			/* hand it out next: a use after free shows up soon */
}

/* kcheck: the free counter must match the bitmap */
const char *pmm_check(void)
{
	U64 pg, n = 0;

	for (pg = 0; pg < npages; pg++)
		n += !used(pg);
	return n == nfree ? NULL : "pmm: the free page count doesn't match the page bitmap";
}

void pmm_test_corrupt(void)
{
	nfree++;
}

/* After vmm_init(): the boot mapping is gone, reach the bitmap through the direct map. */
void pmm_remap(void)
{
	bitmap = phys_to_virt(bitmap_phys);
}

U64 pmm_free_pages(void)  { return nfree; }
U64 pmm_total_pages(void) { return ntotal; }
U64 pmm_max_phys(void)    { return npages * PAGE_SIZE; }

/* ---- public entry points: interrupts off while the bitmap changes (see mem.c). ---- */

U64 pmm_alloc(void)
{
	U64 f = irq_save(), pa = pmm_alloc_nolock();

	irq_restore(f);
	return pa;
}

U64 pmm_alloc_contig(U64 n, U64 limit)
{
	U64 f = irq_save(), pa = pmm_alloc_contig_nolock(n, limit);

	irq_restore(f);
	return pa;
}

void pmm_free(U64 phys)
{
	U64 f = irq_save();

	pmm_free_nolock(phys);
	irq_restore(f);
}
