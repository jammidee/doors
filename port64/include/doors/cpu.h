/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS x86-64 descriptor tables and interrupt frames. */
#ifndef DOORS_CPU_H
#define DOORS_CPU_H

#include <doors/types.h>

/* GDT selectors. The user selectors are ordered for SYSCALL/SYSRET (STAR[63:48] = 0x18):
   SYSRET loads SS = 0x18 + 8 = 0x20 and CS = 0x18 + 16 = 0x28. */
#define SEL_KCODE    0x08
#define SEL_KDATA    0x10
#define SEL_UCODE32  0x18	/* placeholder, unused */
#define SEL_UDATA    0x20
#define SEL_UCODE    0x28
#define SEL_TSS      0x30	/* 16-byte system descriptor */

/* Interrupt Stack Table slots (TSS.ist[n-1]). */
#define IST_DF       1		/* double fault */
#define IST_NMI      2		/* NMI and machine check */

/* Register frame built by isr.asm (lowest address first). */
struct regs {
	U64 r15, r14, r13, r12, r11, r10, r9, r8;
	U64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
	U64 vector, error;
	U64 rip, cs, rflags, rsp, ss;	/* pushed by the CPU */
};

/* IRQs from the remapped 8259 PICs. */
#define IRQ_BASE     0x20
#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1

typedef void (*isr_handler_t)(struct regs *r);

void gdt_init(void);
void tss_set_rsp0(U64 rsp);			/* kernel stack for ring 3 -> 0 entry */
void syscall_init(void);			/* kernel/syscall.c: SYSCALL/SYSRET MSRs */
void idt_init(void);
void isr_register(U8 vector, isr_handler_t h);
void irq_register(U8 irq, isr_handler_t h);
void dump_regs(const struct regs *r);
bool fault_fixup(struct regs *r);		/* resume a faulting probe_read/probe_write */
extern U64 last_fault_addr, last_fault_err;	/* CR2 and error code of the last fixed-up fault */

void pic_init(void);
void pic_unmask(U8 irq);
void pic_eoi(U8 irq);
bool pic_spurious(U8 irq);	/* true (and handled) for a spurious IRQ 7/15 */

void pit_init(U32 hz);
U64  timer_ticks(void);
void timer_sleep(U64 ticks);

void kbd_init(void);
int  kbd_getc(void);		/* -1 if no key waiting */
void kbd_set_exch(U32 exch);	/* send keys as messages to exch instead (0 = polling) */

static inline void sti(void) { __asm__ volatile("sti"); }

#endif
