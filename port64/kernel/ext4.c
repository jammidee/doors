/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* ext4: DOORS's native file system (docs/PORT64_PLAN.md: "a modern native file system").

   Reads what mkfs.ext4 (e2fsprogs 1.46) makes by default: 64-bit block numbers, flex_bg, extents
   (any depth), filetype, dir_index directories (read linearly), huge_file, dir_nlink, extra_isize,
   metadata_csum (CRC32C on superblock, group descriptors, bitmaps, inodes, extent blocks and
   directory blocks). Writes keep every checksum and count right, so `e2fsck -fn` stays clean
   (tools/check_disk.py runs it after every `make test`).

   Writing, in this version:
   - with the journal (JBD2, see "journal" below): every modifying operation is one transaction,
     metadata goes through the journal, file data straight to disk first (ordered mode). A
     journal that needs recovery is replayed at mount (also one Linux left). A power cut at any
     point leaves a volume that the next mount (or e2fsck) brings back to a consistent state with
     every finished operation in it (`make powercut`). Journals with checksum features are
     replayed but the volume is then mounted read-only; volumes without a journal are written
     directly, as before.
   - extent trees up to depth 1 (4 extents in the inode, then up to 4 leaf blocks of 340 each);
     a deeper tree (written by Linux) is read fine but not modified (ErcNotSupported).
   - directories with an htree index (Linux makes them for big directories) are read fine but not
     modified (ErcNotSupported); small directories are plain lists and are written normally.
   - allocation only in groups whose bitmaps are initialised (not BLOCK_/INODE_UNINIT).
   Names are case-sensitive bytes (UTF-8), as on Linux. Called only by the FILESYS service task,
   so the static scratch buffers below need no locking. */

#include <doors/blk.h>
#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/fs.h>
#include <doors/mem.h>
#include <doors/string.h>
#include <doors/task.h>
#include <doors/x86.h>

#define EXT4_MAGIC 0xEF53
#define EXT_MAGIC  0xF30A
#define ROOT_INO   2
#define MAXBS      4096

/* feature bits */
#define INCOMPAT_FILETYPE  0x0002
#define INCOMPAT_RECOVER   0x0004
#define INCOMPAT_EXTENTS   0x0040
#define INCOMPAT_64BIT     0x0080
#define INCOMPAT_FLEX_BG   0x0200
#define INCOMPAT_CSUM_SEED 0x2000
#define INCOMPAT_LARGEDIR  0x4000
#define INCOMPAT_OK  (INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_EXTENTS | INCOMPAT_64BIT | \
		      INCOMPAT_FLEX_BG | INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR)
#define RO_SPARSE_SUPER 0x0001
#define RO_LARGE_FILE   0x0002
#define RO_HUGE_FILE    0x0008
#define RO_GDT_CSUM     0x0010
#define RO_DIR_NLINK    0x0020
#define RO_EXTRA_ISIZE  0x0040
#define RO_METADATA_CSUM 0x0400
#define RO_RW_OK (RO_SPARSE_SUPER | RO_LARGE_FILE | RO_HUGE_FILE | RO_DIR_NLINK | RO_EXTRA_ISIZE | \
		  RO_METADATA_CSUM)

/* inode */
#define S_IFMT   0xF000
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define FL_INDEX   0x00001000
#define FL_HUGE    0x00040000
#define FL_EXTENTS 0x00080000
#define FL_INLINE  0x10000000

/* group descriptor flags */
#define BG_INODE_UNINIT 0x0001
#define BG_BLOCK_UNINIT 0x0002

#define FT_REG 1
#define FT_DIR 2

struct ext4 {
	struct volume *vol;
	struct blkdev *dev;
	U64 base;			/* partition start, sectors */
	U32 bs, spb;			/* block size, sectors per block */
	U64 nblocks;
	U32 ninodes, bpg, ipg, first_data, isize, dsize, ngroups, first_ino;
	U32 incompat, ro_compat;
	bool csum;
	U32 seed;
	U64 gdt;			/* first block of the group descriptor table */
	U8 sb[1024];
	/* journal (JBD2, inode s_journal_inum) */
	bool journal;			/* writes go through the journal */
	bool journal_clean;		/* ext4_unmount() cleared needs_recovery */
	U8 jraw[512];			/* the journal inode */
	U32 jlen, jfirst, jseq;		/* s_maxlen, s_first, next transaction ID */
	U32 jincompat, jtag;		/* journal incompat features, descriptor tag size */
	U32 txmax;			/* blocks per transaction */
	U8 juuid[16];
};

static struct ext4 exts[MAX_VOLS];
static U32 next_ext;

/* scratch blocks (one FILESYS task, never re-entered) */
static U8 b_gd[MAXBS], b_bm[MAXBS], b_leaf[MAXBS], b_leaf2[MAXBS], b_dir[MAXBS], b_data[MAXBS];

static U16 le16(const U8 *p) { return (U16)(p[0] | (p[1] << 8)); }
static U32 le32(const U8 *p) { return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24); }
static void put16(U8 *p, U32 v) { p[0] = (U8)v; p[1] = (U8)(v >> 8); }
static void put32(U8 *p, U32 v) { p[0] = (U8)v; p[1] = (U8)(v >> 8); p[2] = (U8)(v >> 16); p[3] = (U8)(v >> 24); }

/* ---- blocks and the running transaction ----

   With a journal, each modifying file operation is one transaction (see "journal" below). Its
   metadata blocks (superblock, group descriptors, bitmaps, inodes, extent and directory blocks) are
   collected here with mwr()/secwr() instead of going to disk, and brd()/secrd() see them. File data
   is written straight to disk with bwr() before the commit (ordered mode). Without a journal,
   mwr() and secwr() write through at once. */

#define TXMAX 256			/* blocks in one transaction (1 MB of images) */

static struct {
	struct ext4 *e;			/* volume of the open transaction, NULL = none */
	U32 n;
	U64 blk[TXMAX];
	U8 *img;			/* TXMAX * MAXBS bytes (AllocOSPage, first journaled mount) */
} tx;

static erc_t tx_commit(void);

static S32 tx_slot(struct ext4 *e, U64 b)
{
	U32 i;

	if (tx.e != e)
		return -1;
	for (i = 0; i < tx.n; i++)
		if (tx.blk[i] == b)
			return (S32)i;
	return -1;
}

/* raw block I/O through the write-through cache (no transaction) */
static erc_t disk_rd(struct ext4 *e, U64 b, U8 *buf)
{
	U32 i;
	erc_t erc;

	for (i = 0; i < e->spb; i++)
		if ((erc = bc_read(e->dev, e->base + b * e->spb + i, buf + i * 512)))
			return erc;
	return ErcOk;
}

static erc_t bwr(struct ext4 *e, U64 b, const U8 *buf)
{
	U32 i;
	erc_t erc;

	for (i = 0; i < e->spb; i++)
		if ((erc = bc_write(e->dev, e->base + b * e->spb + i, buf + i * 512)))
			return erc;
	return ErcOk;
}

static erc_t brd(struct ext4 *e, U64 b, U8 *buf)
{
	S32 s = tx_slot(e, b);

	if (s >= 0) {
		memcpy(buf, tx.img + (U64)s * MAXBS, e->bs);
		return ErcOk;
	}
	return disk_rd(e, b, buf);
}

/* A slot for block b in the open transaction, filled with the block's current contents. */
static erc_t tx_take(struct ext4 *e, U64 b, S32 *slot)
{
	erc_t erc;

	if ((*slot = tx_slot(e, b)) >= 0)
		return ErcOk;
	if (tx.n == e->txmax && (erc = tx_commit()))	/* full: commit what we have, go on */
		return erc;
	if ((erc = disk_rd(e, b, tx.img + (U64)tx.n * MAXBS)))
		return erc;
	tx.blk[tx.n] = b;
	*slot = (S32)tx.n++;
	return ErcOk;
}

/* metadata block write: into the transaction, or straight to disk without a journal */
static erc_t mwr(struct ext4 *e, U64 b, const U8 *buf)
{
	S32 s;
	erc_t erc;

	if (tx.e != e)
		return bwr(e, b, buf);
	if ((erc = tx_take(e, b, &s)))
		return erc;
	memcpy(tx.img + (U64)s * MAXBS, buf, e->bs);
	return ErcOk;
}

/* 512-byte sector of the volume (inodes, superblock), as seen by the transaction */
static erc_t secrd(struct ext4 *e, U64 sec, U8 *buf)
{
	S32 s = tx_slot(e, (sec - e->base) / e->spb);

	if (s < 0)
		return bc_read(e->dev, sec, buf);
	memcpy(buf, tx.img + (U64)s * MAXBS + ((sec - e->base) % e->spb) * 512, 512);
	return ErcOk;
}

static erc_t secwr(struct ext4 *e, U64 sec, const U8 *buf)
{
	S32 s;
	erc_t erc;

	if (tx.e != e)
		return bc_write(e->dev, sec, buf);
	if ((erc = tx_take(e, (sec - e->base) / e->spb, &s)))
		return erc;
	memcpy(tx.img + (U64)s * MAXBS + ((sec - e->base) % e->spb) * 512, buf, 512);
	return ErcOk;
}

/* ---- superblock ---- */

static erc_t sb_write(struct ext4 *e)
{
	erc_t erc;

	if (e->csum)
		put32(e->sb + 0x3FC, crc32c(~0U, e->sb, 0x3FC));
	if ((erc = secwr(e, e->base + 2, e->sb)))
		return erc;
	return secwr(e, e->base + 3, e->sb + 512);
}

/* the superblock straight to disk (journal commit and recovery), with or without needs_recovery */
static erc_t sb_write_direct(struct ext4 *e, const U8 *sb, bool recover)
{
	U8 s[1024];
	erc_t erc;

	memcpy(s, sb, sizeof(s));
	put32(s + 0x60, recover ? le32(s + 0x60) | INCOMPAT_RECOVER : le32(s + 0x60) & ~INCOMPAT_RECOVER);
	if (e->csum)
		put32(s + 0x3FC, crc32c(~0U, s, 0x3FC));
	if ((erc = bc_write(e->dev, e->base + 2, s)))
		return erc;
	return bc_write(e->dev, e->base + 3, s + 512);
}

