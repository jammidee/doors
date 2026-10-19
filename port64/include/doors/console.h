/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS kernel console: job 1's virtual screen (include/doors/video.h) + COM1 serial, used for
   all kernel messages. */
#ifndef DOORS_CONSOLE_H
#define DOORS_CONSOLE_H

#include <doors/types.h>
#include <doors/video.h>

/* VGA text mode colours (same values as MMURTL's MVID.H attributes). */
enum vga_color {
	BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHTGRAY,
	DARKGRAY, LIGHTBLUE, LIGHTGREEN, LIGHTCYAN, LIGHTRED, LIGHTMAGENTA, YELLOW, WHITE
};

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

void console_putc(char c);
void console_write(const char *s);
void console_write_n(const char *s, U64 n);
int  kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
NORETURN void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
U64  console_ring(char *out, U64 max);	/* the last kernel messages (up to 16 KB), oldest first */

/* ---- debugging aids (kernel/debug.c, ROADMAP 2.4 / 2.7) ---- */

/* An internal invariant: panics with the expression, file and line when it doesn't hold. */
#define ASSERT(c) do { if (!(c)) panic("assertion failed: %s (%s:%d)", #c, __FILE__, __LINE__); } while (0)

struct regs;
extern const struct regs *panic_regs;	/* set by a kernel exception: the trace starts at its RIP */
void stack_trace(U64 rip, U64 rbp);	/* frame-pointer walk, with function names */
void panic_trace(U64 rbp);		/* panic's trace: from panic_regs if set, else from rbp */
const char *ksym_lookup(U64 addr, U64 *off);
void kcheck(void);			/* walk the kernel's structures; panic if one is inconsistent */
void stack_guard_init(void);		/* -fstack-protector: a new canary, first thing in kmain */
void crash_init(void);			/* after vol_init: show the last boot's crash report, once */
void crash_save(void);			/* from panic: the report to the disk's crash area */
void crash_show(void);			/* the monitor's crash command */
void crash_test(const char *cmdline);	/* "crash=NAME": trigger one aid on purpose (make crashtest) */

/* Per-module invariant checks used by kcheck: NULL if fine, else what is wrong. */
const char *sched_check(void);
const char *msg_check(void);
const char *pmm_check(void);

#endif
