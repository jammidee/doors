/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* FAT12/16/32 with VFAT long names (replaces MMURTL's FSYS.C, which did FAT12/16 with 8.3 names).

   Called only by the FILESYS service task, so there is no locking here. All I/O goes through the
   write-through block cache; every change (FAT entries in all copies, directory entries, data)
   is on disk when the call returns.

   Fixed relative to the 32-bit baseline (docs/SMOKE_TEST.md):
   - timestamps come from the RTC (MMURTL's GetFATTime was an empty stub, dates were garbage);
   - writes are byte exact, so a copy keeps the last partial block (MMURTL copy truncated it);
   - DeleteDir works (MMURTL's hung the machine);
   - names match case-insensitively, long names included.

   FAT type is decided by the cluster count, as the Microsoft specification says
   (< 4085 FAT12, < 65525 FAT16, else FAT32). Long names are stored as UCS-2; characters outside
   ASCII read back as '?'. */

#include <doors/blk.h>
#include <doors/console.h>
#include <doors/fs.h>
#include <doors/string.h>

struct fat {
	struct volume *vol;
	struct blkdev *dev;
	U64 base;				/* partition start */
	U32 type;				/* 12, 16 or 32 */
	U32 spc, rsvd, nfats, fatsz, rootents, totsec, nclus, root_clus, hint, fsinfo;
	U64 fat_lba, root_lba, data_lba;	/* relative to base */
	bool fsinfo_done;
};

struct dirent83 {
	U8 name[11];
	U8 attr, ntres, crt_tenth;
	U16 crt_time, crt_date, acc_date, clus_hi, wrt_time, wrt_date, clus_lo;
	U32 size;
} PACKED;

static struct fat fats[MAX_VOLS];
static U32 nfat;

/* ---- sectors ---- */

static erc_t rd(struct fat *f, U64 lba, U8 *b)       { return bc_read(f->dev, f->base + lba, b); }
static erc_t wr(struct fat *f, U64 lba, const U8 *b) { return bc_write(f->dev, f->base + lba, b); }

static U16 le16(const U8 *p) { return (U16)(p[0] | (p[1] << 8)); }
static U32 le32(const U8 *p) { return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24); }

bool fat_bpb_ok(const U8 *s)
{
	U8 spc = s[13];

	return le16(s + 11) == 512 && spc && !(spc & (spc - 1)) && le16(s + 14) >= 1 &&
	       (s[16] == 1 || s[16] == 2) && s[21] >= 0xF0 && (le16(s + 19) || le32(s + 32)) &&
	       (le16(s + 22) || le32(s + 36));
}

erc_t fat_mount(struct volume *v)
{
	struct fat *f;
	U8 s[512];
	U32 rootsecs;
	erc_t erc;

	if (nfat == MAX_VOLS)
		return ErcNotSupported;
	f = &fats[nfat];
	memset(f, 0, sizeof(*f));
	f->vol = v;
	f->dev = v->dev;
	f->base = v->start;
	if ((erc = rd(f, 0, s)))
		return erc;
	if (!fat_bpb_ok(s))
		return ErcNotSupported;
	f->spc = s[13];
	f->rsvd = le16(s + 14);
	f->nfats = s[16];
	f->rootents = le16(s + 17);
	f->totsec = le16(s + 19) ? le16(s + 19) : le32(s + 32);
	f->fatsz = le16(s + 22) ? le16(s + 22) : le32(s + 36);
	rootsecs = (f->rootents * 32 + 511) / 512;
	f->fat_lba = f->rsvd;
	f->root_lba = f->rsvd + (U64)f->nfats * f->fatsz;
	f->data_lba = f->root_lba + rootsecs;
	if (f->totsec > v->nsectors || f->data_lba >= f->totsec)
		return ErcBadFATClstr;
	f->nclus = (U32)((f->totsec - f->data_lba) / f->spc);
	f->type = f->nclus < 4085 ? 12 : f->nclus < 65525 ? 16 : 32;
	if (f->type == 32) {
		f->root_clus = le32(s + 44);
		f->fsinfo = le16(s + 48);
		if (s[66] == 0x29)
			memcpy(v->label, s + 71, 11);
	} else if (s[38] == 0x29) {
		memcpy(v->label, s + 43, 11);
	}
	for (rootsecs = 11; rootsecs > 0 && v->label[rootsecs - 1] == ' '; rootsecs--)
		v->label[rootsecs - 1] = 0;
	f->hint = 2;
	v->fat = f;
	v->ops = &fat_ops;
	v->kind = f->type == 12 ? VOL_FAT12 : f->type == 16 ? VOL_FAT16 : VOL_FAT32;
	nfat++;
	return ErcOk;
}

/* ---- the FAT ---- */

static U32 eoc_min(struct fat *f) { return f->type == 12 ? 0xFF8 : f->type == 16 ? 0xFFF8 : 0x0FFFFFF8; }
static U32 eoc_val(struct fat *f) { return f->type == 12 ? 0xFFF : f->type == 16 ? 0xFFFF : 0x0FFFFFFF; }
static bool valid_clus(struct fat *f, U32 c) { return c >= 2 && c < f->nclus + 2; }
static U64 clus_lba(struct fat *f, U32 c) { return f->data_lba + (U64)(c - 2) * f->spc; }

static erc_t fat_get(struct fat *f, U32 c, U32 *v)
{
	U8 b[512], b2[512];
	U64 off;
	U32 o;
	erc_t erc;

	if (f->type == 12) {
		off = c + c / 2;
		o = (U32)(off % 512);
		if ((erc = rd(f, f->fat_lba + off / 512, b)))
			return erc;
		if (o == 511) {
			if ((erc = rd(f, f->fat_lba + off / 512 + 1, b2)))
				return erc;
			*v = b[511] | (b2[0] << 8);
		} else {
			*v = b[o] | (b[o + 1] << 8);
		}
		*v = (c & 1) ? *v >> 4 : *v & 0xFFF;
	} else if (f->type == 16) {
		off = (U64)c * 2;
		if ((erc = rd(f, f->fat_lba + off / 512, b)))
			return erc;
		*v = le16(b + off % 512);
	} else {
		off = (U64)c * 4;
		if ((erc = rd(f, f->fat_lba + off / 512, b)))
			return erc;
		*v = le32(b + off % 512) & 0x0FFFFFFF;
	}
	return ErcOk;
}

/* FAT32: the free count in FSInfo isn't maintained; mark it "unknown" before the first change. */
static void fsinfo_unknown(struct fat *f)
{
	U8 b[512];

	if (f->type != 32 || f->fsinfo_done || !f->fsinfo || rd(f, f->fsinfo, b))
		return;
	f->fsinfo_done = true;
	if (le32(b) != 0x41615252 || le32(b + 484) != 0x61417272)
		return;
	memset(b + 488, 0xFF, 8);
	wr(f, f->fsinfo, b);
}

static erc_t put_byte_pair(struct fat *f, U64 lba, U32 o, U16 mask_lo, U16 val)
{
	U8 b[512], b2[512];
	erc_t erc;
	U16 cur;

	if ((erc = rd(f, lba, b)))
		return erc;
	if (o == 511) {
		if ((erc = rd(f, lba + 1, b2)))
			return erc;
		cur = (U16)(b[511] | (b2[0] << 8));
		cur = (U16)((cur & mask_lo) | val);
		b[511] = (U8)cur;
		b2[0] = (U8)(cur >> 8);
		if ((erc = wr(f, lba, b)))
			return erc;
		return wr(f, lba + 1, b2);
	}
	cur = (U16)(b[o] | (b[o + 1] << 8));
	cur = (U16)((cur & mask_lo) | val);
	b[o] = (U8)cur;
	b[o + 1] = (U8)(cur >> 8);
	return wr(f, lba, b);
}

static erc_t fat_put(struct fat *f, U32 c, U32 v)
{
	U8 b[512];
	U32 k;
	erc_t erc;

	fsinfo_unknown(f);
	for (k = 0; k < f->nfats; k++) {
		U64 fl = f->fat_lba + (U64)k * f->fatsz, off;

		if (f->type == 12) {
			off = c + c / 2;
			if (c & 1)
				erc = put_byte_pair(f, fl + off / 512, (U32)(off % 512), 0x000F,
						    (U16)((v & 0xFFF) << 4));
			else
				erc = put_byte_pair(f, fl + off / 512, (U32)(off % 512), 0xF000,
						    (U16)(v & 0xFFF));
			if (erc)
				return erc;
			continue;
		}
		off = (U64)c * (f->type == 16 ? 2 : 4);
		if ((erc = rd(f, fl + off / 512, b)))
			return erc;
		if (f->type == 16) {
			b[off % 512] = (U8)v;
			b[off % 512 + 1] = (U8)(v >> 8);
		} else {
			U32 old = le32(b + off % 512);

			v = (old & 0xF0000000) | (v & 0x0FFFFFFF);
			b[off % 512] = (U8)v;
			b[off % 512 + 1] = (U8)(v >> 8);
			b[off % 512 + 2] = (U8)(v >> 16);
			b[off % 512 + 3] = (U8)(v >> 24);
		}
		if ((erc = wr(f, fl + off / 512, b)))
			return erc;
	}
	return ErcOk;
}

static erc_t zero_clus(struct fat *f, U32 c)
{
	static const U8 z[512];
	U32 i;
	erc_t erc;

	for (i = 0; i < f->spc; i++)
		if ((erc = wr(f, clus_lba(f, c) + i, z)))
			return erc;
	return ErcOk;
}

/* Allocate one cluster, link it after prev (0 = none). */
static erc_t alloc_clus(struct fat *f, U32 prev, U32 *out, bool zero)
{
	U32 i, c, v;
	erc_t erc;

	for (i = 0; i < f->nclus; i++) {
		c = 2 + (f->hint - 2 + i) % f->nclus;
		if ((erc = fat_get(f, c, &v)))
			return erc;
		if (v)
			continue;
		if ((erc = fat_put(f, c, eoc_val(f))))
			return erc;
		if (prev && (erc = fat_put(f, prev, c)))
			return erc;
		if (zero && (erc = zero_clus(f, c)))
			return erc;
		f->hint = c + 1 < f->nclus + 2 ? c + 1 : 2;
		*out = c;
		return ErcOk;
	}
	return ErcDiskFull;
}

static erc_t free_chain(struct fat *f, U32 c)
{
	U32 next, n = 0;
	erc_t erc;

	while (valid_clus(f, c) && n++ <= f->nclus) {
		if ((erc = fat_get(f, c, &next)) || (erc = fat_put(f, c, 0)))
			return erc;
		c = next;
	}
	return ErcOk;
}

/* The cluster `index` clusters into the chain starting at c. */
static erc_t chain_at(struct fat *f, U32 c, U64 index, U32 *out)
{
	U32 next;
	erc_t erc;

	while (index--) {
		if ((erc = fat_get(f, c, &next)))
			return erc;
		if (!valid_clus(f, next))
			return ErcBrokenFile;
		c = next;
	}
	*out = c;
	return ErcOk;
}

/* ---- directories (identified by their first cluster; 0 = FAT12/16 fixed root) ---- */

static erc_t dir_loc(struct fat *f, U32 dclus, U32 idx, U64 *lba, U32 *off, bool extend)
{
	U32 per = f->spc * 16, n, c = dclus, next;
	erc_t erc;

	if (!dclus) {
		if (idx >= f->rootents)
			return extend ? ErcRootFull : ErcNoMatch;
		*lba = f->root_lba + idx / 16;
		*off = (idx % 16) * 32;
		return ErcOk;
	}
	for (n = idx / per; n; n--) {
		if ((erc = fat_get(f, c, &next)))
			return erc;
		if (!valid_clus(f, next)) {
			if (!extend)
				return ErcNoMatch;
			if ((erc = alloc_clus(f, c, &next, true)))
				return erc;
		}
		c = next;
	}
	*lba = clus_lba(f, c) + (idx % per) / 16;
	*off = (idx % 16) * 32;
	return ErcOk;
}

static erc_t dir_read(struct fat *f, U32 dclus, U32 idx, U8 *e)
{
	U8 b[512];
	U64 lba;
	U32 off;
	erc_t erc;

	if ((erc = dir_loc(f, dclus, idx, &lba, &off, false)) || (erc = rd(f, lba, b)))
		return erc;
	memcpy(e, b + off, 32);
	return ErcOk;
}

static erc_t dir_write(struct fat *f, U32 dclus, U32 idx, const U8 *e)
{
	U8 b[512];
	U64 lba;
	U32 off;
	erc_t erc;

	if ((erc = dir_loc(f, dclus, idx, &lba, &off, true)) || (erc = rd(f, lba, b)))
		return erc;
	memcpy(b + off, e, 32);
	return wr(f, lba, b);
}

/* ---- names ---- */

static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static bool ieq(const char *a, U32 an, const char *b, U32 bn)
{
	U32 i;

	if (an != bn)
		return false;
	for (i = 0; i < an; i++)
		if (up(a[i]) != up(b[i]))
			return false;
	return true;
}

static U32 short_name(const U8 *n11, char *out)		/* "NAME.EXT" */
{
	U32 i, k = 0, e;

	for (i = 0; i < 8 && n11[i] != ' '; i++)
		out[k++] = (char)n11[i];
	for (e = 8; e < 11 && n11[e] != ' '; e++)
		;
	if (e > 8) {
		out[k++] = '.';
		for (i = 8; i < e; i++)
			out[k++] = (char)n11[i];
	}
	out[k] = 0;
	if (k && (U8)out[0] == 0x05)
		out[0] = (char)0xE5;
	return k;
}

static U8 lfn_sum(const U8 *n11)
{
	U8 s = 0;
	int i;

	for (i = 0; i < 11; i++)
		s = (U8)(((s & 1) << 7) + (s >> 1) + n11[i]);
	return s;
}

static const U8 lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static const char *strchr_(const char *s, char c)
{
	for (; *s; s++)
		if (*s == c)
			return s;
	return NULL;
}

static bool ok83char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (U8)c >= 128 ||
	       (c && strchr_("!#$%&'()-@^_`{}~", c));
}

/* Can the name be stored as a plain 8.3 entry (upper case, valid characters)? */
static bool make83(const char *s, U32 n, U8 *out)
{
	U32 i, dot = n, b = 0, e = 0;

	if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
		return false;
	for (i = 0; i < n; i++)
		if (s[i] == '.') {
			if (dot != n)
				return false;
			dot = i;
		}
	memset(out, ' ', 11);
	for (i = 0; i < dot; i++, b++) {
		if (b == 8 || !ok83char(s[i]))
			return false;
		out[b] = (U8)s[i];
	}
	for (i = dot + 1; i < n; i++, e++) {
		if (e == 3 || !ok83char(s[i]))
			return false;
		out[8 + e] = (U8)s[i];
	}
	return b > 0 && !(dot != n && e == 0);
}

/* ---- lookup ---- */

/* FAT date/time -> Unix time. */
static U32 fat2unix(U16 date, U16 time)
{
	U32 m = (date >> 5) & 15, d = date & 31;

	if (!m || !d)
		return 0;
	return days_from_civil(1980 + (date >> 9), m, d) * 86400U + (time >> 11) * 3600U +
	       ((time >> 5) & 63) * 60U + (time & 31) * 2U;
}

static void fill_node(struct fat *f, struct fnode *nd, U32 dclus, U32 idx, U32 lfn_start, U32 lfn_cnt,
		      const U8 *e)
{
	const struct dirent83 *d = (const struct dirent83 *)e;

	memset(nd, 0, sizeof(*nd));
	nd->vol = f->vol;
	nd->key = ((U64)dclus << 32) | idx;
	nd->dir_clus = dclus;
	nd->idx = idx;
	nd->lfn_idx = lfn_start;
	nd->lfn_n = lfn_cnt;
	nd->first = d->clus_lo | (f->type == 32 ? (U32)d->clus_hi << 16 : 0);
	nd->size = d->size;
	nd->attr = d->attr;
	nd->mtime = fat2unix(d->wrt_date, d->wrt_time);
}

/* Walk a directory: for every live entry (not '.', '..', deleted, volume label) call fn with its
   long name (if any, else the 8.3 name) and its node. Stops when fn returns true. */
typedef bool (*dir_fn)(void *ctx, const char *lname, U32 llen, const char *sname, U32 slen,
		       const struct fnode *nd);

static erc_t dir_walk(struct fat *f, U32 dclus, bool dots, dir_fn fn, void *ctx)
{
	U8 e[32], chk = 0;
	char lfn[264], sn[16];
	U32 idx, lfn_start = 0, lfn_cnt = 0, lfn_len = 0;
	bool lfn_ok = false;
	erc_t erc;

	for (idx = 0;; idx++) {
		erc = dir_read(f, dclus, idx, e);
		if (erc == ErcNoMatch)
			break;
		if (erc)
			return erc;
		if (e[0] == 0)
			break;
		if (e[0] == 0xE5) {
			lfn_ok = false;
			continue;
		}
		if ((e[11] & 0x3F) == ATTR_LFN) {
			U32 ord = e[0] & 0x3F, k;

			if (e[0] & 0x40) {
				lfn_ok = ord >= 1 && ord <= 20;
				lfn_cnt = ord;
				lfn_start = idx;
				chk = e[13];
				lfn_len = ord * 13;
				memset(lfn, 0, sizeof(lfn));
			}
			if (!lfn_ok || e[13] != chk || ord < 1 || ord > lfn_cnt)
				continue;
			for (k = 0; k < 13; k++) {
				U16 u = le16(e + lfn_pos[k]);
				U32 at = (ord - 1) * 13 + k;

				if (u == 0x0000 && at < lfn_len)
					lfn_len = at;
				if (at < sizeof(lfn) - 1 && u != 0x0000 && u != 0xFFFF)
					lfn[at] = u < 128 ? (char)u : '?';
			}
			continue;
		}
		if (e[11] & ATTR_VOLUME) {
			lfn_ok = false;
			continue;
		}
		{
			U32 sl = short_name(e, sn);
			bool long_ok = lfn_ok && lfn_sum(e) == chk && idx - lfn_start == lfn_cnt;
			bool dot = (sl == 1 && sn[0] == '.') || (sl == 2 && sn[0] == '.' && sn[1] == '.');
			struct fnode nd;

			lfn_ok = false;
			if (dot && !dots)
				continue;
			fill_node(f, &nd, dclus, idx, long_ok ? lfn_start : 0, long_ok ? lfn_cnt : 0, e);
			if (fn(ctx, long_ok ? lfn : sn, long_ok ? lfn_len : sl, sn, sl, &nd))
				return ErcOk;
		}
	}
	return ErcNoMatch;
}

struct find_ctx {
	const char *name;
	U32 nlen;
	struct fnode *out;
	bool found;
};

static bool find_fn(void *c, const char *ln, U32 ll, const char *sn, U32 sl, const struct fnode *nd)
{
	struct find_ctx *x = c;

	if (!ieq(ln, ll, x->name, x->nlen) && !ieq(sn, sl, x->name, x->nlen))
		return false;
	*x->out = *nd;
	x->found = true;
	return true;
}

static erc_t dir_find(struct fat *f, U32 dclus, const char *name, U32 nlen, struct fnode *nd)
{
	struct find_ctx x = { name, nlen, nd, false };
	erc_t erc = dir_walk(f, dclus, true, find_fn, &x);

	if (erc && erc != ErcNoMatch)
		return erc;
	return x.found ? ErcOk : ErcNoSuchFile;
}

static void root_node(struct fat *f, struct fnode *nd)
{
	memset(nd, 0, sizeof(*nd));
	nd->vol = f->vol;
	nd->key = ~0UL;
	nd->first = f->type == 32 ? f->root_clus : 0;
	nd->attr = ATTR_DIR;
	nd->is_root = true;
}

/* The directory a node's children live in (its first cluster; '..' to the root gives 0). */
static U32 child_dir(struct fat *f, const struct fnode *nd)
{
	if (nd->is_root || (!nd->first && f->type == 32))
		return f->type == 32 ? f->root_clus : 0;
	return nd->first;
}

/* Resolve "X:\a\b\c". parent: stop at the directory holding the last component and return
   that component in *last / *lastlen. */
static erc_t resolve(const char *path, U32 len, bool parent, struct fnode *nd,
		     const char **last, U32 *lastlen)
{
	struct volume *v;
	struct fat *f;
	U32 i = 2, s;

	if (len < 2 || path[1] != ':')
		return ErcBadFileSpec;
	v = vol_get(path[0]);
	if (!v)
		return ErcNoSuchDrive;
	if (!v->fat)
		return ErcNotSupported;
	f = v->fat;
	root_node(f, nd);
	if (last) {
		*last = path + len;
		*lastlen = 0;
	}
	for (;;) {
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
			if (j >= len) {			/* this is the last component */
				*last = path + s;
				*lastlen = i - s;
				return (i - s) > 255 ? ErcBadFileSpec : ErcOk;
			}
		}
		if (!(nd->attr & ATTR_DIR))
			return ErcNoSuchDir;
		{
			struct fnode child;
			erc_t erc = dir_find(f, child_dir(f, nd), path + s, i - s, &child);

			if (erc == ErcNoSuchFile) {
				U32 j = i;

				while (j < len && (path[j] == '\\' || path[j] == '/'))
					j++;
				return j >= len ? ErcNoSuchFile : ErcNoSuchDir;
			}
			if (erc)
				return erc;
			if (!child.first && (child.attr & ATTR_DIR))	/* '..' to the root */
				root_node(f, &child);
			*nd = child;
		}
	}
}

