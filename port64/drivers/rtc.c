/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* CMOS real-time clock (MMURTL GetCMOSTime/GetCMOSDate). Used for FAT timestamps, which MMURTL
   left empty (FSYS.C GetFATTime). Reads until two passes agree, so an update in the middle can't
   give a torn value; handles BCD and binary formats. */

#include <doors/blk.h>
#include <doors/x86.h>

static U8 cmos(U8 reg)
{
	outb(0x70, reg);
	return inb(0x71);
}

static U8 bcd(U8 v)
{
	return (U8)((v >> 4) * 10 + (v & 15));
}

static void raw(U8 *r)
{
	while (cmos(0x0A) & 0x80)		/* update in progress */
		;
	r[0] = cmos(0x00);
	r[1] = cmos(0x02);
	r[2] = cmos(0x04);
	r[3] = cmos(0x07);
	r[4] = cmos(0x08);
	r[5] = cmos(0x09);
	r[6] = cmos(0x32);			/* century (if the firmware fills it) */
}

void rtc_read(struct rtc_time *t)
{
	U8 a[7], b[7], fmt;
	int i, same;

	do {
		raw(a);
		raw(b);
		for (same = 1, i = 0; i < 7; i++)
			same &= a[i] == b[i];
	} while (!same);

	fmt = cmos(0x0B);
	if (!(fmt & 4))				/* BCD */
		for (i = 0; i < 7; i++)
			a[i] = (i == 2) ? (U8)(bcd(a[i] & 0x7F) | (a[i] & 0x80)) : bcd(a[i]);
	if (!(fmt & 2) && (a[2] & 0x80))	/* 12-hour clock, PM */
		a[2] = (U8)(((a[2] & 0x7F) + 12) % 24);

	t->sec = a[0];
	t->min = a[1];
	t->hour = a[2];
	t->day = a[3];
	t->month = a[4];
	t->year = (U16)((a[6] >= 19 && a[6] <= 21 ? a[6] * 100 : 2000) + a[5]);
}
