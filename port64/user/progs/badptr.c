/* badptr: a misbehaving program. Every bad pointer it hands the kernel must come back as an error
   (never a kernel crash); then it touches unmapped memory itself, and the kernel must end just
   this job (ErcCpuFault + 14) and carry on. */

#include "doors.h"

static int checks, ok;

static void expect(const char *what, erc_t got, erc_t want)
{
	checks++;
	if (got == want)
		ok++;
	else
		printf("badptr: %s: got %u, expected %u\n", what, got, want);
}

int main(void)
{
	U32 e;
	U64 m[2];
	U8 *p;
	static const char ro[] = "read-only";

	expect("TTYOut(NULL)", TTYOut(0, 5, 7), ErcNullPtr);
	expect("TTYOut(kernel address)", TTYOut((const char *)0xFFFFFFFF80100000UL, 5, 7), ErcBadLinAdd);
	expect("TTYOut(unmapped)", TTYOut((const char *)0x500000000000UL, 5, 7), ErcBadLinAdd);
	expect("AllocExch(NULL)", AllocExch(0), ErcNullPtr);
	expect("AllocExch(into code)", AllocExch((U32 *)(uintptr_t)main), ErcBadLinAdd);
	expect("AllocExch(into rodata)", AllocExch((U32 *)ro), ErcBadLinAdd);
	expect("AllocPage(kernel ptr)", AllocPage(1, (U8 **)0xFFFF800000000000UL), ErcBadLinAdd);
	expect("WaitMsg(kernel ptr)", WaitMsg(1, (U64 *)0xFFFFFFFF80000000UL), ErcBadLinAdd);
	expect("Request(kernel buffer)",
	       Request("KECHO   ", 1, 1, m, 0, (U8 *)0xFFFFFFFF80100000UL, 16, 0, 0, 0, 0, 0),
	       ErcBadLinAdd);
	expect("unknown system call", (erc_t)doors_syscall(9999, m), ErcBadCallGate);
	expect("AllocOSPage (kernel only)", (erc_t)doors_syscall(12, m), ErcBadCallGate);
	expect("SpawnTask(fOSCode=1)", SpawnTask((void (*)(void))main, 25, 0, (U8 *)m, 1), ErcBadCallGate);
	expect("KillJob(1)", KillJob(1), ErcBadJobNum);
	expect("DeAllocExch(0)", DeAllocExch(0), ErcOutOfRange);
	if (AllocExch(&e) == ErcOk)
		expect("CheckMsg(empty)", CheckMsg(e, m), ErcNoMsg);
	if (AllocPage(1, &p) == ErcOk)
		expect("DeAllocPage(twice)", DeAllocPage(p, 1) == ErcOk ? DeAllocPage(p, 1) : 0, ErcBadLinAdd);

	if (ok == checks)
		printf("badptr: all %d bad calls rejected with the right error\n", checks);
	else
		printf("badptr: FAILED, %d of %d bad calls rejected correctly\n", ok, checks);
	printf("badptr: now writing to address 8 (unmapped)\n");
	*(volatile int *)8 = 1;
	printf("badptr: still alive?!\n");
	return 99;
}