erc_t fat_lookup(const char *path, U32 len, struct fnode *n)
{
	return resolve(path, len, false, n, NULL, NULL);
}

/* ---- creating entries ---- */

/* FAT time/date from the RTC: (date << 16) | time. */
static U32 now32(void)
{
	struct rtc_time t;
	U16 time, date;

	rtc_read(&t);
	time = (U16)((t.hour << 11) | (t.min << 5) | (t.sec / 2));
	date = (U16)(((t.year > 1980 ? t.year - 1980 : 0) << 9) | (t.month << 5) | t.day);
	return ((U32)date << 16) | time;
}

/* Assigns into (packed) directory entry fields, which can't be passed by address. */
#define NOW(T, D) do { U32 now_ = now32(); (T) = (U16)now_; (D) = (U16)(now_ >> 16); } while (0)

/* First index of `count` consecutive free entries (may be past the current end). */
static erc_t find_free(struct fat *f, U32 dclus, U32 count, U32 *start)
{
	U8 e[32];
	U32 idx, run = 0;
	erc_t erc;

	for (idx = 0;; idx++) {
		erc = dir_read(f, dclus, idx, e);
		if (erc == ErcNoMatch || (!erc && e[0] == 0)) {	/* end: everything after is free */
			*start = run ? idx - run : idx;
			if (!dclus && *start + count > f->rootents)
				return ErcRootFull;
			return ErcOk;
		}
		if (erc)
			return erc;
		if (e[0] == 0xE5) {
			if (++run == count) {
				*start = idx - run + 1;
				return ErcOk;
			}
		} else {
			run = 0;
		}
	}
}

