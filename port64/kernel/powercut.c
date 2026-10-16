/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Power-cut test (tools/powercut.py, `make powercut`). Kernel command line:
     powercut          write files in F:\PC forever, "PC: op N done" after each operation; the test
                       kills QEMU at some point (a power cut)
     powercut stop=K   the same, but halt inside the K-th journal commit, right after the commit is
                       in the journal and before its blocks reach their homes
     pccheck           boot again on the same disk: mounting F: replays the journal; read every file
                       in F:\PC, then end QEMU through isa-debug-exit
   The operation sequence is mirrored in tools/powercut.py, which checks the disk with Linux tools:
   every operation that printed "done" must be there, and e2fsck must find nothing wrong. */

#include <doors/console.h>
#include <doors/fs.h>
#include <doors/kernel.h>
#include <doors/msg.h>
#include <doors/string.h>
#include <doors/task.h>

#define NSLOTS 16

static bool used[NSLOTS];
static U8 buf[4096];

static U32 plen(const char *p) { return (U32)strlen(p); }

static void slot_name(char *p, U32 s)
{
	memcpy(p, "F:\\PC\\F00.BIN", 14);
	p[7] = (char)('0' + s / 10);
	p[8] = (char)('0' + s % 10);
}

/* op i writes slot i % 16 with (i * 7919) % 50000 + 1 bytes, byte k = (k * 31 + i) & 0xFF;
   every 5th op deletes the slot instead (if it exists); every 7th renames slot i % 16 to slot
   (i * 3) % 16 (if the source exists and the target doesn't). */
static erc_t op(U32 i)
{
	char a[16], b[16];
	U32 s = i % NSLOTS, t = (i * 3) % NSLOTS, h;
	U64 len, off, n, k, done;
	erc_t erc;

	slot_name(a, s);
	if (i % 5 == 0) {
		if (!used[s])
			return ErcOk;
		if ((erc = OpenFile(a, plen(a), ModeModify, 1, &h)) || (erc = DeleteFile(h)))
			return erc;
		used[s] = false;
		return ErcOk;
	}
	if (i % 7 == 0) {
		slot_name(b, t);
		if (!used[s] || used[t] || s == t)
			return ErcOk;
		if ((erc = RenameFile(a, plen(a), b, plen(b))))
			return erc;
		used[s] = false;
		used[t] = true;
		return ErcOk;
	}
	len = (U64)(i * 7919) % 50000 + 1;
	erc = CreateFile(a, plen(a), 0);
	if (erc && erc != ErcDupName)
		return erc;
	if ((erc = OpenFile(a, plen(a), ModeModify, 1, &h)))
		return erc;
	erc = SetFileSize(h, 0);
	for (off = 0; !erc && off < len; off += n) {
		n = len - off < sizeof(buf) ? len - off : sizeof(buf);
		for (k = 0; k < n; k++)
			buf[k] = (U8)((off + k) * 31 + i);
		erc = WriteBytes(h, buf, n, &done);
	}
	CloseFile(h);
	used[s] = true;
	return erc;
}

static U32 cmdline_num(const char *cmd, const char *key)
{
	U32 n = 0, k = (U32)strlen(key);

	for (; *cmd; cmd++)
		if (!memcmp(cmd, key, k)) {
			for (cmd += k; *cmd >= '0' && *cmd <= '9'; cmd++)
				n = n * 10 + (U32)(*cmd - '0');
			return n;
		}
	return 0;
}

NORETURN void powercut_run(const char *cmdline)
{
	U32 i;
	erc_t erc;

	ext4_commit_stop = cmdline_num(cmdline, "stop=");
	erc = CreateDir("F:\\PC", 5);
	kprintf("PC: writing (CreateDir: %u, stop at commit %u)\n", erc, ext4_commit_stop);
	for (i = 1;; i++) {
		if ((erc = op(i))) {
			kprintf("PC: op %u failed: error %u\n", i, erc);
			for (;;)
				Sleep(100);
		}
		kprintf("PC: op %u done\n", i);
	}
}

NORETURN void powercut_check(void)
{
	struct dir_entry de;
	U32 i, n = 0, h;
	U64 got, total;
	erc_t erc = ErcOk;

	for (i = 0; ReadDir("F:\\PC", 5, i, &de) == ErcOk; i++) {
		char name[300];
		U32 l = (U32)strlen(de.name);

		memcpy(name, "F:\\PC\\", 6);
		memcpy(name + 6, de.name, l + 1);
		if ((erc = OpenFile(name, l + 6, ModeRead, 1, &h)))
			break;
		total = 0;
		do {
			erc = ReadBytes(h, buf, sizeof(buf), &got);
			total += got;
		} while (erc == ErcOk && got);
		CloseFile(h);
		if (erc != ErcOk && erc != ErcEOF)
			break;
		if (total != de.size) {
			kprintf("PC: %s: read %lu of %lu bytes\n", de.name, total, de.size);
			erc = ErcBrokenFile;
			break;
		}
		erc = ErcOk;
		n++;
	}
	if (erc)
		kprintf("PC: check failed: error %u\n", erc);
	else
		kprintf("PC: check ok (%u files read)\n", n);
	ext4_unmount();				/* clean: needs_recovery cleared */
	qemu_exit(erc ? QEMU_EXIT_FAIL : QEMU_EXIT_PASS);
	for (;;)
		;
}
