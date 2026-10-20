/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Growable kernel tables: see include/doors/gtable.h. The kernel half of every address space
   shares its page tables below PML4, so a page mapped here later is seen by every job at once. */

#include <doors/console.h>
#include <doors/gtable.h>
#include <doors/mem.h>
#include <doors/x86.h>

static struct gtable *all[GT_NSLOTS];

static bool map_pages(struct gtable *t, U64 upto)
{
	while (t->pages < upto) {
		U64 va = (U64)(uintptr_t)t->base + t->pages * PAGE_SIZE, pa = pmm_alloc();

		if (!pa)
			return false;
		if (vmm_map(&kernel_space, va, pa, PTE_W | PTE_NX) ||
		    (t->uwin && vmm_map(&kernel_space, t->uwin + t->pages * PAGE_SIZE, pa, PTE_U | PTE_NX))) {
			if (!vmm_translate(&kernel_space, va, NULL, NULL))
				pmm_free(pa);
			return false;
		}
		t->pages++;
	}
	return true;
}

bool gt_grow(struct gtable *t, U32 want)
{
	U64 f = irq_save(), pages;
	bool ok;

	if (want <= t->n) {
		irq_restore(f);
		return true;
	}
	if (want > t->max) {
		irq_restore(f);
		return false;
	}
	pages = ((U64)want * t->esize + PAGE_SIZE - 1) / PAGE_SIZE;
	ok = map_pages(t, pages);
	t->n = (U32)(t->pages * PAGE_SIZE / t->esize);	/* whatever got mapped is usable */
	if (t->n > t->max)
		t->n = t->max;
	irq_restore(f);
	return ok && t->n >= want;
}

void gt_init(struct gtable *t, const char *name, enum gt_slot slot, U32 esize, U32 max, U32 first,
	     U64 uwin)
{
	t->name = name;
	t->base = (U8 *)(uintptr_t)(GT_BASE + (U64)slot * GT_SLICE);
	t->esize = esize;
	t->max = max;
	t->n = 0;
	t->pages = 0;
	t->uwin = uwin;
	ASSERT((U64)max * esize <= GT_SLICE);
	all[slot] = t;
	if (!gt_grow(t, first))
		panic("gtable %s: no memory for %u entries", name, first);
}

U64 gt_pages_total(void)
{
	U64 n = 0;
	U32 i;

	for (i = 0; i < GT_NSLOTS; i++)
		if (all[i])
			n += all[i]->pages;
	return n;
}

void gt_report(void)
{
	U32 i;

	for (i = 0; i < GT_NSLOTS; i++)
		if (all[i])
			kprintf("  %-14s %6u entries (ceiling %6u), %4lu KB\n", all[i]->name, all[i]->n,
				all[i]->max, all[i]->pages * 4);
}