/* Short alias for a long name: up to 6 characters + "~N" + extension. */
static erc_t make_alias(struct fat *f, U32 dclus, const char *s, U32 n, U8 *out)
{
	U32 i, dot = n, b = 0, e = 0, k;
	char probe[16];
	struct fnode tmp;

	for (i = n; i > 0; i--)
		if (s[i - 1] == '.') {
			dot = i - 1;
			break;
		}
	memset(out, ' ', 11);
	for (i = 0; i < dot && b < 6; i++) {
		char c = up(s[i]);

		if (c == ' ' || c == '.')
			continue;
		out[b++] = (U8)(ok83char(c) ? c : '_');
	}
	if (!b)
		out[b++] = '_';
	for (i = dot + 1; i < n && e < 3; i++) {
		char c = up(s[i]);

		if (c != ' ' && c != '.')
			out[8 + e++] = (U8)(ok83char(c) ? c : '_');
	}
	for (k = 1; k < 100; k++) {
		U32 tl = k < 10 ? 2 : 3, at = b + tl > 8 ? 8 - tl : b;

		out[at] = '~';
		if (k < 10) {
			out[at + 1] = (U8)('0' + k);
		} else {
			out[at + 1] = (U8)('0' + k / 10);
			out[at + 2] = (U8)('0' + k % 10);
		}
		if (dir_find(f, dclus, probe, short_name(out, probe), &tmp) == ErcNoSuchFile)
			return ErcOk;
		for (i = at; i < 8; i++)
			out[i] = i < b ? out[i] : ' ';
	}
	return ErcDupName;
}