static void sb_add_free_blocks(struct ext4 *e, S64 d)
{
	U64 v = le32(e->sb + 0x0C) | ((e->incompat & INCOMPAT_64BIT) ? (U64)le32(e->sb + 0x158) << 32 : 0);

	v += (U64)d;
	put32(e->sb + 0x0C, (U32)v);
	if (e->incompat & INCOMPAT_64BIT)
		put32(e->sb + 0x158, (U32)(v >> 32));
}

static void sb_add_free_inodes(struct ext4 *e, S32 d)
{
	put32(e->sb + 0x10, le32(e->sb + 0x10) + (U32)d);
}

/* ---- group descriptors ---- */

struct gd {
	U8 raw[64];
	U32 g;
};

static U64 gd_u64(const struct ext4 *e, const struct gd *d, U32 lo, U32 hi)
{
	return le32(d->raw + lo) | (e->dsize >= 64 ? (U64)le32(d->raw + hi) << 32 : 0);
}

static U32 gd_u32(const struct ext4 *e, const struct gd *d, U32 lo, U32 hi)	/* 16+16-bit fields */
{
	return le16(d->raw + lo) | (e->dsize >= 64 ? (U32)le16(d->raw + hi) << 16 : 0);
}

static void gd_set32(const struct ext4 *e, struct gd *d, U32 lo, U32 hi, U32 v)
{
	put16(d->raw + lo, v);
	if (e->dsize >= 64)
		put16(d->raw + hi, v >> 16);
}

#define GD_BLOCK_BITMAP(e, d) gd_u64(e, d, 0x00, 0x20)
#define GD_INODE_BITMAP(e, d) gd_u64(e, d, 0x04, 0x24)
#define GD_INODE_TABLE(e, d)  gd_u64(e, d, 0x08, 0x28)
#define GD_FREE_BLOCKS(e, d)  gd_u32(e, d, 0x0C, 0x2C)
#define GD_FREE_INODES(e, d)  gd_u32(e, d, 0x0E, 0x2E)
#define GD_USED_DIRS(e, d)    gd_u32(e, d, 0x10, 0x30)
#define GD_ITABLE_UNUSED(e, d) gd_u32(e, d, 0x1C, 0x32)

static erc_t gd_get(struct ext4 *e, U32 g, struct gd *d)
{
	U64 off = (U64)g * e->dsize;
	erc_t erc;

	if ((erc = brd(e, e->gdt + off / e->bs, b_gd)))
		return erc;
	memset(d, 0, sizeof(*d));
	memcpy(d->raw, b_gd + off % e->bs, e->dsize);
	d->g = g;
	return ErcOk;
}

static erc_t gd_put(struct ext4 *e, struct gd *d)
{
	U64 off = (U64)d->g * e->dsize;
	erc_t erc;

	if (e->csum) {
		U8 g4[4], z[2] = { 0, 0 };
		U32 c;

		put32(g4, d->g);
		c = crc32c(e->seed, g4, 4);
		c = crc32c(c, d->raw, 0x1E);
		c = crc32c(c, z, 2);
		if (e->dsize > 0x20)
			c = crc32c(c, d->raw + 0x20, e->dsize - 0x20);
		put16(d->raw + 0x1E, c & 0xFFFF);
	}
	if ((erc = brd(e, e->gdt + off / e->bs, b_gd)))
		return erc;
	memcpy(b_gd + off % e->bs, d->raw, e->dsize);
	return mwr(e, e->gdt + off / e->bs, b_gd);
}

static void bitmap_csum(struct ext4 *e, struct gd *d, const U8 *bm, bool inodes)
{
	U32 c;

	if (!e->csum)
		return;
	c = crc32c(e->seed, bm, (inodes ? e->ipg : e->bpg) / 8);
	put16(d->raw + (inodes ? 0x1A : 0x18), c & 0xFFFF);
	if (e->dsize >= 64)
		put16(d->raw + (inodes ? 0x3A : 0x38), c >> 16);
}

/* ---- inodes ---- */

static erc_t ino_loc(struct ext4 *e, U32 ino, U64 *sector, U32 *off)
{
	struct gd d;
	U32 g = (ino - 1) / e->ipg, idx = (ino - 1) % e->ipg;
	U64 byte = (U64)idx * e->isize;
	erc_t erc;

	if (!ino || ino > e->ninodes)
		return ErcBrokenFile;
	if ((erc = gd_get(e, g, &d)))
		return erc;
	*sector = e->base + GD_INODE_TABLE(e, &d) * e->spb + byte / 512;
	*off = (U32)(byte % 512);
	return ErcOk;
}

static U32 ino_seed(struct ext4 *e, U32 ino, const U8 *raw)
{
	U8 b[4];
	U32 c;

	put32(b, ino);
	c = crc32c(e->seed, b, 4);
	return crc32c(c, raw + 0x64, 4);		/* i_generation */
}

static U32 ino_csum(struct ext4 *e, U32 ino, const U8 *raw)
{
	U8 tmp[512];
	bool hi = e->isize > 128 && le16(raw + 0x80) >= 4;

	memcpy(tmp, raw, e->isize);
	put16(tmp + 0x7C, 0);
	if (hi)
		put16(tmp + 0x82, 0);
	return crc32c(ino_seed(e, ino, raw), tmp, e->isize);
}

static erc_t iget(struct ext4 *e, U32 ino, U8 *raw)
{
	U8 sec[512];
	U64 s;
	U32 off;
	erc_t erc;

	if ((erc = ino_loc(e, ino, &s, &off)) || (erc = secrd(e, s, sec)))
		return erc;
	memcpy(raw, sec + off, e->isize);
	if (e->csum && le16(raw)) {
		U32 c = ino_csum(e, ino, raw), want = le16(raw + 0x7C);

		if (e->isize > 128 && le16(raw + 0x80) >= 4)
			want |= (U32)le16(raw + 0x82) << 16;
		else
			c &= 0xFFFF;
		if (c != want) {
			kprintf("ext4 %c: inode %u checksum mismatch\n", e->vol->letter, ino);
			return ErcBrokenFile;
		}
	}
	return ErcOk;
}

static erc_t iput(struct ext4 *e, U32 ino, U8 *raw)
{
	U8 sec[512];
	U64 s;
	U32 off;
	erc_t erc;

	if (e->csum) {
		U32 c = ino_csum(e, ino, raw);

		put16(raw + 0x7C, c & 0xFFFF);
		if (e->isize > 128 && le16(raw + 0x80) >= 4)
			put16(raw + 0x82, c >> 16);
	}
	if ((erc = ino_loc(e, ino, &s, &off)) || (erc = secrd(e, s, sec)))
		return erc;
	memcpy(sec + off, raw, e->isize);
	return secwr(e, s, sec);
}

static U64 i_size(const U8 *raw) { return le32(raw + 0x04) | ((U64)le32(raw + 0x6C) << 32); }
static void set_size(U8 *raw, U64 s) { put32(raw + 0x04, (U32)s); put32(raw + 0x6C, (U32)(s >> 32)); }
static U16 i_mode(const U8 *raw) { return le16(raw); }
static U32 i_flags(const U8 *raw) { return le32(raw + 0x20); }
static U64 i_blocks(const U8 *raw) { return le32(raw + 0x1C) | ((U64)le16(raw + 0x74) << 32); }

static void add_blocks(struct ext4 *e, U8 *raw, S64 nblk)	/* i_blocks is in 512-byte units */
{
	U64 v = i_blocks(raw) + (U64)(nblk * e->spb);

	put32(raw + 0x1C, (U32)v);
	put16(raw + 0x74, (U32)(v >> 32));
}

static void touch(U8 *raw, bool mtime)
{
	U32 now = unix_time();

	put32(raw + 0x0C, now);			/* ctime */
	if (mtime)
		put32(raw + 0x10, now);		/* mtime */
}

/* ---- extents: reading ---- */

static bool ext_header_ok(const U8 *h)
{
	return le16(h) == EXT_MAGIC && le16(h + 2) <= le16(h + 4);
}

/* Physical block for logical block lblk (0 = hole / unwritten), and how many blocks from lblk
   on are mapped the same way (contiguously, or as a hole up to the next extent). */
static erc_t ext_map(struct ext4 *e, const U8 *raw, U32 lblk, U64 *pblk, U32 *run)
{
	const U8 *h = raw + 0x28;
	U32 depth, n, i;
	erc_t erc;

	if (!(i_flags(raw) & FL_EXTENTS))
		return ErcNotSupported;			/* block-mapped (ext2/3) file */
	for (;;) {
		if (!ext_header_ok(h))
			return ErcBrokenFile;
		depth = le16(h + 6);
		n = le16(h + 2);
		if (!depth)
			break;
		for (i = n; i > 1 && le32(h + 12 + 12 * (i - 1)) > lblk; i--)
			;
		if (!n)
			break;
		{
			const U8 *ix = h + 12 + 12 * (i - 1);
			U64 child = le32(ix + 4) | ((U64)le16(ix + 8) << 32);

			if ((erc = brd(e, child, b_leaf)))
				return erc;
			h = b_leaf;
		}
	}
	*pblk = 0;
	*run = 0xFFFFFFFF;
	for (i = 0; i < n; i++) {
		const U8 *x = h + 12 + 12 * i;
		U32 start = le32(x), len = le16(x + 4);
		bool unwritten = len > 32768;
		U64 p = le32(x + 8) | ((U64)le16(x + 6) << 32);

		if (unwritten)
			len -= 32768;
		if (lblk < start) {
			*run = start - lblk;			/* hole until this extent */
			return ErcOk;
		}
		if (lblk < start + len) {
			*pblk = unwritten ? 0 : p + (lblk - start);
			*run = start + len - lblk;
			return ErcOk;
		}
	}
	return ErcOk;
}

