/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* jobtest: the M9b job calls and C library from ring 3. It runs three times in the same job,
   told apart by its command line (which a chained or reloaded program inherits):
     ""        the JCB strings, CMOS clock, libc; then SetExitJob("jobtest") and Chain("jobtest")
     "chained" Chain kept the job number, path and command line; open files were closed; ExitJob
     "exited"  ExitJob loaded the exit run file into the same job; Chain to a missing file fails */
#include "doors.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void check(const char *what, int ok)
{
	if (!ok) {
		printf("jobtest: FAILED %s\n", what);
		fails++;
	}
}

static int is_bcd(U32 v, int bytes)
{
	int i;

	for (i = 0; i < bytes * 2; i++, v >>= 4)
		if ((v & 0xF) > 9)
			return 0;
	return 1;
}

static void stage_one(U32 job)
{
	char s[100], line[40];
	U32 n, t, d;
	FILE *f;
	long pos;

	/* JCB strings */
	check("SetPath/GetPath", SetPath("C:\\DIR1\\", 8) == ErcOk && GetPath(job, s, &n) == ErcOk &&
	      n == 8 && !memcmp(s, "C:\\DIR1\\", 8));
	memset(s, 'x', sizeof(s));
	check("SetPath too long", SetPath(s, 70) == ErcBadJobParam);
	check("GetPath of job 99", GetPath(99, s, &n) == ErcBadJobNum);
	check("SetUserName", SetUserName("joel", 4) == ErcOk && GetUserName(s, &n) == ErcOk && n == 4 &&
	      !memcmp(s, "joel", 4));
	check("SysIn/SysOut", SetSysIn("KBD", 3) == ErcOk && SetSysOut("VID", 3) == ErcOk &&
	      GetSysIn(s, &n) == ErcOk && n == 3 && GetSysOut(s + 10, &n) == ErcOk &&
	      !memcmp(s, "KBD", 3) && !memcmp(s + 10, "VID", 3));
	check("no exit job yet", GetExitJob(s, &n) == ErcOk && n == 0);

	/* CMOS clock: BCD, plausible */
	check("GetCMOSTime", GetCMOSTime(&t) == ErcOk && is_bcd(t, 3) && (t >> 16) <= 0x23 &&
	      (t & 0xFF) <= 0x59);
	check("GetCMOSDate", GetCMOSDate(&d) == ErcOk && is_bcd(d, 4) && (d & 0xFF) <= 6 &&
	      ((d >> 16) & 0xFF) >= 1 && ((d >> 16) & 0xFF) <= 0x12 && (d >> 24) >= 0x24);

	/* formatting and strings */
	sprintf(s, "%05d|%-4s|%4s|%x|%X|%ld|%u|%c|%%|%.3s", -42, "ab", "cd", 255, 255, -7L, 3000000000u,
		'Z', "abcdef");
	check("sprintf", !strcmp(s, "-0042|ab  |  cd|ff|FF|-7|3000000000|Z|%|abc"));
	check("snprintf", snprintf(line, 5, "%s", "truncate me") == 11 && !strcmp(line, "trun"));
	strcpy(s, "C:\\");
	strcat(s, "TEST");
	strncat(s, ".TXTXX", 4);
	check("strcpy/strcat", !strcmp(s, "C:\\TEST.TXT") && strlen(s) == 11);
	check("strchr/strrchr/strstr", strchr(s, '\\') == s + 2 && strrchr(s, 'T') == s + 10 &&
	      strstr(s, "EST") == s + 4 && !strstr(s, "xyz"));
	check("strncmp/strncpy", strncmp("abcd", "abcz", 3) == 0 && strncmp("abcd", "abcz", 4) < 0 &&
	      !strcmp(strncpy(line, "hi", 5), "hi") && !line[4]);
	check("atoi/strtol", atoi(" -123") == -123 && strtol("0x1F", NULL, 0) == 31 &&
	      strtoul("777", NULL, 8) == 511);
	check("ctype", isspace('\t') && !isspace('a') && isdigit('7') && toupper('q') == 'Q' &&
	      tolower('Q') == 'q' && isalnum('z') && ispunct('!') && !isalpha('1'));
	check("CompareNCS", CompareNCS("HeLLo", "hello", 5) == -1 && CompareNCS("help", "hello", 4) == 3 &&
	      Compare("abc", "abd", 3) == 2);

	/* stdio on files */
	f = fopen("C:\\STDIO.TXT", "w");
	check("fopen w", f != NULL);
	if (f) {
		fprintf(f, "line %d\n", 1);
		fputs("line two\n", f);
		fputc('3', f);
		fclose(f);
	}
	f = fopen("C:\\STDIO.TXT", "a");
	check("fopen a", f && fputs("\nappended\n", f) == 0 && fclose(f) == 0);
	f = fopen("C:\\STDIO.TXT", "r");
	check("fopen r", f != NULL);
	if (f) {
		check("fgets 1", fgets(line, sizeof(line), f) && !strcmp(line, "line 1\n"));
		pos = ftell(f);
		check("fgets 2", fgets(line, sizeof(line), f) && !strcmp(line, "line two\n"));
		check("fgets short buffer", fgets(line, 2, f) && !strcmp(line, "3"));
		check("fgetc", fgetc(f) == '\n');
		check("fgets 4", fgets(line, sizeof(line), f) && !strcmp(line, "appended\n"));
		check("EOF", fgets(line, sizeof(line), f) == NULL && fgetc(f) == EOF);
		check("fseek/ftell", pos == 7 && fseek(f, pos, SEEK_SET) == 0 &&
		      fgets(line, sizeof(line), f) && !strcmp(line, "line two\n"));
		fclose(f);
	}
	check("fopen of a missing file", fopen("C:\\NOSUCH.TXT", "r") == NULL);
	check("rename", rename("C:\\STDIO.TXT", "C:\\STDIO2.TXT") == 0 && !fopen("C:\\STDIO.TXT", "r"));
	check("remove", remove("C:\\STDIO2.TXT") == 0 && remove("C:\\STDIO2.TXT") == -1);
	check("sprintf to a buffer", sprintf(s, "%s:%u", "job", job) > 4);
}

