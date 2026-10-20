/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* churn: the main load of the soak test (ROADMAP 2.3, kernel/soak.c). It works until it is
   killed: files in its own directories on C: (FAT) and F: (ext4), written and read back and
   compared, renamed, cut and deleted; pages allocated, filled, checked and freed; messages to a
   second task of its own, which allocates and frees pages too. The command line is a seed.
   Data that doesn't read back as written prints "MISMATCH", an error that shouldn't happen prints
   "CHURN ERROR": tools/soak.py fails on both. Being killed half-way is normal: the next churn in
   the same job number starts by clearing its directories. */

#include "doors.h"
#include <stdio.h>
#include <string.h>

#define SLOTS 6

static U64 rng = 0x9E3779B97F4A7C15ULL;
static U8 buf[12288], back[12288];
static char dir[2][24];
static U32 dlen[2], job, wexch;
static U8 wstack[8192] __attribute__((aligned(16)));
static struct { int exists; U32 len, seed; } slot[2][SLOTS];

static U32 rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (U32)(rng >> 16);
}

static void fill(U8 *p, U32 n, U32 seed)
{
	U32 i;

	for (i = 0; i < n; i++)
		p[i] = (U8)(seed * 31 + i * 7 + (i >> 8));
}

static void bad(const char *what, U32 erc)
{
	printf("churn %u: CHURN ERROR %s: error %u\n", job, what, erc);
	ExitJob(1);
}

static U32 name(char *p, int v, int s)
{
	memcpy(p, dir[v], dlen[v]);
	memcpy(p + dlen[v], "\\F0.BIN", 8);
	p[dlen[v] + 2] = (char)('0' + s);
	return dlen[v] + 7;
}

static void verify(int v, int s)
{
	char p[40];
	U32 h, pl = name(p, v, s), erc;
	U64 got = 0;

	if ((erc = OpenFile(p, pl, ModeRead, 1, &h)))
		bad("open to read", erc);
	erc = ReadBytes(h, back, sizeof(back), &got);
	CloseFile(h);
	if ((erc && erc != ErcEOF) || got != slot[v][s].len)
		bad("read back (length)", erc ? erc : (U32)got);
	fill(buf, slot[v][s].len, slot[v][s].seed);
	if (memcmp(buf, back, slot[v][s].len)) {
		printf("churn %u: MISMATCH in %s (%u bytes, seed %u)\n", job, p, slot[v][s].len, slot[v][s].seed);
		ExitJob(2);
	}
}

static void op_write(int v, int s)
{
	char p[40];
	U32 h, pl = name(p, v, s), erc, n = rnd() % sizeof(buf), seed = rnd();
	U64 done;

	erc = CreateFile(p, pl, 0);
	if (erc && erc != ErcDupName)
		bad("create", erc);
	if ((erc = OpenFile(p, pl, ModeModify, 1, &h)))
		bad("open to write", erc);
	fill(buf, n, seed);
	if ((erc = SetFileSize(h, 0)) || (erc = WriteBytes(h, buf, n, &done)) || done != n)
		bad("write", erc ? erc : (U32)done);
	CloseFile(h);
	slot[v][s].exists = 1;
	slot[v][s].len = n;
	slot[v][s].seed = seed;
	verify(v, s);
}

static void op_delete(int v, int s)
{
	char p[40];
	U32 h, pl = name(p, v, s), erc;

	if ((erc = OpenFile(p, pl, ModeModify, 1, &h)) || (erc = DeleteFile(h)))
		bad("delete", erc);
	slot[v][s].exists = 0;
}

static void op_rename(int v, int s, int t)
{
	char a[40], b[40];
	U32 al = name(a, v, s), bl = name(b, v, t), erc;

	if ((erc = RenameFile(a, al, b, bl)))
		bad("rename", erc);
	slot[v][t] = slot[v][s];
	slot[v][s].exists = 0;
	verify(v, t);
}

static void op_pages(void)
{
	U32 n = 1 + rnd() % 8, i, erc;
	U8 *p;

	if ((erc = AllocPage(n, &p)))
		bad("AllocPage", erc);
	for (i = 0; i < n * 4096; i += 512)
		p[i] = (U8)(i >> 9);
	for (i = 0; i < n * 4096; i += 512)
		if (p[i] != (U8)(i >> 9)) {
			printf("churn %u: MISMATCH in allocated memory\n", job);
			ExitJob(2);
		}
	if ((erc = DeAllocPage(p, n)))
		bad("DeAllocPage", erc);
}

/* the second task: a page for every message it gets */
static void worker(void)
{
	U64 m[2];
	U8 *p;

	for (;;) {
		if (WaitMsg(wexch, m) == ErcOk && !AllocPage(1, &p)) {
			p[100] = (U8)m[0];
			DeAllocPage(p, 1);
		}
	}
}

/* a fresh start: the directories empty (an earlier churn in this job may have been killed) */
static void clear(int v)
{
	char p[40];
	U32 h, s;

	CreateDir(dir[v], dlen[v]);
	for (s = 0; s < SLOTS; s++)
		if (OpenFile(p, name(p, v, s), ModeModify, 1, &h) == ErcOk)
			DeleteFile(h);
}

int main(void)
{
	char cmd[80];
	U32 n = 0, i, ops = 0, erc;

	GetJobNum(&job);
	if (!GetCmdLine(cmd, &n))
		for (i = 0; i < n; i++)
			rng = rng * 31 + (U8)cmd[i];
	rng ^= job;
	for (i = 0; i < 2; i++) {
		CreateDir(i ? "F:\\SOAK" : "C:\\SOAK", 7);
		dlen[i] = (U32)snprintf(dir[i], sizeof(dir[i]), "%c:\\SOAK\\J%02u", i ? 'F' : 'C', job);
		clear((int)i);
	}
	if ((erc = AllocExch(&wexch)) || (erc = SpawnTask(worker, 25, 0, wstack + sizeof(wstack), 0)))
		bad("worker", erc);
	for (;;) {
		int v = (int)(rnd() % 2), s = (int)(rnd() % SLOTS), t = (int)(rnd() % SLOTS);
		U32 what = rnd() % 10;

		if (what < 4 || !slot[v][s].exists)
			op_write(v, s);
		else if (what < 6)
			verify(v, s);
		else if (what == 6)
			op_delete(v, s);
		else if (what == 7 && s != t && !slot[v][t].exists)
			op_rename(v, s, t);
		else if (what == 8)
			op_pages();
		else
			SendMsg(wexch, ops, job);
		if (++ops % 200 == 0)
			printf("churn %u: %u operations\n", job, ops);
	}
}
