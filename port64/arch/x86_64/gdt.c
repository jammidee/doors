/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* 64-bit GDT and TSS.
   In long mode the TSS no longer holds task state (MMURTL's hardware task switching is gone,
   see docs/PORT64_PLAN.md); it only provides RSP0 for ring 3 -> 0 transitions and the
   Interrupt Stack Table used for #DF and NMI/#MC. */

#include <doors/cpu.h>

struct tss64 {
	U32 reserved0;
	U64 rsp[3];
	U64 reserved1;
	U64 ist[7];
	U64 reserved2;
	U16 reserved3;
	U16 iomap_base;
} PACKED;

static struct tss64 tss;
static U8 df_stack[8192] __attribute__((aligned(16)));
static U8 nmi_stack[8192] __attribute__((aligned(16)));

extern U8 kernel_stack_top[];	/* boot.asm */

static U64 gdt[8] __attribute__((aligned(16))) = {
	[0]                = 0,
	[SEL_KCODE >> 3]   = 0x00AF9A000000FFFFULL,	/* 64-bit code, DPL 0 */
	[SEL_KDATA >> 3]   = 0x00CF92000000FFFFULL,	/* data, DPL 0 */
	[SEL_UCODE32 >> 3] = 0x00CFFA000000FFFFULL,	/* 32-bit code, DPL 3 (unused) */
	[SEL_UDATA >> 3]   = 0x00CFF2000000FFFFULL,	/* data, DPL 3 */
	[SEL_UCODE >> 3]   = 0x00AFFA000000FFFFULL,	/* 64-bit code, DPL 3 */
	/* [6], [7]: TSS descriptor, filled in by gdt_init() */
};

struct gdt_ptr {
	U16 limit;
	U64 base;
} PACKED;

void gdt_load(const struct gdt_ptr *p, U16 code_sel, U16 data_sel, U16 tss_sel);	/* isr.asm */

void tss_set_rsp0(U64 rsp)
{
	tss.rsp[0] = rsp;
}

void gdt_init(void)
{
	U64 base = (U64)(uintptr_t)&tss;
	U64 limit = sizeof(tss) - 1;
	struct gdt_ptr p;

	tss.rsp[0] = (U64)(uintptr_t)kernel_stack_top;
	tss.ist[IST_DF - 1] = (U64)(uintptr_t)(df_stack + sizeof(df_stack));
	tss.ist[IST_NMI - 1] = (U64)(uintptr_t)(nmi_stack + sizeof(nmi_stack));
	tss.iomap_base = sizeof(tss);	/* no I/O permission bitmap */

	/* 64-bit available TSS (type 9), present, DPL 0. */
	gdt[SEL_TSS >> 3] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
			    (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
	gdt[(SEL_TSS >> 3) + 1] = base >> 32;

	p.limit = sizeof(gdt) - 1;
	p.base = (U64)(uintptr_t)gdt;
	gdt_load(&p, SEL_KCODE, SEL_KDATA, SEL_TSS);
}