/* ---- directories: reading ---- */

typedef bool (*ent_fn)(void *ctx, const char *name, U32 nlen, U32 ino, U8 type);

static erc_t dir_walk(struct ext4 *e, const U8 *draw, ent_fn fn, void *ctx)
{
	U64 nblk = (i_size(draw) + e->bs - 1) / e->bs, l, pblk;
	U32 run;
	erc_t erc;

	for (l = 0; l < nblk; l++) {
		U32 off = 0;

		if ((erc = ext_map(e, draw, (U32)l, &pblk, &run)))
			return erc;
		if (!pblk)
			continue;
		if ((erc = brd(e, pblk, b_dir)))
			return erc;
		while (off + 8 <= e->bs) {
			U8 *d = b_dir + off;
			U32 rec = le16(d + 4), nl = d[6];

			if (rec < 8 || off + rec > e->bs || nl + 8 > rec)
				return ErcBrokenFile;
			if (le32(d) && nl && fn(ctx, (const char *)d + 8, nl, le32(d), d[7]))
				return ErcOk;
			off += rec;
		}
	}
	return ErcNoMatch;
}

struct dfind {
	const char *name;
	U32 nlen, ino;
	U8 type;
};

static bool dfind_fn(void *c, const char *name, U32 nlen, U32 ino, U8 type)
{
	struct dfind *x = c;

	if (nlen != x->nlen || memcmp(name, x->name, nlen))
		return false;
	x->ino = ino;
	x->type = type;
	return true;
}

static erc_t dir_find(struct ext4 *e, const U8 *draw, const char *name, U32 nlen, U32 *ino)
{
	struct dfind x = { name, nlen, 0, 0 };
	erc_t erc = dir_walk(e, draw, dfind_fn, &x);

	if (erc && erc != ErcNoMatch)
		return erc;
	*ino = x.ino;
	return x.ino ? ErcOk : ErcNoSuchFile;
}

/* ---- nodes and paths ---- */

static erc_t fill(struct ext4 *e, U32 ino, U32 dir_ino, const char *name, U32 nlen, struct fnode *n)
{
	U8 raw[512];
	erc_t erc;

	if ((erc = iget(e, ino, raw)))
		return erc;
	memset(n, 0, sizeof(*n));
	n->vol = e->vol;
	n->key = ino;
	n->ino = ino;
	n->dir_ino = dir_ino;
	n->size = i_size(raw);
	n->attr = (i_mode(raw) & S_IFMT) == S_IFDIR ? ATTR_DIR : 0;
	n->mtime = le32(raw + 0x10);
	n->is_root = ino == ROOT_INO;
	if (nlen > 255)
		nlen = 255;
	memcpy(n->name, name, nlen);
	return ErcOk;
}

static struct ext4 *of(const char *path, U32 len)
{
	struct volume *v = len >= 2 && path[1] == ':' ? vol_get(path[0]) : NULL;

	return v ? v->ext4 : NULL;
}

/* Resolve "F:\a\b\c" (both separators). parent: stop at the last component's directory. */
static erc_t resolve(const char *path, U32 len, bool parent, struct fnode *n, const char **last,
		     U32 *lastlen)
{
	struct ext4 *e = of(path, len);
	U32 i = 2, s;
	U8 raw[512];
	erc_t erc;

	if (!e)
		return ErcNoSuchDrive;
	if ((erc = fill(e, ROOT_INO, ROOT_INO, "", 0, n)))
		return erc;
	if (last) {
		*last = path + len;
		*lastlen = 0;
	}
	for (;;) {
		U32 ino;

		while (i < len && (path[i] == '\\' || path[i] == '/'))
			i++;
		if (i >= len)
			return ErcOk;
		for (s = i; i < len && path[i] != '\\' && path[i] != '/'; i++)
			;
		if (parent) {
			U32 j = i;

			while (j < len && (path[j] == '\\' || path[j] == '/'))
				j++;
			if (j >= len) {
				*last = path + s;
				*lastlen = i - s;
				return (i - s) > 255 ? ErcBadFileSpec : ErcOk;
			}
		}
		if (!(n->attr & ATTR_DIR))
			return ErcNoSuchDir;
		if ((erc = iget(e, n->ino, raw)))
			return erc;
		erc = dir_find(e, raw, path + s, i - s, &ino);
		if (erc == ErcNoSuchFile) {
			U32 j = i;

			while (j < len && (path[j] == '\\' || path[j] == '/'))
				j++;
			return j >= len ? ErcNoSuchFile : ErcNoSuchDir;
		}
		if (erc)
			return erc;
		if ((erc = fill(e, ino, n->ino, path + s, i - s, n)))
			return erc;
	}
}

static erc_t e4_lookup(const char *path, U32 len, struct fnode *n)
{
	return resolve(path, len, false, n, NULL, NULL);
}

/* ---- file data: reading ---- */

static erc_t e4_read(struct fnode *n, U64 off, U8 *buf, U64 len, U64 *done)
{
	struct ext4 *e = n->vol->ext4;
	U8 raw[512];
	erc_t erc;

	*done = 0;
	if ((erc = iget(e, n->ino, raw)))
		return erc;
	n->size = i_size(raw);
	if (off >= n->size || !len)
		return ErcOk;
	if (len > n->size - off)
		len = n->size - off;
	while (*done < len) {
		U64 pos = off + *done, pblk;
		U32 boff = (U32)(pos % e->bs), k = e->bs - boff, run;

		if (k > len - *done)
			k = (U32)(len - *done);
		if ((erc = ext_map(e, raw, (U32)(pos / e->bs), &pblk, &run)))
			return erc;
		if (pblk) {
			if ((erc = brd(e, pblk, b_data)))
				return erc;
			memcpy(buf + *done, b_data + boff, k);
		} else {
			memset(buf + *done, 0, k);		/* hole */
		}
		*done += k;
	}
	return ErcOk;
}

/* ---- allocation ---- */

/* Up to `want` free blocks in a row, starting the search at goal. */
static erc_t balloc(struct ext4 *e, U64 goal, U32 want, U64 *start, U32 *got)
{
	U32 gi, g0;

	if (goal < e->first_data || goal >= e->nblocks)
		goal = e->first_data;
	g0 = (U32)((goal - e->first_data) / e->bpg);
	for (gi = 0; gi < e->ngroups; gi++) {
		U32 g = (g0 + gi) % e->ngroups, nbits, i, from;
		U64 gstart = e->first_data + (U64)g * e->bpg;
		struct gd d;
		erc_t erc;

		if ((erc = gd_get(e, g, &d)))
			return erc;
		if ((d.raw[0x12] & BG_BLOCK_UNINIT) || !GD_FREE_BLOCKS(e, &d))
			continue;
		if ((erc = brd(e, GD_BLOCK_BITMAP(e, &d), b_bm)))
			return erc;
		nbits = e->nblocks - gstart < e->bpg ? (U32)(e->nblocks - gstart) : e->bpg;
		from = gi == 0 && goal >= gstart ? (U32)(goal - gstart) : 0;
		for (i = from; i < nbits + from; i++) {		/* from the goal, wrapping around */
			U32 n = 0, b = i % nbits;

			if (b_bm[b / 8] & (1 << (b % 8)))
				continue;
			i = b;
			while (n < want && i + n < nbits && !(b_bm[(i + n) / 8] & (1 << ((i + n) % 8)))) {
				b_bm[(i + n) / 8] |= (U8)(1 << ((i + n) % 8));
				n++;
			}
			bitmap_csum(e, &d, b_bm, false);
			if ((erc = mwr(e, GD_BLOCK_BITMAP(e, &d), b_bm)))
				return erc;
			gd_set32(e, &d, 0x0C, 0x2C, GD_FREE_BLOCKS(e, &d) - n);
			if ((erc = gd_put(e, &d)))
				return erc;
			sb_add_free_blocks(e, -(S64)n);
			if ((erc = sb_write(e)))
				return erc;
			*start = gstart + i;
			*got = n;
			return ErcOk;
		}
	}
	return ErcDiskFull;
}

static erc_t bfree(struct ext4 *e, U64 start, U64 n)
{
	while (n) {
		U32 g = (U32)((start - e->first_data) / e->bpg), i = (U32)((start - e->first_data) % e->bpg), k = 0;
		struct gd d;
		erc_t erc;

		if ((erc = gd_get(e, g, &d)) || (erc = brd(e, GD_BLOCK_BITMAP(e, &d), b_bm)))
			return erc;
		while (n && i < e->bpg) {
			b_bm[i / 8] &= (U8)~(1 << (i % 8));
			i++;
			k++;
			n--;
			start++;
		}
		bitmap_csum(e, &d, b_bm, false);
		if ((erc = mwr(e, GD_BLOCK_BITMAP(e, &d), b_bm)))
			return erc;
		gd_set32(e, &d, 0x0C, 0x2C, GD_FREE_BLOCKS(e, &d) + k);
		if ((erc = gd_put(e, &d)))
			return erc;
		sb_add_free_blocks(e, k);
	}
	return sb_write(e);
}

