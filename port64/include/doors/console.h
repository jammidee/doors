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

#endif
