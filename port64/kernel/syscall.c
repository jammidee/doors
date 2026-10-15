/* System calls from ring 3 (replaces MMURTL's call gates, MPUBLICS.ASM).

   Entry: arch/x86_64/syscall.asm. RAX = call number (include/doors/syscalls.def, MMURTL order),
   RDI = user pointer to the arguments, each a U64, in MMURTL parameter order.

   Every user pointer is checked before use: it must lie in the user half (USER_TOP) and is
   only touched through user_copy(), which turns a fault into an error instead of a crash.
   Pointers are never passed through to kernel calls unchecked; in particular Request's data
   buffers must be user addresses, otherwise the service's alias of them could reach kernel
   memory. Request handles are translated to and from the read-only RQB_UBASE window.

   Calls that are not ported yet, or that only the kernel may use (port I/O, IRQs, AllocOSPage,
   ISendMsg, NewTask, ...), return ErcBadCallGate, MMURTL's answer for an uninitialised gate.
   The memory helpers (CopyData, FillData, Compare...) live in the user library instead. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/job.h>
#include <doors/mem.h>
#include <doors/msg.h>
#include <doors/string.h>
#include <doors/task.h>
#include <doors/x86.h>

enum {
#define X(num, name) SYS_##name = num,
#include <doors/syscalls.def>
#undef X
	SYS_COUNT
};

extern void syscall_entry(void);
extern U64 user_copy(void *dst, const void *src, U64 n);	/* uaccess.asm */

#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_FMASK  0xC0000084

void syscall_init(void)
{
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);			/* EFER.SCE */
	/* SYSCALL: CS = 0x08, SS = 0x10. SYSRET (64-bit): CS = 0x18+16 | 3, SS = 0x18+8 | 3. */
	wrmsr(MSR_STAR, ((U64)SEL_UCODE32 << 48) | ((U64)SEL_KCODE << 32));
	wrmsr(MSR_LSTAR, (U64)(uintptr_t)syscall_entry);
	wrmsr(MSR_FMASK, 0x200 | 0x100 | 0x400 | 0x40000);	/* IF, TF, DF, AC cleared on entry */
}

/* ---- user memory ---- */

static bool urange(U64 p, U64 n)
{
	return p >= PAGE_SIZE && p < USER_TOP && n <= USER_TOP - p;
}

static erc_t copy_in(void *dst, U64 usrc, U64 n)
{
	if (!usrc)
		return ErcNullPtr;
	if (!urange(usrc, n) || user_copy(dst, (const void *)(uintptr_t)usrc, n))
		return ErcBadLinAdd;
	return ErcOk;
}

static erc_t copy_out(U64 udst, const void *src, U64 n)
{
	if (!udst)
		return ErcNullPtr;
	if (!urange(udst, n) || user_copy((void *)(uintptr_t)udst, src, n))
		return ErcBadLinAdd;
	return ErcOk;
}

static erc_t out_u32(U64 udst, U32 v) { return copy_out(udst, &v, sizeof(v)); }
static erc_t out_u64(U64 udst, U64 v) { return copy_out(udst, &v, sizeof(v)); }

/* An optional user buffer: (NULL, 0) is fine, anything else must be inside the user half. */
static bool ubuf_ok(U64 p, U64 n)
{
	return (!p && !n) || (p && urange(p, n));
}

static erc_t out_msg(U64 udst, U64 *m)
{
	m[0] = rqb_to_user(m[0]);	/* request handles: the user-visible window */
	return copy_out(udst, m, 2 * sizeof(U64));
}

/* ---- dispatch ---- */

