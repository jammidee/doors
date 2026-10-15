/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Volumes: find partitions on every disk (MBR, or GPT behind a protective MBR, or a disk with
   no partition table that starts with a FAT boot sector) and give them drive letters from C:,
   in disk order (MMURTL FSYS.C read_PE handled only the 4 MBR entries of 2 disks).
   FAT volumes are mounted; Linux partitions are recorded for ext4 (M8). */

#include <doors/blk.h>
#include <doors/console.h>
#include <doors/fs.h>
#include <doors/string.h>

static struct volume vols[MAX_VOLS];
static U32 nvols;

struct volume *vol_get(char letter)
{
	U32 i;

	if (letter >= 'a' && letter <= 'z')
		letter = (char)(letter - 32);
	for (i = 0; i < nvols; i++)
		if (vols[i].letter == letter)
			return &vols[i];
	return NULL;
}

struct volume *vol_index(U32 i)
{
	return i < nvols ? &vols[i] : NULL;
}

static void add(struct blkdev *d, U64 start, U64 n, U8 part, bool linux_hint)
{
	struct volume *v;

	if (nvols == MAX_VOLS || !n || start + n > d->nblocks)
		return;
	v = &vols[nvols];
	memset(v, 0, sizeof(*v));
	v->dev = d;
	v->start = start;
	v->nsectors = n;
	v->part = part;
	v->letter = (char)('C' + nvols);
	if (fat_mount(v) != ErcOk)
		v->kind = linux_hint ? VOL_LINUX : VOL_NONE;
	nvols++;
	kprintf("Volume %c: disk %u part %u, %lu MB, %s%s%s\n", v->letter, d->devnum, part, n >> 11,
		v->kind == VOL_FAT12 ? "FAT12" : v->kind == VOL_FAT16 ? "FAT16" :
		v->kind == VOL_FAT32 ? "FAT32" : v->kind == VOL_LINUX ? "Linux (not mounted yet)" :
		"unknown", v->label[0] ? " " : "", v->label);
}

struct mbr_part {
	U8 boot, chs1[3], type, chs2[3];
	U32 lba, count;
} PACKED;

struct gpt_hdr {
	char sig[8];
	U32 rev, hsize, hcrc, rsvd;
	U64 my_lba, alt_lba, first, last;
	U8 guid[16];
	U64 entries_lba;
	U32 nentries, esize, ecrc;
} PACKED;

struct gpt_entry {
	U8 type[16], uniq[16];
	U64 first, last, attrs;
	U16 name[36];
} PACKED;

static const U8 GUID_LINUX[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
				   0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };

static void scan_gpt(struct blkdev *d)
{
	U8 sec[512];
	struct gpt_hdr h;
	U32 i, per = 512 / 128;

	if (bc_read(d, 1, sec))
		return;
	memcpy(&h, sec, sizeof(h));
	if (memcmp(h.sig, "EFI PART", 8) || h.esize != 128)
		return;
	for (i = 0; i < h.nentries && i < 128; i++) {
		struct gpt_entry e;
		static const U8 zero[16];

		if (i % per == 0 && bc_read(d, h.entries_lba + i / per, sec))
			return;
		memcpy(&e, sec + (i % per) * 128, sizeof(e));
		if (!memcmp(e.type, zero, 16) || e.last < e.first)
			continue;
		add(d, e.first, e.last - e.first + 1, (U8)(i + 1), !memcmp(e.type, GUID_LINUX, 16));
	}
}

static void scan(struct blkdev *d)
{
	U8 sec[512];
	struct mbr_part p[4];
	U32 i;
	bool any = false;

	if (bc_read(d, 0, sec))
		return;
	if (sec[510] != 0x55 || sec[511] != 0xAA)
		return;
	if (fat_bpb_ok(sec)) {			/* no partition table, a FAT boot sector at 0 */
		add(d, 0, d->nblocks, 0, false);
		return;
	}
	memcpy(p, sec + 446, sizeof(p));
	for (i = 0; i < 4; i++) {
		if (p[i].type == 0xEE) {		/* protective MBR: GPT disk */
			scan_gpt(d);
			return;
		}
	}
	for (i = 0; i < 4; i++) {
		if (!p[i].type || !p[i].count)
			continue;
		any = true;
		add(d, p[i].lba, p[i].count, (U8)(i + 1), p[i].type == 0x83);
	}
	(void)any;
}

void vol_init(void)
{
	U32 n;

	for (n = DEV_HD0; n < DEV_HD0 + 4; n++)
		if (blk_get(n))
			scan(blk_get(n));
}
