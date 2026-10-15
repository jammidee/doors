/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* vidtest: the MMURTL video and keyboard calls from ring 3 (M9a self-test). Draws on its own
   virtual screen and reads it back, takes the screen with SetVidOwner, then reads keys that the
   test runner types (after "M9: type kbd" and "M9: type edit") with ReadKbd and EditLine. */
#include "doors.h"

static int fails;

static void check(const char *what, int ok)
{
	if (!ok) {
		printf("vidtest: FAILED %s\n", what);
		fails++;
	}
}

static int cell(U32 x, U32 y, U8 ch, U8 attr)
{
	U8 c, a;

	return GetVidChar(x, y, &c, &a) == ErcOk && c == ch && a == attr;
}

int main(void)
{
	U32 job, x, y, own, key, n, norm;
	U8 line[20], ex;

	GetJobNum(&job);
	check("ClrScr", ClrScr() == ErcOk && GetXY(&x, &y) == ErcOk && x == 0 && y == 0);
	check("TTYOut", TTYOut("ab\r\ncd", 6, 0x1E) == ErcOk && GetXY(&x, &y) == ErcOk && x == 2 &&
	      y == 1 && cell(0, 0, 'a', 0x1E) && cell(1, 1, 'd', 0x1E));
	check("TTYOut backspace", TTYOut("\b", 1, 7) == ErcOk && GetXY(&x, &y) == ErcOk && x == 1 &&
	      cell(1, 1, 'd', 0x1E));
	check("PutVidChars", PutVidChars(70, 24, "0123456789", 10, 0x2F) == ErcOk &&
	      cell(70, 24, '0', 0x2F) && cell(79, 24, '9', 0x2F));
	check("PutVidChars past the end", PutVidChars(70, 24, "0123456789X", 11, 7) == ErcVidParam);
	check("PutVidAttrs", PutVidAttrs(0, 0, 2, 0x4F) == ErcOk && cell(1, 0, 'b', 0x4F));
	check("ScrollVid up", ScrollVid(0, 0, 80, 2, 1) == ErcOk && cell(0, 0, 'c', 0x1E) &&
	      cell(0, 1, ' ', 7));
	check("ScrollVid down", ScrollVid(0, 0, 80, 2, 0) == ErcOk && cell(0, 1, 'c', 0x1E) &&
	      cell(0, 0, ' ', 7));
	check("bad coordinates", SetXY(80, 0) == ErcVidParam && GetVidChar(0, 25, line, &ex) == ErcVidParam);
	check("SetNormVid/ClrScr", SetNormVid(0x17) == ErcOk && GetNormVid(&norm) == ErcOk &&
	      norm == 0x17 && ClrScr() == ErcOk && cell(5, 5, ' ', 0x17));
	check("TTY scrolls at the bottom", SetXY(0, 24) == ErcOk && TTYOut("x\n", 2, 7) == ErcOk &&
	      GetXY(&x, &y) == ErcOk && y == 24 && cell(0, 23, 'x', 7) && cell(0, 24, ' ', 0x17));

	check("not on screen yet", GetVidOwner(&own) == ErcOk && own == 1);
	check("SetVidOwner", SetVidOwner(job) == ErcOk && GetVidOwner(&own) == ErcOk && own == job);
	check("SetVidOwner(2)", SetVidOwner(2) == ErcVidNum);
	check("no key waiting", ReadKbd(&key, 0) == ErcNoKeyAvail);

	printf("M9: type kbd\n");
	check("key k", ReadKbd(&key, 1) == ErcOk && (key & 0xFF) == 'k' && !(key & KBD_SHIFT));
	check("key Q", ReadKbd(&key, 1) == ErcOk && (key & 0xFF) == 'Q' && (key & KBD_SHIFT));
	check("key F1", ReadKbd(&key, 1) == ErcOk && (key & 0xFF) == 0x0F);
	check("key Up", ReadKbd(&key, 1) == ErcOk && (key & 0xFF) == 0x01 && !(key & KBD_NUMPAD));
	check("key Enter", ReadKbd(&key, 1) == ErcOk && (key & 0xFF) == 0x0D);

	SetXY(0, 10);
	printf("M9: type edit\n");
	SetXY(0, 12);
	check("EditLine", EditLine(line, 0, 20, &n, &ex, 0x70) == ErcOk && n == 5 && ex == 0x0D &&
	      Compare(line, "hello", 5) == -1 && cell(0, 12, 'h', 0x17) && cell(5, 12, ' ', 0x17));
	check("EditLine bad length", EditLine(line, 0, 81, &n, &ex, 7) == ErcEditParam);

	if (!fails)
		printf("vidtest: all video and keyboard calls ok\n");
	return fails;
}