/* Write the entries for `name` in directory dclus: long-name entries (if needed) + the 8.3 entry
   with the given attributes, first cluster and size. Fills nd. */
static erc_t add_entry(struct fat *f, U32 dclus, const char *name, U32 nlen, U8 attr, U32 first,
		       U64 size, const struct dirent83 *times, struct fnode *nd)
{
	U8 n11[11], e[32];
	U32 nlfn = 0, start, i, k;
	struct dirent83 d;
	erc_t erc;

	if (!make83(name, nlen, n11)) {
		if ((erc = make_alias(f, dclus, name, nlen, n11)))
			return erc;
		nlfn = (nlen + 12) / 13;
	}
	if ((erc = find_free(f, dclus, nlfn + 1, &start)))
		return erc;
	for (i = 0; i < nlfn; i++) {
		U32 ord = nlfn - i;

		memset(e, 0, 32);
		e[0] = (U8)(ord | (i == 0 ? 0x40 : 0));
		e[11] = ATTR_LFN;
		e[13] = lfn_sum(n11);
		for (k = 0; k < 13; k++) {
			U32 at = (ord - 1) * 13 + k;
			U16 u = at < nlen ? (U8)name[at] : at == nlen ? 0x0000 : 0xFFFF;

			e[lfn_pos[k]] = (U8)u;
			e[lfn_pos[k] + 1] = (U8)(u >> 8);
		}
		if ((erc = dir_write(f, dclus, start + i, e)))
			return erc;
	}
	memset(&d, 0, sizeof(d));
	memcpy(d.name, n11, 11);
	if (d.name[0] == 0xE5)
		d.name[0] = 0x05;
	d.attr = attr;
	if (times) {
		d.crt_time = times->crt_time;
		d.crt_date = times->crt_date;
	} else {
		NOW(d.crt_time, d.crt_date);
	}
	NOW(d.wrt_time, d.wrt_date);
	d.acc_date = d.wrt_date;
	d.clus_lo = (U16)first;
	d.clus_hi = f->type == 32 ? (U16)(first >> 16) : 0;
	d.size = (U32)size;
	if ((erc = dir_write(f, dclus, start + nlfn, (const U8 *)&d)))
		return erc;

	memset(nd, 0, sizeof(*nd));
	nd->vol = f->vol;
	nd->key = ((U64)dclus << 32) | (start + nlfn);
	nd->dir_clus = dclus;
	nd->idx = start + nlfn;
	nd->lfn_idx = start;
	nd->lfn_n = nlfn;
	nd->first = first;
	nd->size = size;
	nd->attr = attr;
	nd->mtime = fat2unix(d.wrt_date, d.wrt_time);
	return ErcOk;
}

