/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* 64-bit IDT, interrupt dispatch and exception reporting.
   Replaces MMURTL's 32-bit IDT (MOSIDT.ASM) and exception handlers (EXCEPT.ASM). As in MMURTL,
   an unhandled exception shows the faulting registers; here it is a panic until the
   debugger is ported (M9/M10). */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/job.h>
#include <doors/task.h>
#include <doors/x86.h>

struct idt_entry {
	U16 offset_lo;
	U16 selector;
	U8  ist;
	U8  type_attr;
	U16 offset_mid;
	U32 offset_hi;
	U32 reserved;
} PACKED;

struct idt_ptr {
	U16 limit;
	U64 base;
} PACKED;

static struct idt_entry idt[256] __attribute__((aligned(16)));
static isr_handler_t handlers[256];

extern const U64 isr_stub_table[256];	/* isr.asm */

static const char *const exc_names[32] = {
	"Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "BOUND range exceeded",
	"Invalid opcode", "Device not available", "Double fault", "Coprocessor segment overrun",
	"Invalid TSS", "Segment not present", "Stack-segment fault", "General protection fault",
	"Page fault", "Reserved", "x87 floating-point", "Alignment check", "Machine check",
	"SIMD floating-point", "Virtualization", "Control protection", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor injection",
	"VMM communication", "Security", "Reserved",
};

static void set_gate(U8 vec, U64 addr, U8 ist, U8 dpl)
{
	idt[vec].offset_lo = addr & 0xFFFF;
	idt[vec].selector = SEL_KCODE;
	idt[vec].ist = ist;
	idt[vec].type_attr = (U8)(0x80 | (dpl << 5) | 0x0E);	/* present, 64-bit interrupt gate */
	idt[vec].offset_mid = (addr >> 16) & 0xFFFF;
	idt[vec].offset_hi = (U32)(addr >> 32);
	idt[vec].reserved = 0;
}

void idt_init(void)
{
	struct idt_ptr p;
	unsigned v;

	for (v = 0; v < 256; v++)
		set_gate((U8)v, isr_stub_table[v], 0, 0);
	set_gate(2, isr_stub_table[2], IST_NMI, 0);	/* NMI */
	set_gate(3, isr_stub_table[3], 0, 3);		/* INT3 allowed from user mode later */
	set_gate(8, isr_stub_table[8], IST_DF, 0);	/* double fault: always a good stack */
	set_gate(18, isr_stub_table[18], IST_NMI, 0);	/* machine check */

	p.limit = sizeof(idt) - 1;
	p.base = (U64)(uintptr_t)idt;
	__asm__ volatile("lidt %0" : : "m"(p));
}

void isr_register(U8 vector, isr_handler_t h)
{
	handlers[vector] = h;
}

void irq_register(U8 irq, isr_handler_t h)
{
	handlers[IRQ_BASE + irq] = h;
	pic_unmask(irq);
}

void dump_regs(const struct regs *r)
{
	U64 cr2;

	__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
	kprintf("RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", r->rax, r->rbx, r->rcx, r->rdx);
	kprintf("RSI=%016lx RDI=%016lx RBP=%016lx RSP=%016lx\n", r->rsi, r->rdi, r->rbp, r->rsp);
	kprintf("R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n", r->r8, r->r9, r->r10, r->r11);
	kprintf("R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n", r->r12, r->r13, r->r14, r->r15);
	kprintf("RIP=%016lx CS=%04lx SS=%04lx RFLAGS=%08lx ERR=%lx\n",
		r->rip, r->cs, r->ss, r->rflags, r->error);
	kprintf("CR2=%016lx CR3=%016lx\n", cr2, read_cr3());
}

/* Fault fixups (probe.asm): if a #PF/#GP hit one of the marked probe instructions, resume at
   its fixup label instead of treating the fault as fatal. */
extern const U64 probe_fixups[];
U64 last_fault_addr, last_fault_err;

extern const U64 uaccess_fixups[];		/* uaccess.asm: user_copy */

bool fault_fixup(struct regs *r)
{
	const U64 *tables[] = { probe_fixups, uaccess_fixups };
	const U64 *f;
	unsigned i;

	if (r->cs & 3)
		return false;			/* only kernel instructions have fixups */
	for (i = 0; i < 2; i++) {
		for (f = tables[i]; f[0]; f += 2) {
			if (r->rip == f[0]) {
				__asm__ volatile("mov %%cr2, %0" : "=r"(last_fault_addr));
				last_fault_err = r->error;
				r->rip = f[1];
				return true;
			}
		}
	}
	return false;
}

void isr_dispatch(struct regs *r)
{
	U64 v = r->vector;

	if (v >= IRQ_BASE && v < IRQ_BASE + 16) {
		U8 irq = (U8)(v - IRQ_BASE);

		if (pic_spurious(irq))
			return;
		/* Acknowledge first (as MMURTL's IntTimer did before its task switch): a handler may
		   switch to another task, and this IRQ line must not stay blocked meanwhile.
		   Interrupts stay disabled until iretq, so this cannot nest. */
		pic_eoi(irq);
		if (handlers[v])
			handlers[v](r);
		sched_irq_exit();	/* a task the handler woke (ISendMsg) may outrank us */
		return;
	}

	if (handlers[v]) {
		handlers[v](r);
		return;
	}

	if ((v == 13 || v == 14) && fault_fixup(r))
		return;

	if (v < 32 && (r->cs & 3)) {
		/* A fault in ring 3 only ends that job (the kernel keeps running), as a bad MMURTL
		   program ended up terminated by the Monitor. */
		kprintf("\n*** CPU exception %lu: %s in job %u (task %u) ***\n", v, exc_names[v],
			current->job, current->tid);
		dump_regs(r);
		job_fault(r, (U32)v);		/* does not return */
	}
	if (v < 32) {
		U64 off;
		const char *fn = ksym_lookup(r->rip, &off);

		kprintf("\n*** CPU exception %lu: %s ***\n", v, exc_names[v]);
		dump_regs(r);
		panic_regs = r;			/* the trace starts where it happened */
		panic("unhandled exception %lu (%s) at %016lx in %s", v, exc_names[v], r->rip, fn ? fn : "?");
	}
	kprintf("DOORS: unexpected interrupt %lu\n", v);
}
