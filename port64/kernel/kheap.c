/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Kernel heap (ROADMAP 3.1): kmalloc/kfree for kernel objects of any size.
   - up to 2 KB: size classes of 16 .. 2048 bytes (powers of two); each class has a free list and
     takes a fresh OS page (AllocOSPage) when it is empty;
   - larger: whole OS pages.
   Every block starts with a 16-byte header (magic, class or page count), so kfree knows what it
   frees, and a bad or double free panics. Blocks are zeroed. Interrupts off while the lists change. */

#include <doors/console.h>
#include <doors/kheap.h>
#include <doors/mem.h>
#include <doors/string.h>
#include <doors/x86.h>

#define HDR       16
#define NCLASS    8			/* 16, 32, ..., 2048 */
#define KH_MAGIC  0x4B48454150ULL	/* "KHEAP" */
#define KH_FREE   0x46524545ULL		/* "FREE" (in a free block's header) */

struct hdr {
	U64 magic;
	U32 cls;			/* class, or NCLASS for a page run */
	U32 pages;			/* page run: its length */
};

struct fblk {
	struct hdr h;
	struct fblk *next;
};

static struct fblk *freel[NCLASS];
static U64 in_use, pages_held;

static U32 class_of(U64 size)
{
	U32 c = 0;

	while ((16ULL << c) < size + HDR)
		c++;
	return c;
}

void *kmalloc(U64 size)
{
	U64 f;
	U32 c;
	U8 *p;

	if (!size)
		return NULL;
	if (size + HDR > (16U << (NCLASS - 1))) {		/* whole pages */
		U32 n = (U32)((size + HDR + PAGE_SIZE - 1) / PAGE_SIZE);
		struct hdr *h;

		if (AllocOSPage(n, &p))
			return NULL;
		h = (struct hdr *)p;
		h->magic = KH_MAGIC;
		h->cls = NCLASS;
		h->pages = n;
		f = irq_save();
		in_use++;
		pages_held += n;
		irq_restore(f);
		return p + HDR;
	}
	c = class_of(size);
	f = irq_save();
	if (!freel[c]) {					/* a new page for this class */
		U32 bs = 16U << c, i;

		if (AllocOSPage(1, &p)) {
			irq_restore(f);
			return NULL;
		}
		pages_held++;
		for (i = 0; i + bs <= PAGE_SIZE; i += bs) {
			struct fblk *b = (struct fblk *)(p + i);

			b->h.magic = KH_FREE;
			b->h.cls = c;
			b->next = freel[c];
			freel[c] = b;
		}
	}
	p = (U8 *)freel[c];
	freel[c] = freel[c]->next;
	in_use++;
	irq_restore(f);
	memset(p, 0, 16U << c);
	((struct hdr *)p)->magic = KH_MAGIC;
	((struct hdr *)p)->cls = c;
	return p + HDR;
}

void kfree(void *ptr)
{
	struct hdr *h;
	U64 f;

	if (!ptr)
		return;
	h = (struct hdr *)((U8 *)ptr - HDR);
	if (h->magic != KH_MAGIC || h->cls > NCLASS)
		panic("kfree: %p is not a heap block (or was freed already)", ptr);
	if (h->cls == NCLASS) {
		U32 n = h->pages;

		h->magic = 0;
		f = irq_save();
		in_use--;
		pages_held -= n;
		irq_restore(f);
		DeAllocPage((U8 *)h, n);
		return;
	}
	f = irq_save();
	h->magic = KH_FREE;
	((struct fblk *)h)->next = freel[h->cls];
	freel[h->cls] = (struct fblk *)h;
	in_use--;
	irq_restore(f);
}

void kheap_stats(U64 *blocks, U64 *pages)
{
	*blocks = in_use;
	*pages = pages_held;
}

/* kcheck: every block on a free list is marked free and in its own class */
const char *kheap_check(void)
{
	U32 c;

	for (c = 0; c < NCLASS; c++) {
		const struct fblk *b;
		U64 n = 0;

		for (b = freel[c]; b; b = b->next) {
			if (b->h.magic != KH_FREE || b->h.cls != c)
				return "kheap: a free block was written (or is on the wrong list)";
			if (++n > pages_held * (PAGE_SIZE / 16))
				return "kheap: a free list has a loop";
		}
	}
	return NULL;
}
