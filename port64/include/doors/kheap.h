/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Kernel heap (kernel/kheap.c): zeroed blocks of any size; NULL when memory runs out. */
#ifndef DOORS_KHEAP_H
#define DOORS_KHEAP_H

#include <doors/types.h>

void *kmalloc(U64 size);
void  kfree(void *p);			/* NULL is fine; a bad or double free panics */
void  kheap_stats(U64 *blocks, U64 *pages);
const char *kheap_check(void);		/* kcheck */

#endif