U64 syscall_dispatch(U64 n, U64 uargs)
{
	U64 a[12], m[2], h;
	char name[32];
	erc_t erc;
	U32 v32;
	U8 *p;

#define ARGS(k) do { erc = copy_in(a, uargs, (k) * sizeof(U64)); if (erc) return erc; } while (0)

	switch (n) {
	case SYS_WaitMsg:
	case SYS_CheckMsg:
		ARGS(2);
		if (!urange(a[1], 2 * sizeof(U64)))
			return a[1] ? ErcBadLinAdd : ErcNullPtr;	/* check before blocking */
		erc = n == SYS_WaitMsg ? WaitMsg((U32)a[0], m) : CheckMsg((U32)a[0], m);
		if (erc == ErcOk || erc == ErcNotRqBlk || erc == ErcBadLinAdd || erc == ErcBadJobNum ||
		    erc == ErcNoMem) {
			erc_t e2 = out_msg(a[1], m);

			return erc ? erc : e2;
		}
		return erc;
	case SYS_SendMsg:
		ARGS(3);
		return SendMsg((U32)a[0], a[1], a[2]);
	case SYS_SetPriority:
		ARGS(1);
		return SetPriority((U32)a[0]);
	case SYS_Request:
		ARGS(12);
		if ((erc = copy_in(name, a[0], 8)))
			return erc;
		if (!urange(a[3], sizeof(U64)))
			return a[3] ? ErcBadLinAdd : ErcNullPtr;
		if (!ubuf_ok(a[5], a[6]) || !ubuf_ok(a[7], a[8]))
			return ErcBadLinAdd;
		erc = Request(name, (U32)a[1], (U32)a[2], &h, (U32)a[4], (U8 *)(uintptr_t)a[5], a[6],
			      (U8 *)(uintptr_t)a[7], a[8], (U32)a[9], (U32)a[10], (U32)a[11]);
		if (!erc)
			out_u64(a[3], rqb_to_user(h));
		return erc;
	case SYS_Respond:
		ARGS(2);
		return Respond(rqb_from_user(a[0]), (U32)a[1]);
	case SYS_MoveRequest:
		ARGS(2);
		return MoveRequest(rqb_from_user(a[0]), (U32)a[1]);
	case SYS_AllocExch:
		ARGS(1);
		if (!urange(a[0], 4))
			return a[0] ? ErcBadLinAdd : ErcNullPtr;
		erc = AllocExch(&v32);
		if (!erc && (erc = out_u32(a[0], v32)))
			DeAllocExch(v32);
		return erc;
	case SYS_DeAllocExch:
		ARGS(1);
		return DeAllocExch((U32)a[0]);
	case SYS_GetTSSExch:
		ARGS(1);
		return out_u32(a[0], current->exch);
	case SYS_Sleep:
		ARGS(1);
		return Sleep((U32)a[0]);
	case SYS_Alarm:
		ARGS(2);
		return Alarm((U32)a[0], (U32)a[1]);
	case SYS_KillAlarm:
		ARGS(1);
		return KillAlarm((U32)a[0]);
	case SYS_GetTimerTick:
		ARGS(1);
		return out_u64(a[0], timer_ticks());
	case SYS_RegisterSvc:
		ARGS(2);
		if ((erc = copy_in(name, a[0], 8)))
			return erc;
		if ((erc = exch_check((U32)a[1], &v32)))
			return erc;
		if (v32 != current->job)
			return ErcNotOwner;		/* only your own exchange */
		return RegisterSvc(name, (U32)a[1]);
	case SYS_UnRegisterSvc:
		ARGS(1);
		if ((erc = copy_in(name, a[0], 8)))
			return erc;
		return UnRegisterSvc(name);
	case SYS_AllocPage:
		ARGS(2);
		if (!urange(a[1], sizeof(U64)))
			return a[1] ? ErcBadLinAdd : ErcNullPtr;
		erc = AllocPage((U32)a[0], &p);
		if (!erc && (erc = out_u64(a[1], (U64)(uintptr_t)p)))
			DeAllocPage(p, (U32)a[0]);
		return erc;
	case SYS_DeAllocPage:
		ARGS(2);
		if (!urange(a[0], a[1] * PAGE_SIZE))
			return ErcBadLinAdd;
		return DeAllocPage((U8 *)(uintptr_t)a[0], (U32)a[1]);
	case SYS_QueryPages:
		ARGS(1);
		QueryPages(&v32);
		return out_u32(a[0], v32);
	case SYS_AliasMem:
		ARGS(4);
		if (!urange(a[0], a[1]) || !urange(a[3], sizeof(U64)))
			return ErcBadLinAdd;
		erc = AliasMem((U8 *)(uintptr_t)a[0], a[1], (U32)a[2], &p);
		if (!erc)
			erc = out_u64(a[3], (U64)(uintptr_t)p);
		return erc;
	case SYS_DeAliasMem:
		ARGS(3);
		if (!urange(a[0], a[1]))
			return ErcBadLinAdd;
		return DeAliasMem((U8 *)(uintptr_t)a[0], a[1], (U32)a[2]);
	case SYS_SpawnTask:
		ARGS(5);
		if (a[4])
			return ErcBadCallGate;		/* fOSCode: kernel tasks only from the kernel */
		if (!urange(a[0], 1) || !urange(a[3] - 1, 1))
			return ErcBadLinAdd;
		return task_create_user("task", current->job, a[0], a[3] & ~15UL, (U8)a[1], &erc) ?
			ErcOk : erc;
	case SYS_TTYOut:
		ARGS(3);
		while (a[1]) {
			U64 k = a[1] < sizeof(name) ? a[1] : sizeof(name);

			if ((erc = copy_in(name, a[0], k)))
				return erc;
			console_write_n(name, k);
			a[0] += k;
			a[1] -= k;
		}
		return ErcOk;
	case SYS_GetJobNum:
		ARGS(1);
		return out_u32(a[0], current->job);
	case SYS_ExitJob:
		ARGS(1);
		ExitJob((erc_t)a[0]);
	case SYS_KillJob:
		ARGS(1);
		return KillJob((U32)a[0]);
	case SYS_LoadNewJob:
		ARGS(3);
		if (a[1] >= sizeof(name))
			return ErcNoSuchFile;
		if ((erc = copy_in(name, a[0], a[1])))
			return erc;
		if (!urange(a[2], 4))
			return a[2] ? ErcBadLinAdd : ErcNullPtr;
		erc = LoadNewJob(name, (U32)a[1], &v32);
		if (!erc)
			erc = out_u32(a[2], v32);
		return erc;
	case SYS_SetJobName:
		ARGS(2);
		if (a[1] > 15)
			a[1] = 15;
		if ((erc = copy_in(name, a[0], a[1])))
			return erc;
		return SetJobName(name, (U32)a[1]);
	case SYS_GetCmdLine: {
		char cmd[80];

		ARGS(2);
		GetCmdLine(cmd, &v32);
		if ((erc = copy_out(a[0], cmd, v32)))
			return erc;
		return out_u32(a[1], v32);
	}
	case SYS_SetCmdLine: {
		char cmd[80];

		ARGS(2);
		if (a[1] > sizeof(cmd) - 1)
			a[1] = sizeof(cmd) - 1;
		if ((erc = copy_in(cmd, a[0], a[1])))
			return erc;
		return SetCmdLine(cmd, (U32)a[1]);
	}
	default:
		return ErcBadCallGate;
	}
#undef ARGS
}