static erc_t mark_deleted(struct fat *f, const struct fnode *nd)
{
	U8 e[32];
	U32 i;
	erc_t erc;

	for (i = 0; i <= nd->lfn_n; i++) {
		U32 idx = i < nd->lfn_n ? nd->lfn_idx + i : nd->idx;

		if ((erc = dir_read(f, nd->dir_clus, idx, e)))
			return erc;
		e[0] = 0xE5;
		if ((erc = dir_write(f, nd->dir_clus, idx, e)))
			return erc;
	}
	return ErcOk;
}

static erc_t set_dotdot(struct fat *f, U32 dirclus, U32 parent)
{
	U8 e[32];
	erc_t erc;

	if ((erc = dir_read(f, dirclus, 1, e)))
		return erc;
	if (memcmp(e, "..         ", 11))
		return ErcBadFATClstr;
	e[26] = (U8)parent;
	e[27] = (U8)(parent >> 8);
	e[20] = (U8)(parent >> 16);
	e[21] = (U8)(parent >> 24);
	return dir_write(f, dirclus, 1, e);
}

erc_t fat_create(const char *path, U32 len, U8 attr, struct fnode *n)
{
	struct fnode dir, tmp;
	const char *name;
	U32 nlen, first = 0, dclus;
	struct fat *f;
	erc_t erc;

	if ((erc = resolve(path, len, true, &dir, &name, &nlen)))
		return erc;
	if (!nlen)
		return ErcBadFileSpec;
	if (!(dir.attr & ATTR_DIR))
		return ErcNoSuchDir;
	f = dir.vol->fat;
	dclus = child_dir(f, &dir);
	erc = dir_find(f, dclus, name, nlen, &tmp);
	if (erc == ErcOk)
		return ErcDupName;
	if (erc != ErcNoSuchFile)
		return erc;

	if (attr & ATTR_DIR) {				/* new directory: one cluster with . and .. */
		struct dirent83 d;

		if ((erc = alloc_clus(f, 0, &first, true)))
			return erc;
		memset(&d, 0, sizeof(d));
		memcpy(d.name, ".          ", 11);
		d.attr = ATTR_DIR;
		NOW(d.wrt_time, d.wrt_date);
		d.crt_time = d.wrt_time;
		d.crt_date = d.acc_date = d.wrt_date;
		d.clus_lo = (U16)first;
		d.clus_hi = f->type == 32 ? (U16)(first >> 16) : 0;
		if ((erc = dir_write(f, first, 0, (const U8 *)&d)))
			return erc;
		memcpy(d.name, "..         ", 11);
		dclus = dir.is_root ? 0 : dir.first;	/* the root is always 0 in '..' */
		d.clus_lo = (U16)dclus;
		d.clus_hi = f->type == 32 ? (U16)(dclus >> 16) : 0;
		if ((erc = dir_write(f, first, 1, (const U8 *)&d)))
			return erc;
		dclus = child_dir(f, &dir);
	}
	erc = add_entry(f, dclus, name, nlen, attr ? attr : ATTR_ARCH, first, 0, NULL, n);
	if (erc && first)
		free_chain(f, first);
	return erc;
}

