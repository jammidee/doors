/* DOORS kernel-wide state and helpers. */
#ifndef DOORS_KERNEL_H
#define DOORS_KERNEL_H

#include <doors/types.h>

#define DOORS_VERSION "0.1"

/* Set when the kernel command line contains "test" (make test). */
extern bool test_mode;

/* QEMU isa-debug-exit (only attached by `make test`): QEMU exits with status (code << 1) | 1. */
#define QEMU_EXIT_PASS 0x10	/* status 33 */
#define QEMU_EXIT_FAIL 0x11	/* status 35 */
void qemu_exit(U8 code);

/* In-kernel milestone self-tests (kernel/selftest.c), run when test_mode is set. */
NORETURN void selftest_run(void);

#endif
