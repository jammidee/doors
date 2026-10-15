/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* 8259A PIC pair, remapped to vectors 0x20-0x2F (MMURTL's INTCODE.ASM: MaskIRQ, UnMaskIRQ,
   EndOfIRQ). All IRQs start masked; drivers unmask theirs through irq_register(). */

#include <doors/cpu.h>
#include <doors/x86.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20
#define PIC_READ_ISR 0x0B

static void io_wait(void)
{
	outb(0x80, 0);		/* POST port: a tiny delay for old PICs */
}

void pic_init(void)
{
	outb(PIC1_CMD, 0x11);	/* ICW1: edge, cascade, ICW4 needed */
	io_wait();
	outb(PIC2_CMD, 0x11);
	io_wait();
	outb(PIC1_DATA, IRQ_BASE);	/* ICW2: vector offsets */
	io_wait();
	outb(PIC2_DATA, IRQ_BASE + 8);
	io_wait();
	outb(PIC1_DATA, 0x04);	/* ICW3: slave on IRQ2 */
	io_wait();
	outb(PIC2_DATA, 0x02);
	io_wait();
	outb(PIC1_DATA, 0x01);	/* ICW4: 8086 mode */
	io_wait();
	outb(PIC2_DATA, 0x01);
	io_wait();

	outb(PIC1_DATA, 0xFB);	/* mask everything except the cascade line (IRQ2) */
	outb(PIC2_DATA, 0xFF);
}

void pic_unmask(U8 irq)
{
	U16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;

	outb(port, inb(port) & (U8)~(1u << (irq & 7)));
}

void pic_eoi(U8 irq)
{
	if (irq >= 8)
		outb(PIC2_CMD, PIC_EOI);
	outb(PIC1_CMD, PIC_EOI);
}

bool pic_spurious(U8 irq)
{
	if (irq == 7) {
		outb(PIC1_CMD, PIC_READ_ISR);
		return !(inb(PIC1_CMD) & 0x80);		/* no EOI for a spurious IRQ 7 */
	}
	if (irq == 15) {
		outb(PIC2_CMD, PIC_READ_ISR);
		if (!(inb(PIC2_CMD) & 0x80)) {
			outb(PIC1_CMD, PIC_EOI);	/* the master did see IRQ 2 */
			return true;
		}
	}
	return false;
}
