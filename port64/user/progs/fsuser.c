/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* fsuser: file calls from ring 3. Reads C:\HELLO.TXT, writes C:\USER.TXT (checked on the host
   after the test), checks that bad buffers are refused, and exits with a file still open (the
   kernel must close it when the job ends). */

#include "doors.h"

static int fails;

static void check(const char *what, bool ok)
{
	if (!ok) {
		printf("fsuser: FAILED %s\n", what);
		fails++;
	}
}

int main(void)
{
	static const char msg[] = "written by a ring 3 program\n";
	char buf[64];
	U32 h, h2;
	U64 n, size;
	U8 sec[512], disk;

	check("OpenFile HELLO.TXT", OpenFile("C:\\HELLO.TXT", 12, ModeRead, 1, &h) == ErcOk);
	check("ReadBytes", ReadBytes(h, (U8 *)buf, sizeof(buf), &n) == ErcEOF && n == 25 &&
	      !memcmp(buf, "Hello from a FAT32 file.\n", 25));
	check("ReadBytes into kernel memory refused",
	      ReadBytes(h, (U8 *)0xFFFFFFFF80100000UL, 16, &n) == ErcBadLinAdd);
	check("ReadBytes into read-only memory refused",
	      ReadBytes(h, (U8 *)msg, 16, &n) != ErcOk);
	CloseFile(h);

	check("CreateFile USER.TXT", CreateFile("C:\\USER.TXT", 11, 0) == ErcOk);
	check("OpenFile USER.TXT", OpenFile("C:\\USER.TXT", 11, ModeModify, 1, &h) == ErcOk);
	check("WriteBytes", WriteBytes(h, (const U8 *)msg, sizeof(msg) - 1, &n) == ErcOk &&
	      n == sizeof(msg) - 1);
	check("GetFileSize", GetFileSize(h, &size) == ErcOk && size == sizeof(msg) - 1);
	CloseFile(h);

	check("GetDirSector", GetDirSector("C:\\", 3, sec, sizeof(sec), 0) == ErcOk);
	{
		struct dir_entry de;
		U32 i, seen = 0;

		for (i = 0; ReadDir("F:\\", 3, i, &de) == ErcOk; i++)
			seen |= Compare(de.name, "HELLO.TXT", 10) == 10 && de.size == 17 && !(de.attr & ATTR_DIR);
		check("ReadDir F:\\ (ext4)", seen && i > 5);
		check("ReadDir into kernel memory refused",
		      ReadDir("F:\\", 3, 0, (struct dir_entry *)0xFFFFFFFF80100000UL) == ErcBadLinAdd);
	}
	check("GetSystemDisk",GetSystemDisk(&disk) == ErcOk && disk == 2);
	check("bad handle", CloseFile(9999) == ErcBadFileHandle);

	/* leave one file open on purpose: ExitJob must close it */
	check("OpenFile again", OpenFile("C:\\USER.TXT", 11, ModeRead, 1, &h2) == ErcOk);

	if (!fails)
		printf("fsuser: all file calls from ring 3 ok\n");
	return fails;
}
