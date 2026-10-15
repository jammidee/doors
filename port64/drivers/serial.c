/* COM1 serial output (16550 UART), 115200 8N1. Used as the kernel debug log from M1 on,
   so failures can be read without the screen (QEMU: -serial stdio / -serial file:...). */

#include <doors/console.h>
#include <doors/x86.h>

#define COM1 0x3F8

void serial_init(void)
{
	outb(COM1 + 1, 0x00);	/* interrupts off */
	outb(COM1 + 3, 0x80);	/* DLAB on */
	outb(COM1 + 0, 0x01);	/* divisor 1 = 115200 baud */
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03);	/* 8 bits, no parity, 1 stop, DLAB off */
	outb(COM1 + 2, 0xC7);	/* FIFO on, cleared, 14-byte threshold */
	outb(COM1 + 4, 0x03);	/* DTR + RTS */
}

void serial_putc(char c)
{
	if (c == '\n')
		serial_putc('\r');
	while ((inb(COM1 + 5) & 0x20) == 0)	/* wait for transmit holding register empty */
		;
	outb(COM1, (U8)c);
}
