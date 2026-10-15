/* DOORS x86-64 CPU helpers (port I/O, control registers, MSRs). */
#ifndef DOORS_X86_H
#define DOORS_X86_H

#include <doors/types.h>

static inline void outb(U16 port, U8 v)  { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outw(U16 port, U16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outl(U16 port, U32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }

static inline U8 inb(U16 port)
{
	U8 v;
	__asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline U64 read_cr0(void) { U64 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline U64 read_cr3(void) { U64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline U64 read_cr4(void) { U64 v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }

static inline U64 rdmsr(U32 msr)
{
	U32 lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((U64)hi << 32) | lo;
}

static inline void wrmsr(U32 msr, U64 v)
{
	__asm__ volatile("wrmsr" : : "c"(msr), "a"((U32)v), "d"((U32)(v >> 32)));
}

#define MSR_EFER 0xC0000080
#define EFER_NXE (1ULL << 11)
#define EFER_LMA (1ULL << 10)

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }

static inline NORETURN void halt_forever(void)
{
	for (;;) {
		cli();
		hlt();
	}
}

#endif