static erc_t ialloc(struct ext4 *e, bool dir, U32 *ino)
{
	U32 g;
	erc_t erc;

	for (g = 0; g < e->ngroups; g++) {
		struct gd d;
		U32 i, unused;

		if ((erc = gd_get(e, g, &d)))
			return erc;
		if ((d.raw[0x12] & BG_INODE_UNINIT) || !GD_FREE_INODES(e, &d))
			continue;
		if ((erc = brd(e, GD_INODE_BITMAP(e, &d), b_bm)))
			return erc;
		for (i = g == 0 ? e->first_ino - 1 : 0; i < e->ipg; i++)
			if (!(b_bm[i / 8] & (1 << (i % 8))))
				break;
		if (i == e->ipg)
			continue;
		b_bm[i / 8] |= (U8)(1 << (i % 8));
		bitmap_csum(e, &d, b_bm, true);
		if ((erc = mwr(e, GD_INODE_BITMAP(e, &d), b_bm)))
			return erc;
		gd_set32(e, &d, 0x0E, 0x2E, GD_FREE_INODES(e, &d) - 1);
		if (dir)
			gd_set32(e, &d, 0x10, 0x30, GD_USED_DIRS(e, &d) + 1);
		unused = GD_ITABLE_UNUSED(e, &d);
		if (i >= e->ipg - unused)
			gd_set32(e, &d, 0x1C, 0x32, e->ipg - i - 1);
		if ((erc = gd_put(e, &d)))
			return erc;
		sb_add_free_inodes(e, -1);
		if ((erc = sb_write(e)))
			return erc;
		*ino = g * e->ipg + i + 1;
		return ErcOk;
	}
	return ErcDiskFull;
}

static erc_t ifree(struct ext4 *e, U32 ino, bool dir)
{
	U32 g = (ino - 1) / e->ipg, i = (ino - 1) % e->ipg;
	struct gd d;
	erc_t erc;

	if ((erc = gd_get(e, g, &d)) || (erc = brd(e, GD_INODE_BITMAP(e, &d), b_bm)))
		return erc;
	b_bm[i / 8] &= (U8)~(1 << (i % 8));
	bitmap_csum(e, &d, b_bm, true);
	if ((erc = mwr(e, GD_INODE_BITMAP(e, &d), b_bm)))
		return erc;
	gd_set32(e, &d, 0x0E, 0x2E, GD_FREE_INODES(e, &d) + 1);
	if (dir)
		gd_set32(e, &d, 0x10, 0x30, GD_USED_DIRS(e, &d) - 1);
	if ((erc = gd_put(e, &d)))
		return erc;
	sb_add_free_inodes(e, 1);
	return sb_write(e);
}

/* ---- extents: writing (depth 0 and 1) ---- */

static U32 leaf_max(struct ext4 *e) { return (e->bs - 12) / 12; }

static erc_t leaf_write(struct ext4 *e, U32 ino, const U8 *raw, U64 blk, U8 *leaf)
{
	if (e->csum)
		put32(leaf + 12 + 12 * le16(leaf + 4), crc32c(ino_seed(e, ino, raw), leaf, 12 + 12 * le16(leaf + 4)));
	return mwr(e, blk, leaf);
}

static void ext_set(U8 *x, U32 lblk, U64 pblk, U32 len)
{
	put32(x, lblk);
	put16(x + 4, len);
	put16(x + 6, (U32)(pblk >> 32));
	put32(x + 8, (U32)pblk);
}

/* Insert (lblk, pblk, len) into the extent array of header h (sorted, merging with the previous
   extent when contiguous). Returns false if the array is full. */
static bool ext_insert(U8 *h, U32 lblk, U64 pblk, U32 len)
{
	U32 n = le16(h + 2), max = le16(h + 4), i;
	U8 *x;

	for (i = 0; i < n && le32(h + 12 + 12 * i) < lblk; i++)
		;
	if (i > 0) {
		U8 *p = h + 12 + 12 * (i - 1);
		U32 ps = le32(p), pl = le16(p + 4);
		U64 pp = le32(p + 8) | ((U64)le16(p + 6) << 32);

		if (pl <= 32768 && ps + pl == lblk && pp + pl == pblk && pl + len <= 32768) {
			put16(p + 4, pl + len);
			return true;
		}
	}
	if (n == max)
		return false;
	x = h + 12 + 12 * i;
	memmove(x + 12, x, 12 * (n - i));
	ext_set(x, lblk, pblk, len);
	put16(h + 2, n + 1);
	return true;
}

static erc_t ext_add(struct ext4 *e, U32 ino, U8 *raw, U32 lblk, U64 pblk, U32 len)
{
	U8 *root = raw + 0x28;
	erc_t erc;

	if (le16(root + 6) == 0) {
		if (ext_insert(root, lblk, pblk, len))
			return ErcOk;
		/* root full: move its extents into a new leaf, root becomes an index (depth 1) */
		{
			U64 lb;
			U32 got;

			if ((erc = balloc(e, pblk, 1, &lb, &got)))
				return erc;
			add_blocks(e, raw, 1);
			memset(b_leaf, 0, e->bs);
			put16(b_leaf, EXT_MAGIC);
			put16(b_leaf + 2, le16(root + 2));
			put16(b_leaf + 4, leaf_max(e));
			memcpy(b_leaf + 12, root + 12, 12 * le16(root + 2));
			if ((erc = leaf_write(e, ino, raw, lb, b_leaf)))
				return erc;
			put16(root + 2, 1);
			put16(root + 6, 1);
			put32(root + 12, le32(b_leaf + 12));		/* ei_block */
			put32(root + 16, (U32)lb);			/* ei_leaf_lo */
			put16(root + 20, (U32)(lb >> 32));		/* ei_leaf_hi */
			put16(root + 22, 0);
		}
	}
	if (le16(root + 6) != 1)
		return ErcNotSupported;			/* deeper trees: read only for now */
	{
		U32 n = le16(root + 2), i;
		U8 *ix;
		U64 lb;

		for (i = n; i > 1 && le32(root + 12 + 12 * (i - 1)) > lblk; i--)
			;
		ix = root + 12 + 12 * (i - 1);
		lb = le32(ix + 4) | ((U64)le16(ix + 8) << 32);
		if ((erc = brd(e, lb, b_leaf)))
			return erc;
		if (ext_insert(b_leaf, lblk, pblk, len)) {
			if (lblk < le32(ix))
				put32(ix, lblk);
			return leaf_write(e, ino, raw, lb, b_leaf);
		}
		/* leaf full: split it in two */
		if (n == 4)
			return ErcNotSupported;		/* > 4 leaves (> 1360 extents) */
		{
			U32 half = le16(b_leaf + 2) / 2, rest = le16(b_leaf + 2) - half;
			U64 nb;
			U32 got;

			if ((erc = balloc(e, lb, 1, &nb, &got)))
				return erc;
			add_blocks(e, raw, 1);
			memset(b_leaf2, 0, e->bs);
			put16(b_leaf2, EXT_MAGIC);
			put16(b_leaf2 + 2, rest);
			put16(b_leaf2 + 4, leaf_max(e));
			memcpy(b_leaf2 + 12, b_leaf + 12 + 12 * half, 12 * rest);
			put16(b_leaf + 2, half);
			memset(b_leaf + 12 + 12 * half, 0, 12 * rest);
			if ((erc = leaf_write(e, ino, raw, lb, b_leaf)) ||
			    (erc = leaf_write(e, ino, raw, nb, b_leaf2)))
				return erc;
			memmove(ix + 24, ix + 12, 12 * (n - i));
			put32(ix + 12, le32(b_leaf2 + 12));
			put32(ix + 16, (U32)nb);
			put16(ix + 20, (U32)(nb >> 32));
			put16(ix + 22, 0);
			put16(root + 2, n + 1);
		}
		return ext_add(e, ino, raw, lblk, pblk, len);	/* now there is room */
	}
}

/* Free every block at logical >= keep. */
static erc_t ext_cut_array(struct ext4 *e, U8 *raw, U8 *h, U32 keep)
{
	U32 n = le16(h + 2), i, w = 0;
	erc_t erc;

	for (i = 0; i < n; i++) {
		U8 *x = h + 12 + 12 * i;
		U32 s = le32(x), len = le16(x + 4), real = len > 32768 ? len - 32768 : len;
		U64 p = le32(x + 8) | ((U64)le16(x + 6) << 32);

		if (s >= keep) {
			if ((erc = bfree(e, p, real)))
				return erc;
			add_blocks(e, raw, -(S64)real);
			continue;
		}
		if (s + real > keep) {
			if ((erc = bfree(e, p + (keep - s), s + real - keep)))
				return erc;
			add_blocks(e, raw, -(S64)(s + real - keep));
			put16(x + 4, (len > 32768 ? 32768 : 0) + keep - s);
		}
		if (w != i)
			memcpy(h + 12 + 12 * w, x, 12);
		w++;
	}
	memset(h + 12 + 12 * w, 0, 12 * (n - w));
	put16(h + 2, w);
	return ErcOk;
}

static erc_t ext_truncate(struct ext4 *e, U32 ino, U8 *raw, U32 keep)
{
	U8 *root = raw + 0x28;
	U32 depth = le16(root + 6), n, i, w = 0;
	erc_t erc;

	if (depth == 0)
		return ext_cut_array(e, raw, root, keep);
	if (depth != 1)
		return ErcNotSupported;
	n = le16(root + 2);
	for (i = 0; i < n; i++) {
		U8 *ix = root + 12 + 12 * i;
		U64 lb = le32(ix + 4) | ((U64)le16(ix + 8) << 32);

		if ((erc = brd(e, lb, b_leaf)) || (erc = ext_cut_array(e, raw, b_leaf, keep)))
			return erc;
		if (!le16(b_leaf + 2)) {		/* leaf now empty: free it */
			if ((erc = bfree(e, lb, 1)))
				return erc;
			add_blocks(e, raw, -1);
			continue;
		}
		if ((erc = leaf_write(e, ino, raw, lb, b_leaf)))
			return erc;
		if (w != i)
			memcpy(root + 12 + 12 * w, ix, 12);
		w++;
	}
	memset(root + 12 + 12 * w, 0, 12 * (n - w));
	put16(root + 2, w);
	if (!w)
		put16(root + 6, 0);			/* back to an empty depth-0 tree */
	return ErcOk;
}

/* ---- file data: writing ---- */

