/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS user library: system call stubs (numbers from doors/syscalls.def, so they match MMURTL's
   call gate order), the MMURTL memory helpers, and a small printf on top of TTYOut. */

#include <stdarg.h>
#include "doors.h"

enum {
#define X(num, name) SYS_##name = num,
#include <doors/syscalls.def>
#undef X
};

/* RAX = number, RDI = &args; result in RAX. The kernel clears RDI, RSI, RDX, R8-R10 on return
   and SYSCALL/SYSRET use RCX and R11. */
U64 doors_syscall(U64 num, const U64 *args)
{
	U64 r = num;
	const U64 *a = args;

	__asm__ volatile("syscall"
			 : "+a"(r), "+D"(a)
			 :
			 : "rcx", "r11", "rdx", "rsi", "r8", "r9", "r10", "memory");
	return r;
}

#define A(x) ((U64)(uintptr_t)(x))
#define CALL(name, ...) ({ const U64 _a[] = { __VA_ARGS__ }; (erc_t)doors_syscall(SYS_##name, _a); })

erc_t AllocExch(U32 *p)			{ return CALL(AllocExch, A(p)); }
erc_t DeAllocExch(U32 e)		{ return CALL(DeAllocExch, e); }
erc_t GetTSSExch(U32 *p)		{ return CALL(GetTSSExch, A(p)); }
erc_t SendMsg(U32 e, U64 m1, U64 m2)	{ return CALL(SendMsg, e, m1, m2); }
erc_t WaitMsg(U32 e, U64 *m)		{ return CALL(WaitMsg, e, A(m)); }
erc_t CheckMsg(U32 e, U64 *m)		{ return CALL(CheckMsg, e, A(m)); }
erc_t Respond(U64 h, U32 erc)		{ return CALL(Respond, h, erc); }
erc_t MoveRequest(U64 h, U32 e)		{ return CALL(MoveRequest, h, e); }
erc_t RegisterSvc(const char *n, U32 e)	{ return CALL(RegisterSvc, A(n), e); }
erc_t UnRegisterSvc(const char *n)	{ return CALL(UnRegisterSvc, A(n)); }

erc_t Request(const char *pSvcName, U32 wSvcCode, U32 dRespExch, U64 *pRqHndlRet,
	      U32 dnpSend, U8 *pData1, U64 dcbData1, U8 *pData2, U64 dcbData2,
	      U32 dData0, U32 dData1, U32 dData2)
{
	return CALL(Request, A(pSvcName), wSvcCode, dRespExch, A(pRqHndlRet), dnpSend,
		    A(pData1), dcbData1, A(pData2), dcbData2, dData0, dData1, dData2);
}

erc_t SpawnTask(void (*pEntry)(void), U32 dPriority, U32 fDebug, U8 *pStack, U32 fOSCode)
{
	return CALL(SpawnTask, A(pEntry), dPriority, fDebug, A(pStack), fOSCode);
}

erc_t SetPriority(U32 p)		{ return CALL(SetPriority, p); }
erc_t Sleep(U32 n)			{ return CALL(Sleep, n); }
erc_t Alarm(U32 e, U32 n)		{ return CALL(Alarm, e, n); }
erc_t KillAlarm(U32 e)			{ return CALL(KillAlarm, e); }
erc_t GetTimerTick(U64 *p)		{ return CALL(GetTimerTick, A(p)); }

erc_t AllocPage(U32 n, U8 **pp)		{ return CALL(AllocPage, n, A(pp)); }
erc_t DeAllocPage(U8 *p, U32 n)		{ return CALL(DeAllocPage, A(p), n); }
erc_t QueryPages(U32 *p)		{ return CALL(QueryPages, A(p)); }
erc_t AliasMem(U8 *p, U64 cb, U32 j, U8 **pp)	{ return CALL(AliasMem, A(p), cb, j, A(pp)); }
erc_t DeAliasMem(U8 *p, U64 cb, U32 j)	{ return CALL(DeAliasMem, A(p), cb, j); }

erc_t LoadNewJob(const char *f, U32 cb, U32 *pj)	{ return CALL(LoadNewJob, A(f), cb, A(pj)); }
erc_t KillJob(U32 j)			{ return CALL(KillJob, j); }
erc_t GetJobNum(U32 *p)			{ return CALL(GetJobNum, A(p)); }
erc_t SetJobName(const char *n, U32 cb)	{ return CALL(SetJobName, A(n), cb); }
erc_t SetCmdLine(const char *c, U32 cb)	{ return CALL(SetCmdLine, A(c), cb); }
erc_t GetCmdLine(char *c, U32 *pcb)	{ return CALL(GetCmdLine, A(c), A(pcb)); }

void ExitJob(erc_t e)
{
	CALL(ExitJob, (U64)e);
	for (;;)
		;
}

erc_t TTYOut(const char *p, U64 n, U32 attr)	{ return CALL(TTYOut, A(p), n, attr); }

/* ---- MMURTL helpers in the library ---- */

void CopyData(const void *s, void *d, U64 n)	{ memcpy(d, s, n); }

void CopyDataR(const void *s, void *d, U64 n)	/* copies from the end (overlapping moves) */
{
	const U8 *ps = s;
	U8 *pd = d;

	while (n--)
		pd[n] = ps[n];
}

void FillData(void *d, U64 n, U8 b)		{ memset(d, b, n); }

U64 Compare(const void *a, const void *b, U64 n)
{
	const U8 *x = a, *y = b;
	U64 i;

	for (i = 0; i < n && x[i] == y[i]; i++)
		;
	return i;
}