int main(void)
{
	char cmd[80], s[80];
	U32 n, job, h;

	GetJobNum(&job);
	GetCmdLine(cmd, &n);
	cmd[n] = 0;
	if (!strncmp(cmd, "chained ", 8)) {
		check("Chain kept the job", (U32)atoi(cmd + 8) == job);
		check("Chain kept the path", GetPath(job, s, &n) == ErcOk && n == 8 &&
		      !memcmp(s, "C:\\DIR1\\", 8));
		check("Chain kept the exit job", GetExitJob(s, &n) == ErcOk && n == 7);
		check("Chain closed the files", OpenFile("C:\\HELLO.TXT", 12, ModeModify, 1, &h) == ErcOk);
		sprintf(s, "exited %u", job);
		SetCmdLine(s, (U32)strlen(s));
		if (fails)
			SetExitJob("", 0);		/* end here with the failure count */
		ExitJob(fails);
	}
	if (!strncmp(cmd, "exited ", 7)) {
		check("ExitJob reloaded into the same job", (U32)atoi(cmd + 7) == job);
		SetExitJob("", 0);
		check("Chain to a missing program", Chain("nosuch", 6, 0) == ErcNoSuchFile);
		if (!fails)
			printf("jobtest: job calls, Chain, exit job and C library ok\n");
		return fails;
	}
	stage_one(job);
	OpenFile("C:\\HELLO.TXT", 12, ModeModify, 1, &h);	/* left open: Chain must close it */
	sprintf(s, "chained %u", job);
	SetCmdLine(s, (U32)strlen(s));
	check("SetExitJob", SetExitJob("jobtest", 7) == ErcOk);
	if (fails)
		return fails;
	Chain("jobtest", 7, 0);
	printf("jobtest: FAILED Chain returned\n");
	return 1;
}