static erc_t write_blocks(struct ext4 *e, U32 ino, U8 *raw, U64 off, const U8 *buf, U64 len, U64 *done)
{
	U64 fresh_lo = 1, fresh_hi = 0;		/* logical blocks allocated by this call */
	erc_t erc;

	*done = 0;
	while (*done < len) {
		U64 pos = off + *done, pblk;
		U32 lblk = (U32)(pos / e->bs), boff = (U32)(pos % e->bs), k = e->bs - boff, run;

		if (k > len - *done)
			k = (U32)(len - *done);
		if ((erc = ext_map(e, raw, lblk, &pblk, &run)))
			return erc;
		if (!pblk) {				/* hole: allocate a run for the rest of the write */
			U64 last = (off + len - 1) / e->bs, goal = 0, start, prev;
			U32 want = (U32)(last - lblk + 1), got, r2;

			if (want > run)
				want = run;
			if (want > 32768)
				want = 32768;
			if (lblk && ext_map(e, raw, lblk - 1, &prev, &r2) == ErcOk && prev)
				goal = prev + 1;
			if ((erc = balloc(e, goal, want, &start, &got)))
				return erc;
			if ((erc = ext_add(e, ino, raw, lblk, start, got)))
				return erc;
			add_blocks(e, raw, got);
			fresh_lo = lblk;
			fresh_hi = (U64)lblk + got - 1;
			pblk = start;
		}
		if (k == e->bs) {
			if ((erc = bwr(e, pblk, buf + *done)))
				return erc;
		} else {
			if (lblk >= fresh_lo && lblk <= fresh_hi)
				memset(b_data, 0, e->bs);	/* new block: no stale data */
			else if ((erc = brd(e, pblk, b_data)))
				return erc;
			memcpy(b_data + boff, buf + *done, k);
			if ((erc = bwr(e, pblk, b_data)))
				return erc;
		}
		*done += k;
	}
	return ErcOk;
}

static erc_t e4_write(struct fnode *n, U64 off, const U8 *buf, U64 len, U64 *done)
{
	struct ext4 *e = n->vol->ext4;
	U8 raw[512];
	erc_t erc;

	*done = 0;
	if (!len)
		return ErcOk;
	if (off + len > (U64)0xFFFFFFFF * e->bs)
		return ErcDiskFull;			/* 32-bit logical block numbers */
	if ((erc = iget(e, n->ino, raw)))
		return erc;
	erc = write_blocks(e, n->ino, raw, off, buf, len, done);
	if (off + *done > i_size(raw))
		set_size(raw, off + *done);
	touch(raw, true);
	n->size = i_size(raw);
	{
		erc_t e2 = iput(e, n->ino, raw);

		return erc ? erc : e2;
	}
}

static erc_t e4_truncate(struct fnode *n, U64 size)
{
	struct ext4 *e = n->vol->ext4;
	U8 raw[512];
	erc_t erc;

	if ((erc = iget(e, n->ino, raw)))
		return erc;
	if (size < i_size(raw)) {
		U32 keep = (U32)((size + e->bs - 1) / e->bs);

		if ((erc = ext_truncate(e, n->ino, raw, keep)))
			return erc;
		if (size % e->bs) {			/* zero the tail of the last block */
			U64 pblk;
			U32 run;

			if (ext_map(e, raw, (U32)(size / e->bs), &pblk, &run) == ErcOk && pblk) {
				if ((erc = brd(e, pblk, b_data)))
					return erc;
				memset(b_data + size % e->bs, 0, e->bs - size % e->bs);
				if ((erc = bwr(e, pblk, b_data)))
					return erc;
			}
		}
	}
	set_size(raw, size);			/* growing leaves a hole (sparse file) */
	touch(raw, true);
	n->size = size;
	return iput(e, n->ino, raw);
}

/* ---- directories: writing ---- */

static U32 rec_need(U32 nlen) { return (8 + nlen + 3) & ~3U; }

static U32 dir_usable(struct ext4 *e) { return e->bs - (e->csum ? 12 : 0); }

static void dir_tail(struct ext4 *e, U32 dino, const U8 *draw, U8 *blk)
{
	U8 *t = blk + e->bs - 12;

	if (!e->csum)
		return;
	put32(t, 0);
	put16(t + 4, 12);
	t[6] = 0;
	t[7] = 0xDE;
	put32(t + 8, crc32c(ino_seed(e, dino, draw), blk, e->bs - 12));
}

static erc_t add_link(struct ext4 *e, U32 dino, U8 *draw, const char *name, U32 nlen, U32 ino, U8 type)
{
	U32 need = rec_need(nlen), usable = dir_usable(e);
	U64 nblk = (i_size(draw) + e->bs - 1) / e->bs, l, pblk;
	U32 run;
	erc_t erc;

	if (i_flags(draw) & FL_INDEX)
		return ErcNotSupported;			/* htree directory: read only for now */
	for (l = 0; l < nblk; l++) {
		U32 off = 0;

		if ((erc = ext_map(e, draw, (U32)l, &pblk, &run)))
			return erc;
		if (!pblk)
			continue;
		if ((erc = brd(e, pblk, b_dir)))
			return erc;
		while (off < usable) {
			U8 *d = b_dir + off;
			U32 rec = le16(d + 4), used = le32(d) ? rec_need(d[6]) : 0;

			if (rec < 8 || off + rec > e->bs)
				return ErcBrokenFile;
			if (rec - used >= need) {
				U8 *nd = d + used;

				if (used)
					put16(d + 4, used);
				put32(nd, ino);
				put16(nd + 4, rec - used);
				nd[6] = (U8)nlen;
				nd[7] = type;
				memcpy(nd + 8, name, nlen);
				dir_tail(e, dino, draw, b_dir);
				return mwr(e, pblk, b_dir);
			}
			off += rec;
		}
	}
	/* no room: one more directory block */
	{
		U64 goal = 0, start, prev;
		U32 got, r2;

		if (nblk && ext_map(e, draw, (U32)nblk - 1, &prev, &r2) == ErcOk && prev)
			goal = prev + 1;
		if ((erc = balloc(e, goal, 1, &start, &got)) ||
		    (erc = ext_add(e, dino, draw, (U32)nblk, start, 1)))
			return erc;
		add_blocks(e, draw, 1);
		memset(b_dir, 0, e->bs);
		put32(b_dir, ino);
		put16(b_dir + 4, usable);
		b_dir[6] = (U8)nlen;
		b_dir[7] = type;
		memcpy(b_dir + 8, name, nlen);
		set_size(draw, (nblk + 1) * e->bs);
		dir_tail(e, dino, draw, b_dir);		/* the seed doesn't depend on size: fine */
		if ((erc = mwr(e, start, b_dir)))
			return erc;
		return iput(e, dino, draw);
	}
}

static erc_t del_link(struct ext4 *e, U32 dino, U8 *draw, const char *name, U32 nlen)
{
	U64 nblk = (i_size(draw) + e->bs - 1) / e->bs, l, pblk;
	U32 run, usable = dir_usable(e);
	erc_t erc;

	if (i_flags(draw) & FL_INDEX)
		return ErcNotSupported;
	for (l = 0; l < nblk; l++) {
		U32 off = 0, prev = 0xFFFFFFFF;

		if ((erc = ext_map(e, draw, (U32)l, &pblk, &run)))
			return erc;
		if (!pblk)
			continue;
		if ((erc = brd(e, pblk, b_dir)))
			return erc;
		while (off < usable) {
			U8 *d = b_dir + off;
			U32 rec = le16(d + 4);

			if (rec < 8 || off + rec > e->bs)
				return ErcBrokenFile;
			if (le32(d) && d[6] == nlen && !memcmp(d + 8, name, nlen)) {
				if (prev != 0xFFFFFFFF)
					put16(b_dir + prev + 4, le16(b_dir + prev + 4) + rec);
				else
					put32(d, 0);
				dir_tail(e, dino, draw, b_dir);
				return mwr(e, pblk, b_dir);
			}
			prev = off;
			off += rec;
		}
	}
	return ErcNoSuchFile;
}

static erc_t set_dotdot(struct ext4 *e, U32 dino, const U8 *draw, U32 parent)
{
	U64 pblk;
	U32 run;
	erc_t erc;

	if ((erc = ext_map(e, draw, 0, &pblk, &run)) || !pblk || (erc = brd(e, pblk, b_dir)))
		return erc ? erc : ErcBrokenFile;
	{
		U8 *dd = b_dir + le16(b_dir + 4);	/* '..' follows '.' */

		if (dd[6] != 2 || dd[8] != '.' || dd[9] != '.')
			return ErcBrokenFile;
		put32(dd, parent);
	}
	dir_tail(e, dino, draw, b_dir);
	return mwr(e, pblk, b_dir);
}

static void new_inode(struct ext4 *e, U8 *raw, U16 mode, U16 links)
{
	U32 now = unix_time();

	memset(raw, 0, e->isize);
	put16(raw, mode);
	put32(raw + 0x08, now);
	put32(raw + 0x0C, now);
	put32(raw + 0x10, now);
	put16(raw + 0x1A, links);
	put32(raw + 0x20, FL_EXTENTS);
	put16(raw + 0x28, EXT_MAGIC);		/* empty extent tree, depth 0, room for 4 */
	put16(raw + 0x2C, 4);
	put32(raw + 0x64, (U32)timer_ticks() * 2654435761U);	/* generation */
	if (e->isize >= 160) {
		put16(raw + 0x80, 32);		/* extra_isize */
		put32(raw + 0x90, now);		/* crtime */
	}
}