static U8 lower(U8 c) { return c >= 'A' && c <= 'Z' ? (U8)(c + 32) : c; }

U64 CompareNCS(const void *a, const void *b, U64 n)
{
	const U8 *x = a, *y = b;
	U64 i;

	for (i = 0; i < n && lower(x[i]) == lower(y[i]); i++)
		;
	return i;
}

/* ---- C library (GCC may also call these itself) ---- */

void *memcpy(void *d, const void *s, size_t n)
{
	U8 *pd = d;
	const U8 *ps = s;

	while (n--)
		*pd++ = *ps++;
	return d;
}

void *memset(void *d, int c, size_t n)
{
	U8 *p = d;

	while (n--)
		*p++ = (U8)c;
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const U8 *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

/* printf: %c %s %d %u %x %p %%, l/ll, width and '0'. Output goes out with TTYOut. */
struct obuf {
	char b[128];
	U32 n;
	int total;
};

static void flush(struct obuf *o)
{
	if (o->n)
		TTYOut(o->b, o->n, 7);
	o->n = 0;
}

static void putc_(struct obuf *o, char c)
{
	if (o->n == sizeof(o->b))
		flush(o);
	o->b[o->n++] = c;
	o->total++;
}

static void putnum(struct obuf *o, U64 v, unsigned base, int width, char pad, bool neg)
{
	char t[24];
	int n = 0;

	do {
		t[n++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	if (neg)
		t[n++] = '-';
	while (width-- > n)
		putc_(o, pad);
	while (n)
		putc_(o, t[--n]);
}

int printf(const char *fmt, ...)
{
	struct obuf o = { .n = 0, .total = 0 };
	va_list ap;

	va_start(ap, fmt);
	for (; *fmt; fmt++) {
		int width = 0, lng = 0;
		char pad = ' ';

		if (*fmt != '%') {
			putc_(&o, *fmt);
			continue;
		}
		fmt++;
		if (*fmt == '0')
			pad = '0';
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l')
			lng++, fmt++;
		switch (*fmt) {
		case 'c':
			putc_(&o, (char)va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);

			while (s && *s)
				putc_(&o, *s++);
			break;
		}
		case 'd': {
			S64 v = lng ? va_arg(ap, S64) : va_arg(ap, int);

			putnum(&o, v < 0 ? (U64)(-(v + 1)) + 1 : (U64)v, 10, width, pad, v < 0);
			break;
		}
		case 'u':
			putnum(&o, lng ? va_arg(ap, U64) : va_arg(ap, unsigned), 10, width, pad, false);
			break;
		case 'x':
			putnum(&o, lng ? va_arg(ap, U64) : va_arg(ap, unsigned), 16, width, pad, false);
			break;
		case 'p':
			putc_(&o, '0');
			putc_(&o, 'x');
			putnum(&o, (U64)(uintptr_t)va_arg(ap, void *), 16, 0, ' ', false);
			break;
		case '%':
			putc_(&o, '%');
			break;
		default:
			putc_(&o, *fmt);
		}
	}
	va_end(ap);
	flush(&o);
	return o.total;
}

/* ---- file system (MFILES.H) ---- */

erc_t OpenFile(const char *n, U32 cb, U32 mode, U32 stream, U32 *ph)
					{ return CALL(OpenFile, A(n), cb, mode, stream, A(ph)); }
erc_t CloseFile(U32 h)			{ return CALL(CloseFile, h); }
erc_t CreateFile(const char *n, U32 cb, U32 attr)	{ return CALL(CreateFile, A(n), cb, attr); }
erc_t DeleteFile(U32 h)			{ return CALL(DeleteFile, h); }
erc_t RenameFile(const char *a, U32 ca, const char *b, U32 cb)
					{ return CALL(RenameFile, A(a), ca, A(b), cb); }
erc_t CreateDir(const char *n, U32 cb)	{ return CALL(CreateDir, A(n), cb, 0); }
erc_t DeleteDir(const char *n, U32 cb)	{ return CALL(DeleteDir, A(n), cb, 0); }
erc_t ReadBytes(U32 h, U8 *p, U64 n, U64 *pn)	{ return CALL(ReadBytes, h, A(p), n, A(pn)); }
erc_t WriteBytes(U32 h, const U8 *p, U64 n, U64 *pn)	{ return CALL(WriteBytes, h, A(p), n, A(pn)); }
erc_t ReadBlock(U32 h, U8 *p, U64 n, U64 lfa, U64 *pn)	{ return CALL(ReadBlock, h, A(p), n, lfa, A(pn)); }
erc_t WriteBlock(U32 h, const U8 *p, U64 n, U64 lfa, U64 *pn)
					{ return CALL(WriteBlock, h, A(p), n, lfa, A(pn)); }
erc_t GetFileSize(U32 h, U64 *p)	{ return CALL(GetFileSize, h, A(p)); }
erc_t SetFileSize(U32 h, U64 s)		{ return CALL(SetFileSize, h, s); }
erc_t GetFileLFA(U32 h, U64 *p)		{ return CALL(GetFileLFA, h, A(p)); }
erc_t SetFileLFA(U32 h, U64 lfa)	{ return CALL(SetFileLFA, h, lfa); }
erc_t GetDirSector(const char *p, U32 cb, U8 *s, U32 ss, U32 n)
					{ return CALL(GetDirSector, A(p), cb, A(s), ss, n); }
erc_t GetSystemDisk(U8 *p)		{ return CALL(GetSystemDisk, A(p)); }
erc_t ReadDir(const char *p, U32 cb, U32 i, struct dir_entry *e)
					{ return CALL(ReadDir, A(p), cb, i, A(e)); }
