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

/* Days since 1970-01-01 for a civil date (Howard Hinnant's days_from_civil). */
U32 days_from_civil(U32 y, U32 m, U32 d)
{
	U32 era, yoe, doy, doe;

	y -= m <= 2;
	era = y / 400;
	yoe = y - era * 400;
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

U32 unix_time(void)
{
	struct rtc_time t;

	rtc_read(&t);
	return days_from_civil(t.year, t.month, t.day) * 86400U + t.hour * 3600U + t.min * 60U + t.sec;
}

static U32 to_bcd(U32 v)
{
	return (v / 10) << 4 | (v % 10);
}

/* MMURTL GetCMOSTime: seconds | minutes << 8 | hours << 16, each in BCD. */
erc_t GetCMOSTime(U32 *pdTimeRet)
{
	struct rtc_time t;

	rtc_read(&t);
	*pdTimeRet = to_bcd(t.sec) | to_bcd(t.min) << 8 | to_bcd(t.hour) << 16;
	return ErcOk;
}

/* MMURTL GetCMOSDate: day of week (0 = Sunday) | day << 8 | month << 16 | year (0-99) << 24, BCD. */
erc_t GetCMOSDate(U32 *pdDateRet)
{
	struct rtc_time t;
	U32 dow;

	rtc_read(&t);
	dow = (days_from_civil(t.year, t.month, t.day) + 4) % 7;	/* 1970-01-01 was a Thursday */
	*pdDateRet = to_bcd(dow) | to_bcd(t.day) << 8 | to_bcd(t.month) << 16 | to_bcd(t.year % 100) << 24;
	return ErcOk;
}