erc_t fat_remove(struct fnode *n)
{
	struct fat *f = n->vol->fat;
	erc_t erc;

	if (n->is_root)
		return ErcBadFileSpec;
	if ((erc = mark_deleted(f, n)))
		return erc;
	return n->first ? free_chain(f, n->first) : ErcOk;
}

erc_t fat_rename(struct fnode *n, const char *newpath, U32 len)
{
	struct fat *f = n->vol->fat;
	struct fnode dir, tmp, moved;
	struct dirent83 old;
	const char *name;
	U32 nlen, dclus;
	erc_t erc;

	if (n->is_root)
		return ErcBadFileSpec;
	if ((erc = resolve(newpath, len, true, &dir, &name, &nlen)))
		return erc;
	if (dir.vol != n->vol)
		return ErcRenameDrv;
	if (!nlen || !(dir.attr & ATTR_DIR))
		return ErcBadFileSpec;
	dclus = child_dir(f, &dir);
	if (dir_find(f, dclus, name, nlen, &tmp) == ErcOk)
		return ErcDupName;
	if ((erc = dir_read(f, n->dir_clus, n->idx, (U8 *)&old)))
		return erc;
	if ((erc = add_entry(f, dclus, name, nlen, old.attr, n->first, n->size, &old, &moved)))
		return erc;
	if ((erc = mark_deleted(f, n)))
		return erc;
	if ((old.attr & ATTR_DIR) && n->first && dclus != n->dir_clus &&
	    (erc = set_dotdot(f, n->first, dir.is_root ? 0 : dir.first)))
		return erc;
	*n = moved;
	return ErcOk;
}