static erc_t e4_create(const char *path, U32 len, U8 attr, struct fnode *n)
{
	struct ext4 *e = of(path, len);
	struct fnode dir;
	const char *name;
	U32 nlen, ino, tmp;
	U8 draw[512], raw[512];
	bool isdir = attr & ATTR_DIR;
	erc_t erc;

	if (!e)
		return ErcNoSuchDrive;
	if ((erc = resolve(path, len, true, &dir, &name, &nlen)))
		return erc;
	if (!nlen || (nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.'))
		return ErcBadFileSpec;
	if (!(dir.attr & ATTR_DIR))
		return ErcNoSuchDir;
	if ((erc = iget(e, dir.ino, draw)))
		return erc;
	erc = dir_find(e, draw, name, nlen, &tmp);
	if (erc == ErcOk)
		return ErcDupName;
	if (erc != ErcNoSuchFile)
		return erc;
	if (i_flags(draw) & FL_INDEX)
		return ErcNotSupported;
	if ((erc = ialloc(e, isdir, &ino)))
		return erc;

	if (isdir) {
		U64 blk;
		U32 got;

		new_inode(e, raw, S_IFDIR | 0755, 2);
		if ((erc = balloc(e, 0, 1, &blk, &got)) || (erc = ext_add(e, ino, raw, 0, blk, 1)))
			return erc;
		add_blocks(e, raw, 1);
		set_size(raw, e->bs);
		memset(b_dir, 0, e->bs);
		put32(b_dir, ino);			/* "." */
		put16(b_dir + 4, 12);
		b_dir[6] = 1;
		b_dir[7] = FT_DIR;
		b_dir[8] = '.';
		put32(b_dir + 12, dir.ino);		/* ".." */
		put16(b_dir + 16, dir_usable(e) - 12);
		b_dir[18] = 2;
		b_dir[19] = FT_DIR;
		b_dir[20] = '.';
		b_dir[21] = '.';
		dir_tail(e, ino, raw, b_dir);
		if ((erc = mwr(e, blk, b_dir)))
			return erc;
		put16(draw + 0x1A, le16(draw + 0x1A) + 1);	/* the new '..' links the parent */
	} else {
		new_inode(e, raw, S_IFREG | 0644, 1);
	}
	if ((erc = iput(e, ino, raw)))
		return erc;
	touch(draw, true);
	if ((erc = iput(e, dir.ino, draw)))
		return erc;
	if ((erc = add_link(e, dir.ino, draw, name, nlen, ino, isdir ? FT_DIR : FT_REG)))
		return erc;
	return fill(e, ino, dir.ino, name, nlen, n);
}

static erc_t e4_remove(struct fnode *n)
{
	struct ext4 *e = n->vol->ext4;
	U8 raw[512], draw[512];
	bool isdir = n->attr & ATTR_DIR;
	U32 links;
	erc_t erc;

	if (n->is_root)
		return ErcBadFileSpec;
	if ((erc = iget(e, n->dir_ino, draw)) ||
	    (erc = del_link(e, n->dir_ino, draw, n->name, (U32)strlen(n->name))))
		return erc;
	if (isdir)
		put16(draw + 0x1A, le16(draw + 0x1A) - 1);	/* its '..' is gone */
	touch(draw, true);
	if ((erc = iput(e, n->dir_ino, draw)))
		return erc;

	if ((erc = iget(e, n->ino, raw)))
		return erc;
	links = isdir ? 0 : le16(raw + 0x1A) - 1;
	put16(raw + 0x1A, links);
	touch(raw, false);
	if (!links) {
		if ((erc = ext_truncate(e, n->ino, raw, 0)))
			return erc;
		set_size(raw, 0);
		put32(raw + 0x14, unix_time());		/* dtime */
		if ((erc = iput(e, n->ino, raw)))
			return erc;
		return ifree(e, n->ino, isdir);
	}
	return iput(e, n->ino, raw);
}

static erc_t e4_rename(struct fnode *n, const char *newpath, U32 len)
{
	struct ext4 *e = n->vol->ext4;
	struct fnode dir;
	const char *name;
	U32 nlen, tmp;
	U8 draw[512], oraw[512], raw[512];
	bool isdir = n->attr & ATTR_DIR;
	erc_t erc;

	if (n->is_root)
		return ErcBadFileSpec;
	if ((erc = resolve(newpath, len, true, &dir, &name, &nlen)))
		return erc;
	if (!nlen || !(dir.attr & ATTR_DIR))
		return ErcBadFileSpec;
	if (isdir && dir.ino == n->ino)
		return ErcBadFileSpec;
	if ((erc = iget(e, dir.ino, draw)))
		return erc;
	if (dir_find(e, draw, name, nlen, &tmp) == ErcOk)
		return ErcDupName;
	if ((erc = add_link(e, dir.ino, draw, name, nlen, n->ino, isdir ? FT_DIR : FT_REG)))
		return erc;
	if ((erc = iget(e, n->dir_ino, oraw)) ||
	    (erc = del_link(e, n->dir_ino, oraw, n->name, (U32)strlen(n->name))))
		return erc;
	if (isdir && dir.ino != n->dir_ino) {		/* moved to another directory */
		if ((erc = iget(e, n->ino, raw)) || (erc = set_dotdot(e, n->ino, raw, dir.ino)))
			return erc;
		put16(oraw + 0x1A, le16(oraw + 0x1A) - 1);
		touch(oraw, true);
		if ((erc = iput(e, n->dir_ino, oraw)) || (erc = iget(e, dir.ino, draw)))
			return erc;
		put16(draw + 0x1A, le16(draw + 0x1A) + 1);
		touch(draw, true);
		if ((erc = iput(e, dir.ino, draw)))
			return erc;
	} else {
		touch(oraw, true);
		if ((erc = iput(e, n->dir_ino, oraw)))
			return erc;
	}
	n->dir_ino = dir.ino;
	memset(n->name, 0, sizeof(n->name));
	memcpy(n->name, name, nlen);
	return ErcOk;
}

/* ---- ReadDir, dir_empty ---- */

struct nth {
	U32 want, at, ino;
	char name[256];
};

static bool nth_fn(void *c, const char *name, U32 nlen, U32 ino, U8 type)
{
	struct nth *x = c;

	(void)type;
	if ((nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.'))
		return false;
	if (x->at++ != x->want)
		return false;
	memcpy(x->name, name, nlen);
	x->name[nlen] = 0;
	x->ino = ino;
	return true;
}

static erc_t e4_readdir(struct fnode *dir, U32 index, struct dir_entry *out)
{
	struct ext4 *e = dir->vol->ext4;
	struct nth x;
	struct fnode n;
	U8 draw[512];
	erc_t erc;

	memset(&x, 0, sizeof(x));
	x.want = index;
	if ((erc = iget(e, dir->ino, draw)))
		return erc;
	erc = dir_walk(e, draw, nth_fn, &x);
	if (erc && erc != ErcNoMatch)
		return erc;
	if (!x.ino)
		return ErcNoMatch;
	if ((erc = fill(e, x.ino, dir->ino, x.name, (U32)strlen(x.name), &n)))
		return erc;
	memset(out, 0, sizeof(*out));
	memcpy(out->name, x.name, strlen(x.name));
	out->size = n.size;
	out->attr = n.attr;
	out->mtime = n.mtime;
	return ErcOk;
}

static bool any_fn(void *c, const char *name, U32 nlen, U32 ino, U8 type)
{
	(void)c;
	(void)ino;
	(void)type;
	return !((nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.'));
}

static bool e4_dir_empty(struct fnode *dir)
{
	struct ext4 *e = dir->vol->ext4;
	U8 draw[512];

	if (iget(e, dir->ino, draw))
		return false;
	return dir_walk(e, draw, any_fn, NULL) == ErcNoMatch;
}

/* ---- journal (JBD2) ----

   The journal is the file s_journal_inum (inode 8). All JBD2 fields are big-endian. Block 0 is
   the journal superblock; blocks s_first..s_maxlen-1 form a circular log of transactions: a
   descriptor block (tags naming the home block of each logged block), the logged blocks, and a
   commit block, all stamped with the transaction ID. Revoke blocks cancel older copies of a block.

   Recovery (mount, when s_start != 0) follows jbd2/e2fsck: a scan pass finds the last committed
   transaction, a revoke pass collects revoke records, a replay pass writes every logged block that
   isn't revoked by the same or a later transaction to its home. Then the journal is marked empty
   and needs_recovery cleared.

   As on Linux, needs_recovery is set in the superblock when the volume is mounted for writing and
   cleared only by ext4_unmount() (Linux wipes a journal on a volume without that flag). DOORS writes
   a transaction at s_first each time and checkpoints it right away (the journal is empty between
   operations), in this order:
     1. descriptor, blocks, commit block go to the journal;
     2. the journal superblock's s_start points at the transaction: from here a crash replays it;
     3. every block goes to its home, the superblock too;
     4. s_start = 0: the journal is empty.
   A crash before 2 leaves the old metadata (file data written by the operation may be lost); a
   crash after 2 is repaired by the replay, even a torn superblock write in 3. The superblock is
   never written outside the journal while mounted (its two sectors could tear: power-cut test).
   Every ATA write is followed by a cache flush (drivers/ata.c), so the writes reach the disk in
   this order. */

#define COMPAT_HAS_JOURNAL 0x0004
#define JBD2_MAGIC      0xC03B3998U
#define JBD2_DESCRIPTOR 1
#define JBD2_COMMIT     2
#define JBD2_SB_V1      3
#define JBD2_SB_V2      4
#define JBD2_REVOKE     5
#define JF_ESCAPE    1
#define JF_SAME_UUID 2
#define JF_LAST_TAG  8
#define JI_REVOKE  0x01
#define JI_64BIT   0x02
#define JI_ASYNC   0x04
#define JI_CSUM_V2 0x08
#define JI_CSUM_V3 0x10
#define JI_OK      (JI_REVOKE | JI_64BIT | JI_ASYNC | JI_CSUM_V2 | JI_CSUM_V3)	/* replay */
#define JI_WRITE   (JI_REVOKE | JI_64BIT)		/* what DOORS writes itself (no checksums) */
#define NREVOKE 1024

static U8 b_j[MAXBS], b_jd[MAXBS];		/* journal scratch */
U32 ext4_commit_stop;				/* power-cut test (kernel/powercut.c) */
static U32 commits;
static struct { U64 blk; U32 seq; } revoked[NREVOKE];
static U32 nrevoked;

static U32 be32(const U8 *p) { return (U32)p[0] << 24 | (U32)p[1] << 16 | (U32)p[2] << 8 | p[3]; }
static void pbe32(U8 *p, U32 v) { p[0] = (U8)(v >> 24); p[1] = (U8)(v >> 16); p[2] = (U8)(v >> 8); p[3] = (U8)v; }

static erc_t jmap(struct ext4 *e, U32 lblk, U64 *pblk)
{
	U32 run;
	erc_t erc;

	if (lblk >= e->jlen && e->jlen)
		return ErcBrokenFile;
	if ((erc = ext_map(e, e->jraw, lblk, pblk, &run)))
		return erc;
	return *pblk ? ErcOk : ErcBrokenFile;
}

static erc_t jread(struct ext4 *e, U32 lblk, U8 *buf)
{
	U64 p;
	erc_t erc;

	return (erc = jmap(e, lblk, &p)) ? erc : disk_rd(e, p, buf);
}

static erc_t jwrite(struct ext4 *e, U32 lblk, const U8 *buf)
{
	U64 p;
	erc_t erc;

	return (erc = jmap(e, lblk, &p)) ? erc : bwr(e, p, buf);
}

static U32 jnext(struct ext4 *e, U32 lblk)
{
	return ++lblk >= e->jlen ? e->jfirst : lblk;
}

/* the journal superblock with a new s_start and s_sequence (= e->jseq) */
static erc_t jsb_write(struct ext4 *e, U32 start)
{
	erc_t erc;

	if ((erc = jread(e, 0, b_j)))
		return erc;
	pbe32(b_j + 0x18, e->jseq);
	pbe32(b_j + 0x1C, start);
	return jwrite(e, 0, b_j);
}

/* Tags of a descriptor block: calls fn(home block, flags, ctx) for each; returns their number. */
static U32 jtags(struct ext4 *e, const U8 *d, void (*fn)(U64, U32, void *), void *ctx)
{
	U32 off = 12, n = 0, end = e->bs - ((e->jincompat & (JI_CSUM_V2 | JI_CSUM_V3)) ? 4 : 0);

	while (off + e->jtag <= end) {
		U64 blk = be32(d + off);
		U32 flags;

		if (e->jincompat & JI_CSUM_V3) {
			flags = be32(d + off + 4);
			blk |= (U64)be32(d + off + 8) << 32;
		} else {
			flags = (U32)d[off + 6] << 8 | d[off + 7];
			if (e->jincompat & JI_64BIT)
				blk |= (U64)be32(d + off + 8) << 32;
		}
		if (fn)
			fn(blk, flags, ctx);
		n++;
		off += e->jtag + ((flags & JF_SAME_UUID) ? 0 : 16);
		if (flags & JF_LAST_TAG)
			break;
	}
	return n;
}

static bool is_revoked(U64 blk, U32 seq)
{
	U32 i;

	for (i = 0; i < nrevoked; i++)
		if (revoked[i].blk == blk && (S32)(revoked[i].seq - seq) >= 0)
			return true;
	return false;
}

struct replay {
	struct ext4 *e;
	U32 lblk, seq, done;		/* next logged block, transaction, blocks written */
	erc_t erc;
};

static void replay_tag(U64 blk, U32 flags, void *c)
{
	struct replay *r = c;

	r->lblk = jnext(r->e, r->lblk);
	if (r->erc || is_revoked(blk, r->seq))
		return;
	if ((r->erc = jread(r->e, r->lblk, b_jd)))
		return;
	if (flags & JF_ESCAPE)
		pbe32(b_jd, JBD2_MAGIC);
	if (!(r->erc = bwr(r->e, blk, b_jd)))
		r->done++;
}

/* One pass over the log from s_start (pass 0 scan, 1 revoke, 2 replay); *end = first ID not
   committed (set by the scan pass). */
static erc_t jpass(struct ext4 *e, U32 start, int pass, U32 *end, U32 *done)
{
	U32 lblk = start, seq = e->jseq, guard;
	erc_t erc;

	for (guard = 0; guard < e->jlen; guard++) {
		U32 type, n, i;

		if (pass && seq == *end)
			return ErcOk;
		if ((erc = jread(e, lblk, b_j)))
			return erc;
		if (be32(b_j) != JBD2_MAGIC || be32(b_j + 8) != seq)
			break;				/* end of the log */
		type = be32(b_j + 4);
		if (type == JBD2_DESCRIPTOR) {
			if (pass == 2) {
				struct replay r = { e, lblk, seq, 0, ErcOk };

				jtags(e, b_j, replay_tag, &r);	/* the logged blocks are read into b_jd */
				if (r.erc)
					return r.erc;
				*done += r.done;
				lblk = r.lblk;
			} else {
				n = jtags(e, b_j, NULL, NULL);
				for (i = 0; i < n; i++)
					lblk = jnext(e, lblk);
			}
		} else if (type == JBD2_COMMIT) {
			seq++;
			if (pass == 0)
				*end = seq;
		} else if (type == JBD2_REVOKE) {
			if (pass == 1) {
				U32 used = be32(b_j + 12), rs = (e->jincompat & JI_64BIT) ? 8 : 4, off;

				for (off = 16; off + rs <= used && off + rs <= e->bs; off += rs) {
					if (nrevoked == NREVOKE)
						return ErcNotSupported;
					revoked[nrevoked].blk = rs == 8 ? (U64)be32(b_j + off) << 32 |
							be32(b_j + off + 4) : be32(b_j + off);
					revoked[nrevoked++].seq = seq;
				}
			}
		} else {
			break;
		}
		lblk = jnext(e, lblk);
	}
	return ErcOk;
}

/* Mount: load the journal, replay it if it holds committed transactions. *ro is set when DOORS
   must not write (an external journal, or features it can replay but not write). */
static erc_t journal_mount(struct ext4 *e, bool *ro)
{
	U32 jino = le32(e->sb + 0xE0), start, end = 0, done = 0, type;
	bool recover = e->incompat & INCOMPAT_RECOVER;
	erc_t erc;

	if (!(le32(e->sb + 0x5C) & COMPAT_HAS_JOURNAL))
		return ErcOk;
	if (!jino) {
		kprintf("ext4 on %c: external journal, mounted read-only\n", e->vol->letter);
		*ro = true;
		return ErcOk;
	}
	e->jlen = 0;
	if ((erc = iget(e, jino, e->jraw)) || (erc = jread(e, 0, b_j)))
		return erc;
	type = be32(b_j + 4);
	if (be32(b_j) != JBD2_MAGIC || (type != JBD2_SB_V1 && type != JBD2_SB_V2) || be32(b_j + 0xC) != e->bs)
		return ErcBrokenFile;
	e->jlen = be32(b_j + 0x10);
	e->jfirst = be32(b_j + 0x14);
	e->jseq = be32(b_j + 0x18);
	start = be32(b_j + 0x1C);
	e->jincompat = type == JBD2_SB_V2 ? be32(b_j + 0x28) : 0;
	memcpy(e->juuid, b_j + 0x30, 16);
	if (e->jincompat & ~JI_OK) {
		kprintf("ext4 on %c: journal features %x not supported\n", e->vol->letter, e->jincompat);
		return ErcNotSupported;
	}
	e->jtag = (e->jincompat & JI_CSUM_V3) ? 16 :
		  8 + ((e->jincompat & JI_CSUM_V2) ? 2 : 0) + ((e->jincompat & JI_64BIT) ? 4 : 0);
	if (!e->jfirst || e->jfirst >= e->jlen || e->jlen > i_size(e->jraw) / e->bs)
		return ErcBrokenFile;

	if (start) {
		nrevoked = 0;
		if ((erc = jpass(e, start, 0, &end, &done)) || (erc = jpass(e, start, 1, &end, &done)) ||
		    (erc = jpass(e, start, 2, &end, &done)))
			return erc;
		kprintf("ext4 on %c: journal replayed: %u transactions, %u blocks\n", e->vol->letter,
			end - e->jseq, done);
		e->jseq = end;
		if ((erc = jsb_write(e, 0)))
			return erc;
		recover = true;
	}
	if (recover) {				/* the replay may have rewritten the superblock */
		if ((erc = bc_read(e->dev, e->base + 2, e->sb)) || (erc = bc_read(e->dev, e->base + 3, e->sb + 512)))
			return erc;
		put32(e->sb + 0x60, le32(e->sb + 0x60) & ~INCOMPAT_RECOVER);
		e->incompat = le32(e->sb + 0x60);
		e->ro_compat = le32(e->sb + 0x64);
		if ((erc = sb_write_direct(e, e->sb, false)))
			return erc;
		if (e->csum)
			put32(e->sb + 0x3FC, crc32c(~0U, e->sb, 0x3FC));
	}
	if (e->jincompat & ~JI_WRITE) {
		kprintf("ext4 on %c: journal features %x: replayed, but mounted read-only\n", e->vol->letter,
			e->jincompat);
		*ro = true;
		return ErcOk;
	}
	if (!tx.img && AllocOSPage(TXMAX * MAXBS / PAGE_SIZE, &tx.img))
		return ErcNoMem;
	e->txmax = (e->jlen - e->jfirst) / 2 < TXMAX ? (e->jlen - e->jfirst) / 2 : TXMAX;
	/* needs_recovery stays set while the volume is mounted (as on Linux), so no commit has to write
	   the superblock outside the journal; ext4_unmount() clears it */
	put32(e->sb + 0x60, le32(e->sb + 0x60) | INCOMPAT_RECOVER);
	if ((erc = sb_write_direct(e, e->sb, true)))
		return erc;
	if (e->csum)
		put32(e->sb + 0x3FC, crc32c(~0U, e->sb, 0x3FC));
	e->journal = true;
	return ErcOk;
}

/* A clean unmount of every journaled ext4 volume: needs_recovery cleared (the journal is always
   empty between operations). Call it when no file operation is running; the volumes stay usable,
   and the next write sets the flag again. */
void ext4_unmount(void)
{
	U32 i;

	for (i = 0; i < next_ext; i++) {
		struct ext4 *e = &exts[i];

		if (!e->journal || tx.e == e)
			continue;
		put32(e->sb + 0x60, le32(e->sb + 0x60) & ~INCOMPAT_RECOVER);
		if (e->csum)
			put32(e->sb + 0x3FC, crc32c(~0U, e->sb, 0x3FC));
		sb_write_direct(e, e->sb, false);
		e->journal_clean = true;
	}
}

/* Write the open transaction (steps 1-4 above) and empty it. */
static erc_t tx_commit(void)
{
	struct ext4 *e = tx.e;
	U32 lblk = e->jfirst, i = 0, k;
	erc_t erc;

	if (!tx.n)
		return ErcOk;
	/* 1. descriptor(s) + blocks, then the commit block (needs_recovery has been set since mount) */
	while (i < tx.n) {
		U32 first = i, off = 12, last = 0;

		memset(b_jd, 0, e->bs);
		pbe32(b_jd, JBD2_MAGIC);
		pbe32(b_jd + 4, JBD2_DESCRIPTOR);
		pbe32(b_jd + 8, e->jseq);
		for (; i < tx.n; i++) {
			U32 need = e->jtag + (i == first ? 16 : 0), flags = i == first ? 0 : JF_SAME_UUID;

			if (off + need > e->bs)
				break;
			if (be32(tx.img + (U64)i * MAXBS) == JBD2_MAGIC)
				flags |= JF_ESCAPE;
			pbe32(b_jd + off, (U32)tx.blk[i]);
			b_jd[off + 6] = (U8)(flags >> 8);
			b_jd[off + 7] = (U8)flags;
			if (e->jincompat & JI_64BIT)
				pbe32(b_jd + off + 8, (U32)(tx.blk[i] >> 32));
			last = off;
			off += e->jtag;
			if (i == first) {
				memcpy(b_jd + off, e->juuid, 16);
				off += 16;
			}
		}
		b_jd[last + 7] |= JF_LAST_TAG;
		if ((erc = jwrite(e, lblk, b_jd)))
			return erc;
		lblk = jnext(e, lblk);
		for (k = first; k < i; k++) {
			const U8 *img = tx.img + (U64)k * MAXBS;

			if (be32(img) == JBD2_MAGIC) {		/* escaped: the replay restores the magic */
				memcpy(b_j, img, e->bs);
				pbe32(b_j, 0);
				img = b_j;
			}
			if ((erc = jwrite(e, lblk, img)))
				return erc;
			lblk = jnext(e, lblk);
		}
	}
	memset(b_jd, 0, e->bs);
	pbe32(b_jd, JBD2_MAGIC);
	pbe32(b_jd + 4, JBD2_COMMIT);
	pbe32(b_jd + 8, e->jseq);
	pbe32(b_jd + 0x34, unix_time());		/* h_commit_sec (low 32 bits of a be64) */
	if ((erc = jwrite(e, lblk, b_jd)))
		return erc;
	/* 2. committed: a crash from here on is repaired by the replay */
	if ((erc = jsb_write(e, e->jfirst)))
		return erc;
	if (ext4_commit_stop && ++commits == ext4_commit_stop) {	/* power-cut test */
		kprintf("PC: stopped inside commit %u (in the journal, not yet checkpointed)\n", commits);
		cli();
		for (;;)
			hlt();
	}
	/* 3. checkpoint: every block to its home (a torn write here is repaired by the replay) */
	for (k = 0; k < tx.n; k++)
		if ((erc = bwr(e, tx.blk[k], tx.img + (U64)k * MAXBS)))
			return erc;
	/* 4. the journal is empty again */
	e->jseq++;
	if ((erc = jsb_write(e, 0)))
		return erc;
	tx.n = 0;
	return ErcOk;
}

static void tx_begin(struct ext4 *e)
{
	if (e && e->journal) {
		if (e->journal_clean) {		/* after ext4_unmount: mounted for writing again */
			put32(e->sb + 0x60, le32(e->sb + 0x60) | INCOMPAT_RECOVER);
			if (e->csum)
				put32(e->sb + 0x3FC, crc32c(~0U, e->sb, 0x3FC));
			sb_write_direct(e, e->sb, true);
			e->journal_clean = false;
		}
		tx.e = e;
		tx.n = 0;
	}
}

static erc_t tx_end(struct ext4 *e, erc_t erc)
{
	erc_t c;

	if (!e || tx.e != e)
		return erc;
	c = tx_commit();
	tx.e = NULL;
	tx.n = 0;
	return erc ? erc : c;
}

/* The modifying operations, one transaction each. */
static erc_t j_create(const char *path, U32 len, U8 attr, struct fnode *n)
{
	struct ext4 *e = of(path, len);

	tx_begin(e);
	return tx_end(e, e4_create(path, len, attr, n));
}

static erc_t j_remove(struct fnode *n)
{
	struct ext4 *e = n->vol->ext4;

	tx_begin(e);
	return tx_end(e, e4_remove(n));
}

static erc_t j_rename(struct fnode *n, const char *newpath, U32 len)
{
	struct ext4 *e = n->vol->ext4;

	tx_begin(e);
	return tx_end(e, e4_rename(n, newpath, len));
}

static erc_t j_write(struct fnode *n, U64 off, const U8 *buf, U64 len, U64 *done)
{
	struct ext4 *e = n->vol->ext4;

	tx_begin(e);
	return tx_end(e, e4_write(n, off, buf, len, done));
}

static erc_t j_truncate(struct fnode *n, U64 size)
{
	struct ext4 *e = n->vol->ext4;

	tx_begin(e);
	return tx_end(e, e4_truncate(n, size));
}

const struct fs_ops ext4_ops = {
	.name = "ext4",
	.lookup = e4_lookup,
	.create = j_create,
	.remove = j_remove,
	.rename = j_rename,
	.read = e4_read,
	.write = j_write,
	.truncate = j_truncate,
	.readdir = e4_readdir,
	.dir_empty = e4_dir_empty,
	.dir_sector = NULL,
};

/* ---- mount ---- */

erc_t ext4_mount(struct volume *v)
{
	struct ext4 *e;
	U8 *sb;
	U32 log, rev;
	U8 root[512];
	erc_t erc;
	bool ro = false;

	if (next_ext == MAX_VOLS)
		return ErcNotSupported;
	e = &exts[next_ext];
	memset(e, 0, sizeof(*e));
	e->vol = v;
	e->dev = v->dev;
	e->base = v->start;
	sb = e->sb;
	if ((erc = bc_read(e->dev, e->base + 2, sb)) || (erc = bc_read(e->dev, e->base + 3, sb + 512)))
		return erc;
	if (le16(sb + 0x38) != EXT4_MAGIC)
		return ErcNotSupported;
	log = le32(sb + 0x18);
	if (log > 2) {
		kprintf("ext4 on %c: block size %u not supported (max 4096)\n", v->letter, 1024 << log);
		return ErcNotSupported;
	}
	e->bs = 1024U << log;
	e->spb = e->bs / 512;
	e->incompat = le32(sb + 0x60);
	e->ro_compat = le32(sb + 0x64);
	if (e->incompat & ~INCOMPAT_OK) {
		kprintf("ext4 on %c: unsupported features %x, not mounted\n", v->letter,
			e->incompat & ~INCOMPAT_OK);
		return ErcNotSupported;
	}
	e->csum = e->ro_compat & RO_METADATA_CSUM;
	if (e->csum && crc32c(~0U, sb, 0x3FC) != le32(sb + 0x3FC)) {
		kprintf("ext4 on %c: superblock checksum mismatch\n", v->letter);
		return ErcBrokenFile;
	}
	rev = le32(sb + 0x4C);
	e->ninodes = le32(sb + 0x00);
	e->nblocks = le32(sb + 0x04) | ((e->incompat & INCOMPAT_64BIT) ? (U64)le32(sb + 0x150) << 32 : 0);
	e->first_data = le32(sb + 0x14);
	e->bpg = le32(sb + 0x20);
	e->ipg = le32(sb + 0x28);
	e->isize = rev ? le16(sb + 0x58) : 128;
	e->first_ino = rev ? le32(sb + 0x54) : 11;
	e->dsize = (e->incompat & INCOMPAT_64BIT) ? le16(sb + 0xFE) : 32;
	if (!e->bpg || !e->ipg || e->isize > 512 || 512 % e->isize || e->dsize < 32 || e->dsize > 64 ||
	    e->nblocks * e->spb > v->nsectors)
		return ErcBrokenFile;
	e->ngroups = (U32)((e->nblocks - e->first_data + e->bpg - 1) / e->bpg);
	e->gdt = e->first_data + 1;
	e->seed = (e->incompat & INCOMPAT_CSUM_SEED) ? le32(sb + 0x270) : crc32c(~0U, sb + 0x68, 16);
	memcpy(v->label, sb + 0x78, 16);
	v->label[16] = 0;

	if ((erc = journal_mount(e, &ro))) {	/* replays a journal that needs it */
		kprintf("ext4 on %c: journal: error %u, mounted read-only\n", v->letter, erc);
		ro = true;
		e->journal = false;
	}
	if ((e->ro_compat & ~RO_RW_OK) || ((e->ro_compat & RO_GDT_CSUM) && !e->csum))
		ro = true;			/* unknown read-only features / crc16 descriptors */
	if (!(e->incompat & INCOMPAT_EXTENTS))
		ro = true;

	v->ext4 = e;
	next_ext++;
	if ((erc = iget(e, ROOT_INO, root)) || (i_mode(root) & S_IFMT) != S_IFDIR) {
		v->ext4 = NULL;
		next_ext--;
		return erc ? erc : ErcBrokenFile;
	}
	v->ops = &ext4_ops;
	v->ro = ro;
	v->kind = VOL_EXT4;
	if (ro)
		kprintf("ext4 on %c: mounted read-only (features %x/%x%s)\n", v->letter, e->incompat,
			e->ro_compat, (e->incompat & INCOMPAT_RECOVER) ? ", journal needs recovery" : "");
	return ErcOk;
}
