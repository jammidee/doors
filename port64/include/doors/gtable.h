/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Growable kernel tables (ROADMAP 3.1, kernel/gtable.c). Each table is an ordinary array at its own
   16 GiB slice of virtual memory (GT_BASE, PML4 387); pages are mapped as it grows, so its size is
   bounded by memory, with a large ceiling (max) instead of a small compile-time limit. Entries
   never move, so pointers to them stay valid. A table never shrinks: its `n` entries stay mapped.
   Code loops to t->n (entries that exist), not to the ceiling. */
#ifndef DOORS_GTABLE_H
#define DOORS_GTABLE_H

#include <doors/types.h>

#define GT_BASE  0xFFFFC18000000000UL	/* PML4 387 */
#define GT_SLICE (16ULL << 30)

enum gt_slot {				/* one slice each */
	GT_TASKS, GT_JOBS, GT_SCREENS, GT_KEYQ, GT_KPEND, GT_KSTALE, GT_SPACES, GT_EXCH, GT_LB,
	GT_RQB, GT_FILES, GT_NSLOTS
};

struct gtable {
	const char *name;
	U8 *base;			/* entry i is at base + i * esize */
	U32 esize, max;			/* entry size, ceiling */
	U32 n;				/* entries mapped (0..n-1 usable) */
	U64 pages;			/* pages mapped */
	U64 uwin;			/* also mapped read-only for user mode here (RQBs), or 0 */
};

/* Map the first `first` entries. Panics if that much memory isn't there (boot only). */
void gt_init(struct gtable *t, const char *name, enum gt_slot slot, U32 esize, U32 max, U32 first,
	     U64 uwin);
/* Make at least `want` entries usable (new ones zeroed). False if over the ceiling or out of
   memory; the table then stays as it was. Interrupts off, or the table's own lock. */
bool gt_grow(struct gtable *t, U32 want);
U64  gt_pages_total(void);		/* pages all tables hold (leak checks count them as free) */
void gt_report(void);			/* the monitor's tables: name, entries, ceiling, KB */
U32  gt_limit(enum gt_slot slot, U32 max);	/* tests: lower a ceiling (0: to the current size) */

#endif