/* ---- file data ---- */

erc_t fat_update(struct fnode *n)
{
	struct fat *f = n->vol->fat;
	struct dirent83 d;
	erc_t erc;

	if (n->is_root)
		return ErcOk;
	if ((erc = dir_read(f, n->dir_clus, n->idx, (U8 *)&d)))
		return erc;
	d.clus_lo = (U16)n->first;
	d.clus_hi = f->type == 32 ? (U16)(n->first >> 16) : 0;
	d.size = (U32)n->size;
	NOW(d.wrt_time, d.wrt_date);
	d.acc_date = d.wrt_date;
	return dir_write(f, n->dir_clus, n->idx, (const U8 *)&d);
}

erc_t fat_read(struct fnode *n, U64 off, U8 *buf, U64 len, U64 *done)
{
	struct fat *f = n->vol->fat;
	U64 cs = (U64)f->spc * 512, pos;
	U32 c;
	U8 sec[512];
	erc_t erc;

	*done = 0;
	if (off >= n->size || !len)
		return ErcOk;
	if (len > n->size - off)
		len = n->size - off;
	if ((erc = chain_at(f, n->first, off / cs, &c)))
		return erc;
	pos = off;
	while (*done < len) {
		U64 in_clus = pos % cs, lba = clus_lba(f, c) + in_clus / 512;
		U32 in_sec = (U32)(pos % 512), k = 512 - in_sec;

		if (k > len - *done)
			k = (U32)(len - *done);
		if ((erc = rd(f, lba, sec)))
			return erc;
		memcpy(buf + *done, sec + in_sec, k);
		*done += k;
		pos += k;
		if (pos % cs == 0 && *done < len && (erc = chain_at(f, c, 1, &c)))
			return erc;
	}
	return ErcOk;
}

/* Make the chain hold at least `bytes` (new clusters zeroed when zero is set). */
static erc_t grow(struct fat *f, struct fnode *n, U64 bytes, bool zero)
{
	U64 cs = (U64)f->spc * 512, need = (bytes + cs - 1) / cs, have = 0;
	U32 c = n->first, next, last = 0;
	erc_t erc;

	if (!need)
		return ErcOk;
	while (valid_clus(f, c)) {
		have++;
		last = c;
		if ((erc = fat_get(f, c, &next)))
			return erc;
		if (next >= eoc_min(f) || !valid_clus(f, next))
			break;
		c = next;
	}
	while (have < need) {
		if ((erc = alloc_clus(f, last, &c, zero)))
			return erc;
		if (!n->first)
			n->first = c;
		last = c;
		have++;
	}
	return ErcOk;
}

erc_t fat_write(struct fnode *n, U64 off, const U8 *buf, U64 len, U64 *done)
{
	struct fat *f = n->vol->fat;
	U64 cs = (U64)f->spc * 512, pos;
	U32 c;
	U8 sec[512];
	erc_t erc;

	*done = 0;
	if (!len)
		return ErcOk;
	if (off + len > 0xFFFFFFFFUL)
		return ErcDiskFull;		/* FAT file size limit (4 GB - 1); ext4 lifts it (M8) */
	if (off > n->size && (erc = fat_truncate(n, off)))	/* writing past the end: zero the gap */
		return erc;
	if ((erc = grow(f, n, off + len, false)))
		return erc;
	if ((erc = chain_at(f, n->first, off / cs, &c)))
		return erc;
	pos = off;
	while (*done < len) {
		U64 lba = clus_lba(f, c) + (pos % cs) / 512;
		U32 in_sec = (U32)(pos % 512), k = 512 - in_sec;

		if (k > len - *done)
			k = (U32)(len - *done);
		if (k < 512 && (erc = rd(f, lba, sec)))
			return erc;
		memcpy(sec + in_sec, buf + *done, k);
		if (k < 512 && pos + k >= n->size && in_sec + k < 512 && pos + k > n->size)
			memset(sec + in_sec + k, 0, 512 - in_sec - k);	/* no stale bytes past EOF */
		if ((erc = wr(f, lba, sec)))
			return erc;
		*done += k;
		pos += k;
		if (pos > n->size)
			n->size = pos;
		if (pos % cs == 0 && *done < len && (erc = chain_at(f, c, 1, &c)))
			return erc;
	}
	return fat_update(n);
}

