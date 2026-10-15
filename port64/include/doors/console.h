/* DOORS kernel console: VGA text screen + COM1 serial, used for all kernel messages. */
#ifndef DOORS_CONSOLE_H
#define DOORS_CONSOLE_H

#include <doors/types.h>

/* VGA text mode colours (same values as MMURTL's MVID.H attributes). */
enum vga_color {
	BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHTGRAY,
	DARKGRAY, LIGHTBLUE, LIGHTGREEN, LIGHTCYAN, LIGHTRED, LIGHTMAGENTA, YELLOW, WHITE
};

void serial_init(void);
void serial_putc(char c);

void vga_init(void);
void vga_putc(char c);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_status_tick(U64 ticks);	/* "Tick:n" at the top right, like MMURTL's Monitor */
void vga_set_title(const char *title);
void vga_remap(void);			/* after vmm_init(): reach VGA memory via the direct map */	/* row 0 status bar (text scrolls in rows 1..24) */
void serial_write(const char *s);

void console_putc(char c);
void console_write(const char *s);
int  kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
NORETURN void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
