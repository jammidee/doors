/* VGA 80x25 text console (port of the essentials of OSSOURCE/VIDEO.ASM: TTY output,
   scrolling, hardware cursor). The buffer at physical 0xB8000 is reached via the higher half. */

#include <doors/console.h>
#include <doors/x86.h>

#define VGA_COLS 80
#define VGA_ROWS 25

static volatile U16 *const vga = (volatile U16 *)PHYS_TO_VIRT(0xB8000);
static U32 row, col;
static U8 attr = (BLACK << 4) | LIGHTGRAY;

static void update_cursor(void)
{
	U16 pos = (U16)(row * VGA_COLS + col);

	outb(0x3D4, 0x0F);
	outb(0x3D5, (U8)(pos & 0xFF));
	outb(0x3D4, 0x0E);
	outb(0x3D5, (U8)(pos >> 8));
}

/* Row 0 is a fixed status bar (title + tick count); text scrolls in rows 1..24. */
#define FIRST_ROW 1

static void scroll(void)
{
	U32 i;

	for (i = FIRST_ROW * VGA_COLS; i < (VGA_ROWS - 1) * VGA_COLS; i++)
		vga[i] = vga[i + VGA_COLS];
	for (; i < VGA_ROWS * VGA_COLS; i++)
		vga[i] = (U16)(attr << 8) | ' ';
	row = VGA_ROWS - 1;
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
	attr = (U8)((bg << 4) | fg);
}

void vga_init(void)
{
	U32 i;

	for (i = 0; i < VGA_ROWS * VGA_COLS; i++)
		vga[i] = (U16)(attr << 8) | ' ';
	row = FIRST_ROW;
	col = 0;
	update_cursor();
}

void vga_set_title(const char *title)
{
	const U16 a = (U16)((BLUE << 4) | WHITE) << 8;
	U32 i;

	for (i = 0; i < VGA_COLS; i++)
		vga[i] = a | (U8)(*title ? *title++ : ' ');
}

/* Called from the timer interrupt: writes straight into row 0 without moving the cursor. */
void vga_status_tick(U64 ticks)
{
	char num[21];
	int n = 0, i, col0;
	const U16 a = (U16)((BLUE << 4) | WHITE) << 8;

	do {
		num[n++] = (char)('0' + ticks % 10);
		ticks /= 10;
	} while (ticks && n < 20);

	col0 = VGA_COLS - 5 - n - 1;
	vga[col0 + 0] = a | 'T';
	vga[col0 + 1] = a | 'i';
	vga[col0 + 2] = a | 'c';
	vga[col0 + 3] = a | 'k';
	vga[col0 + 4] = a | ':';
	for (i = 0; i < n; i++)
		vga[col0 + 5 + i] = a | (U8)num[n - 1 - i];
	vga[VGA_COLS - 1] = a | ' ';
}

void vga_putc(char c)
{
	switch (c) {
	case '\n':
		col = 0;
		row++;
		break;
	case '\r':
		col = 0;
		break;
	case '\t':
		col = (col + 8) & ~7u;
		break;
	case '\b':
		if (col)
			col--;
		break;
	default:
		vga[row * VGA_COLS + col] = (U16)(attr << 8) | (U8)c;
		col++;
	}
	if (col >= VGA_COLS) {
		col = 0;
		row++;
	}
	if (row >= VGA_ROWS)
		scroll();
	update_cursor();
}
