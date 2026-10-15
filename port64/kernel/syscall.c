/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
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
#include <doors/fs.h>
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
			U64 k = a[1] < sizeof(name) ? a[1] : sizeof(name), i;

			if ((erc = copy_in(name, a[0], k)))
				return erc;
			TTYOut(name, k, (U32)a[2]);
			for (i = 0; i < k; i++)		/* every job's text also goes to COM1 (test logs) */
				serial_putc(name[i]);
			a[0] += k;
			a[1] -= k;
		}
		return ErcOk;
	case SYS_PutVidChars: {
		char line[VID_COLS * VID_ROWS];

		ARGS(5);
		if (a[3] > sizeof(line))
			return ErcVidParam;
		if ((erc = copy_in(line, a[2], a[3])))
			return erc;
		return PutVidChars((U32)a[0], (U32)a[1], line, (U32)a[3], (U32)a[4]);
	}
	case SYS_PutVidAttrs:
		ARGS(4);
		return PutVidAttrs((U32)a[0], (U32)a[1], (U32)a[2], (U32)a[3]);
	case SYS_GetVidChar: {
		U8 c, at;

		ARGS(4);
		if ((erc = GetVidChar((U32)a[0], (U32)a[1], &c, &at)))
			return erc;
		if ((erc = copy_out(a[2], &c, 1)))
			return erc;
		return copy_out(a[3], &at, 1);
	}
	case SYS_SetVidOwner:
		ARGS(1);
		return SetVidOwner((U32)a[0]);
	case SYS_GetVidOwner:
		ARGS(1);
		GetVidOwner(&v32);
		return out_u32(a[0], v32);
	case SYS_SetNormVid:
		ARGS(1);
		return SetNormVid((U32)a[0]);
	case SYS_GetNormVid:
		ARGS(1);
		GetNormVid(&v32);
		return out_u32(a[0], v32);
	case SYS_ClrScr:
		return ClrScr();
	case SYS_SetXY:
		ARGS(2);
		return SetXY((U32)a[0], (U32)a[1]);
	case SYS_GetXY: {
		U32 x, y;

		ARGS(2);
		GetXY(&x, &y);
		if ((erc = out_u32(a[0], x)))
			return erc;
		return out_u32(a[1], y);
	}
	case SYS_ScrollVid:
		ARGS(5);
		return ScrollVid((U32)a[0], (U32)a[1], (U32)a[2], (U32)a[3], (U32)a[4]);
	case SYS_Beep:
		return Beep();
	case SYS_Tone:
		ARGS(2);
		return Tone((U32)a[0], (U32)a[1]);
	case SYS_ReadKbd:
		ARGS(2);
		if (!urange(a[0], 4))			/* check before blocking */
			return a[0] ? ErcBadLinAdd : ErcNullPtr;
		if ((erc = ReadKbd(&v32, (U32)a[1])))
			return erc;
		return out_u32(a[0], v32);
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
	/* ---- file system (MFILES.H). Paths are copied in; data buffers are checked and passed
	   on, the FILESYS service reaches them through its request's aliases. ---- */
	case SYS_OpenFile: {
		char path[MAX_PATH];

		ARGS(5);
		if (a[1] >= MAX_PATH)
			return ErcBadFileSpec;
		if ((erc = copy_in(path, a[0], a[1])))
			return erc;
		if (!urange(a[4], 4))
			return a[4] ? ErcBadLinAdd : ErcNullPtr;
		erc = OpenFile(path, (U32)a[1], (U32)a[2], (U32)a[3], &v32);
		if (!erc && (erc = out_u32(a[4], v32)))
			CloseFile(v32);
		return erc;
	}
	case SYS_CloseFile:
		ARGS(1);
		return CloseFile((U32)a[0]);
	case SYS_DeleteFile:
		ARGS(1);
		return DeleteFile((U32)a[0]);
	case SYS_CreateFile:
	case SYS_CreateDir:
	case SYS_DeleteDir: {
		char path[MAX_PATH];

		ARGS(3);
		if (a[1] >= MAX_PATH)
			return ErcBadFileSpec;
		if ((erc = copy_in(path, a[0], a[1])))
			return erc;
		if (n == SYS_CreateFile)
			return CreateFile(path, (U32)a[1], (U32)a[2]);
		return n == SYS_CreateDir ? CreateDir(path, (U32)a[1]) : DeleteDir(path, (U32)a[1]);
	}
	case SYS_RenameFile: {
		char from[MAX_PATH], to[MAX_PATH];

		ARGS(4);
		if (a[1] >= MAX_PATH || a[3] >= MAX_PATH)
			return ErcBadFileSpec;
		if ((erc = copy_in(from, a[0], a[1])) || (erc = copy_in(to, a[2], a[3])))
			return erc;
		return RenameFile(from, (U32)a[1], to, (U32)a[3]);
	}
	case SYS_ReadBytes:
	case SYS_WriteBytes:
		ARGS(4);
		if (!ubuf_ok(a[1], a[2]) || !urange(a[3], 8))
			return ErcBadLinAdd;
		erc = n == SYS_ReadBytes ? ReadBytes((U32)a[0], (U8 *)(uintptr_t)a[1], a[2], &h) :
			WriteBytes((U32)a[0], (const U8 *)(uintptr_t)a[1], a[2], &h);
		out_u64(a[3], h);
		return erc;
	case SYS_ReadBlock:
	case SYS_WriteBlock:
		ARGS(5);
		if (!ubuf_ok(a[1], a[2]) || !urange(a[4], 8))
			return ErcBadLinAdd;
		erc = n == SYS_ReadBlock ? ReadBlock((U32)a[0], (U8 *)(uintptr_t)a[1], a[2], a[3], &h) :
			WriteBlock((U32)a[0], (const U8 *)(uintptr_t)a[1], a[2], a[3], &h);
		out_u64(a[4], h);
		return erc;
	case SYS_GetFileSize:
	case SYS_GetFileLFA:
		ARGS(2);
		if (!urange(a[1], 8))
			return a[1] ? ErcBadLinAdd : ErcNullPtr;
		erc = n == SYS_GetFileSize ? GetFileSize((U32)a[0], &h) : GetFileLFA((U32)a[0], &h);
		if (!erc)
			erc = out_u64(a[1], h);
		return erc;
	case SYS_SetFileSize:
		ARGS(2);
		return SetFileSize((U32)a[0], a[1]);
	case SYS_SetFileLFA:
		ARGS(2);
		return SetFileLFA((U32)a[0], a[1]);
	case SYS_GetDirSector: {
		char path[MAX_PATH];

		ARGS(5);
		if (a[1] >= MAX_PATH)
			return ErcBadFileSpec;
		if ((erc = copy_in(path, a[0], a[1])))
			return erc;
		if (a[3] < 512 || !urange(a[2], 512))
			return ErcBadLinAdd;
		return GetDirSector(path, (U32)a[1], (U8 *)(uintptr_t)a[2], 512, (U32)a[4]);
	}
	case SYS_ReadDir: {
		char path[MAX_PATH];

		ARGS(4);
		if (a[1] >= MAX_PATH)
			return ErcBadFileSpec;
		if ((erc = copy_in(path, a[0], a[1])))
			return erc;
		if (!urange(a[3], sizeof(struct dir_entry)))
			return ErcBadLinAdd;
		return ReadDir(path, (U32)a[1], (U32)a[2], (struct dir_entry *)(uintptr_t)a[3]);
	}
	case SYS_GetSystemDisk: {
		U8 d;

		ARGS(1);
		GetSystemDisk(&d);
		return copy_out(a[0], &d, 1);
	}
	default:
		return ErcBadCallGate;
	}
#undef ARGS
}