erc_t fat_truncate(struct fnode *n, U64 size)
{
	struct fat *f = n->vol->fat;
	U64 cs = (U64)f->spc * 512;
	erc_t erc;

	if (size > 0xFFFFFFFFUL)
		return ErcDiskFull;
	if (size < n->size) {
		U64 keep = (size + cs - 1) / cs;
		U32 c, next;

		if (!keep) {
			if ((erc = free_chain(f, n->first)))
				return erc;
			n->first = 0;
		} else {
			if ((erc = chain_at(f, n->first, keep - 1, &c)) || (erc = fat_get(f, c, &next)))
				return erc;
			if ((erc = fat_put(f, c, eoc_val(f))))
				return erc;
			if (valid_clus(f, next) && (erc = free_chain(f, next)))
				return erc;
		}
		n->size = size;
		return fat_update(n);
	}
	if (size > n->size) {			/* grow with zeros */
		static const U8 z[512];
		U64 pos = n->size, done;

		if ((erc = grow(f, n, size, true)))
			return erc;
		while (pos < size) {
			U64 k = size - pos < 512 - pos % 512 ? size - pos : 512 - pos % 512;
			U64 old = n->size;

			n->size = size;		/* let fat_write see the room */
			if ((erc = fat_write(n, pos, z, k, &done))) {
				n->size = old;
				return erc;
			}
			pos += k;
		}
		n->size = size;
		return fat_update(n);
	}
	return ErcOk;
}

/* ---- directory listing support ---- */

erc_t fat_dir_sector(struct fnode *dir, U32 sectnum, U8 *out)
{
	struct fat *f = dir->vol->fat;
	U32 dclus = child_dir(f, dir), c;
	erc_t erc;

	if (!(dir->attr & ATTR_DIR))
		return ErcNoSuchDir;
	if (!dclus) {
		if (sectnum >= (f->rootents * 32 + 511) / 512)
			return ErcNoMatch;
		return rd(f, f->root_lba + sectnum, out);
	}
	erc = chain_at(f, dclus, sectnum / f->spc, &c);
	if (erc == ErcBrokenFile)
		return ErcNoMatch;
	if (erc)
		return erc;
	return rd(f, clus_lba(f, c) + sectnum % f->spc, out);
}

bool fat_dir_empty(struct fnode *dir)
{
	struct fat *f = dir->vol->fat;
	U32 dclus = child_dir(f, dir), idx;
	U8 e[32];

	for (idx = 0; dir_read(f, dclus, idx, e) == ErcOk && e[0]; idx++) {
		if (e[0] == 0xE5 || (e[11] & 0x3F) == ATTR_LFN)
			continue;
		if (!memcmp(e, ".          ", 11) || !memcmp(e, "..         ", 11))
			continue;
		return false;
	}
	return true;
}

/* ---- ReadDir and the ops table ---- */

struct nth_ctx {
	U32 want, at;
	struct dir_entry *out;
	bool found;
};

static bool nth_fn(void *c, const char *ln, U32 ll, const char *sn, U32 sl, const struct fnode *nd)
{
	struct nth_ctx *x = c;

	(void)sn;
	(void)sl;
	if (x->at++ != x->want)
		return false;
	if (ll > sizeof(x->out->name) - 1)
		ll = sizeof(x->out->name) - 1;
	memcpy(x->out->name, ln, ll);
	x->out->name[ll] = 0;
	x->out->size = nd->size;
	x->out->attr = nd->attr;
	x->out->mtime = nd->mtime;
	x->found = true;
	return true;
}

static erc_t fat_readdir(struct fnode *dir, U32 index, struct dir_entry *out)
{
	struct fat *f = dir->vol->fat;
	struct nth_ctx x = { index, 0, out, false };
	erc_t erc;

	if (!(dir->attr & ATTR_DIR))
		return ErcNoSuchDir;
	erc = dir_walk(f, child_dir(f, dir), false, nth_fn, &x);
	if (erc && erc != ErcNoMatch)
		return erc;
	return x.found ? ErcOk : ErcNoMatch;
}

const struct fs_ops fat_ops = {
	.name = "FAT",
	.lookup = fat_lookup,
	.create = fat_create,
	.remove = fat_remove,
	.rename = fat_rename,
	.read = fat_read,
	.write = fat_write,
	.truncate = fat_truncate,
	.readdir = fat_readdir,
	.dir_empty = fat_dir_empty,
	.dir_sector = fat_dir_sector,
};
