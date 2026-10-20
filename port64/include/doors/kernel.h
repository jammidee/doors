/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS kernel-wide state and helpers. */
#ifndef DOORS_KERNEL_H
#define DOORS_KERNEL_H

#include <doors/types.h>

/* Set when the kernel command line contains "test" (make test). */
extern bool test_mode;

/* "1.9.0 v1.9.0-3-gabcdef": the version and build (kernel/main.c, the only user of version.h) */
extern const char doors_build[];

/* QEMU isa-debug-exit (only attached by `make test`): QEMU exits with status (code << 1) | 1. */
#define QEMU_EXIT_PASS 0x10	/* status 33 */
#define QEMU_EXIT_FAIL 0x11	/* status 35 */
void qemu_exit(U8 code);

/* In-kernel milestone self-tests (kernel/selftest.c), run when test_mode is set. full: also fill
   every volume to 100 % and back (make fulltest, kernel command line "test full"). */
NORETURN void selftest_run(bool full);

/* Soak test (kernel/soak.c, make soak): kernel command line "soak minutes=M [seed=S]". */
NORETURN void soak_run(const char *cmdline);

#endif
