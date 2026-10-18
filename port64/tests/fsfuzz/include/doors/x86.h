/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Host stand-in for include/doors/x86.h (tests/fsfuzz): the file system code runs as a Linux
   program, where cli/hlt/port I/O would fault. Only what the file system modules use. */
#ifndef DOORS_X86_H
#define DOORS_X86_H

#include <doors/types.h>
#include <stdlib.h>

static inline U64 irq_save(void) { return 0; }
static inline void irq_restore(U64 f) { (void)f; }
static inline void cli(void) { }
static inline void hlt(void) { abort(); }	/* only the power-cut stop halts: never in the fuzzer */

#endif
