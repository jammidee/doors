/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* fsfuzz: damaged-disk fuzzer for DOORS's file systems (ROADMAP step 1.4, `make fuzz`).

   The kernel's own volume, FAT, ext4, CRC32C and block-cache code (kernel/vol.c, fat.c, ext4.c,
   crc32c.c, bcache.c) is compiled into this Linux program unchanged, over a disk image held in
   memory. Each iteration forks; the child damages its copy of the image, then mounts every volume
   (vol_init: partitions, FAT and ext4 mounts, journal replay) and runs a workload through the
   fs_ops: walk every directory, look up and read every file, then (on writable volumes) create,
   write, grow, cut, rename and delete files and directories, also in damaged directories and
   files, and finish with ext4_unmount(). The FILESYS service makes the same checks first (read-only
   volumes, directories), so the calls here are the ones the kernel can make.

   A run passes when the child returns. A failure is any signal (crash, abort from panic(), a
   sanitizer report) or a hang (over --timeout seconds). Built twice by the Makefile:
     build/host/fsfuzz      AddressSanitizer + UndefinedBehaviorSanitizer: any bad memory access
     build/host/fsfuzz-stk  no sanitizers; the workload runs on a 12 KB stack with an unmapped guard
                            page below it, as the FILESYS task has 16 KB (less what serve() and an
                            interrupt need): a deep or runaway recursion faults here as it would
                            in the kernel.

   Where to damage: one clean run records every sector the code read (outside of file-data reads,
   which go to a second list); each iteration then damages 1-8 of those sectors (bit flips, bytes,
   16/32-bit fields set to edge values, bytes copied from another sector, runs of random bytes).
   Iteration i uses seed (seed, i), so a failure is reproduced with
     fsfuzz IMG --seed S --replay I [--dump OUT.img] [-v]
   which also writes the damaged image (to boot it in QEMU) and shows the kernel messages. */

#define _GNU_SOURCE
#include <doors/blk.h>
#include <doors/console.h>
#include <doors/fs.h>
#include <doors/mem.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef FSFUZZ_STACK
#define FSFUZZ_STACK 0			/* fsfuzz-stk: -DFSFUZZ_STACK=12288 */
#endif

/* the fuzzer keeps its lists until exit, and a child ends with _exit(): no leak checking */
const char *__asan_default_options(void);
const char *__asan_default_options(void) { return "detect_leaks=0"; }

/* ---- the kernel functions the file system modules call ---- */

static bool verbose;

int kprintf(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return 0;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	return 0;
}

void panic(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "PANIC: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	abort();
}

erc_t AllocOSPage(U32 nPages, U8 **ppMemRet)
{
	void *p = NULL;

	if (posix_memalign(&p, PAGE_SIZE, (size_t)nPages * PAGE_SIZE))
		return ErcNoMem;
	memset(p, 0, (size_t)nPages * PAGE_SIZE);
	*ppMemRet = p;
	return ErcOk;
}

U64 timer_ticks(void) { return 12345; }
U32 unix_time(void) { return 1790000000U; }

void rtc_read(struct rtc_time *t)
{
	t->year = 2026, t->month = 10, t->day = 18, t->hour = 12, t->min = 0, t->sec = 0;
}

U32 days_from_civil(U32 y, U32 m, U32 d)
{
	S32 yy = (S32)y - (m <= 2), era = (yy >= 0 ? yy : yy - 399) / 400;
	U32 yoe = (U32)(yy - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	U32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return (U32)(era * 146097 + (S32)doe - 719468);
}

/* ---- the disk: an image in memory ---- */

static U8 *img;				/* the child's copy (fork: copy-on-write) */
static U64 img_sectors;
static U8 *seen, *seen_data;		/* shared with the parent: sectors read (trace run) */
static bool in_data;			/* inside a file-data read: reads go to seen_data */

static erc_t ram_io(struct blkdev *d, U32 op, U64 lba, U32 n, U8 *buf)
{
	U64 i;

	(void)d;
	if (op == CmdRead) {
		memcpy(buf, img + lba * 512, (size_t)n * 512);
		if (seen)
			for (i = lba; i < lba + n; i++)
				(in_data ? seen_data : seen)[i >> 3] |= (U8)(1 << (i & 7));
	} else if (op == CmdWrite) {
		memcpy(img + lba * 512, buf, (size_t)n * 512);
	}
	return ErcOk;
}

static struct blkdev disk = { .devnum = DEV_HD0, .name = "fuzz disk", .io = ram_io };

struct blkdev *blk_get(U32 devnum) { return devnum == DEV_HD0 ? &disk : NULL; }

erc_t blk_io(struct blkdev *d, U32 op, U64 lba, U32 n, U8 *buf)	/* as kernel/device.c */
{
	if (op != CmdRead && op != CmdWrite && op != CmdVerify)
		return ErcBadOpNum;
	if (!n || lba >= d->nblocks || n > d->nblocks - lba)
		return ErcBeyondEOF;
	return d->io(d, op, lba, n, buf);
}

/* ---- results, shared with the parent ---- */

#define NERC 1024
struct shared {
	U64 erc[NERC];			/* how often each error code came back */
	U64 ops, mounted, rw;
};
static struct shared *sh;

static erc_t note(erc_t e)
{
	sh->erc[e < NERC ? e : NERC - 1]++;
	sh->ops++;
	return e;
}

/* ---- the workload ---- */

#define MAXDIRS  48
#define MAXFILES 48
#define MAXENT   200

struct path {
	char s[MAX_PATH];
	U32 n;
};

static bool join(struct path *out, const struct path *dir, const char *name)
{
	U32 l = (U32)strlen(name);

	if (dir->n + 1 + l >= MAX_PATH)
		return false;
	memcpy(out->s, dir->s, dir->n);
	out->n = dir->n;
	if (out->s[out->n - 1] != '\\')
		out->s[out->n++] = '\\';
	memcpy(out->s + out->n, name, l);
	out->n += l;
	out->s[out->n] = 0;
	return true;
}

/* everything the workload keeps is on the heap: only the fs calls use the (small) stack */
struct work {
	struct path dirs[MAXDIRS], files[MAXFILES], tmp, tmp2;
	U32 ndirs, nfiles;
	struct fnode n, n2;
	struct dir_entry de;
	U8 buf[65536];
};
static struct work *w;

static void rd(struct volume *v, struct fnode *n, U64 off, U64 len)
{
	U64 done = 0;

	if (len > sizeof(w->buf))
		len = sizeof(w->buf);
	in_data = true;
	note(v->ops->read(n, off, w->buf, len, &done));
	in_data = false;
	if (done > len) {
		fprintf(stderr, "BUG: read returned %lu bytes for %lu\n", done, len);
		abort();
	}
}

static erc_t wr(struct volume *v, struct fnode *n, U64 off, U64 len, U8 fill)
{
	U64 done = 0;

	memset(w->buf, fill, len);
	return note(v->ops->write(n, off, w->buf, len, &done));
}

static void walk(struct volume *v)
{
	U32 d, i;

	w->ndirs = 1;
	w->nfiles = 0;
	w->dirs[0].s[0] = v->letter;
	memcpy(w->dirs[0].s + 1, ":\\", 3);
	w->dirs[0].n = 3;
	for (d = 0; d < w->ndirs; d++) {
		if (note(v->ops->lookup(w->dirs[d].s, w->dirs[d].n, &w->n)) || !(w->n.attr & ATTR_DIR))
			continue;
		for (i = 0; i < MAXENT; i++) {
			struct fnode dir = w->n;

			if (note(v->ops->readdir(&dir, i, &w->de)))
				break;
			w->de.name[255] = 0;
			if (!join(&w->tmp, &w->dirs[d], w->de.name))
				continue;
			if (note(v->ops->lookup(w->tmp.s, w->tmp.n, &w->n2)))
				continue;
			if (w->n2.attr & ATTR_DIR) {
				if (w->ndirs < MAXDIRS)
					w->dirs[w->ndirs++] = w->tmp;
			} else {
				if (w->nfiles < MAXFILES)
					w->files[w->nfiles++] = w->tmp;
				rd(v, &w->n2, 0, 16384);
				if (w->n2.size > 16384)
					rd(v, &w->n2, w->n2.size - 8192, 16384);
			}
		}
	}
}

/* create, write, grow (sparse), read, cut, rename, delete; a directory with a file in it */
static void churn(struct volume *v, const struct path *dir)
{
	struct fnode *n = &w->n;

	if (!join(&w->tmp, dir, "FZ.TMP") || !join(&w->tmp2, dir, "FZ2.TMP"))
		return;
	if (!note(v->ops->create(w->tmp.s, w->tmp.n, 0, n))) {
		wr(v, n, 0, 20000, 0x5A);
		wr(v, n, 300000, 4096, 0xA5);
		rd(v, n, 0, 8192);
		note(v->ops->truncate(n, 5000));
		note(v->ops->rename(n, w->tmp2.s, w->tmp2.n));
		if (!note(v->ops->lookup(w->tmp2.s, w->tmp2.n, n)))
			note(v->ops->remove(n));
	}
	if (!join(&w->tmp, dir, "FZD") || !join(&w->tmp2, &w->tmp, "IN.TXT"))
		return;
	if (!note(v->ops->create(w->tmp.s, w->tmp.n, ATTR_DIR, n))) {
		if (!note(v->ops->create(w->tmp2.s, w->tmp2.n, 0, &w->n2))) {
			wr(v, &w->n2, 0, 100, 0x33);
			note(v->ops->remove(&w->n2));
		}
		if (!note(v->ops->lookup(w->tmp.s, w->tmp.n, n)) && v->ops->dir_empty(n))
			note(v->ops->remove(n));
	}
}

static void modify(struct volume *v)
{
	struct fnode *n = &w->n;
	U32 i;

	churn(v, &w->dirs[0]);
	if (w->ndirs > 1)
		churn(v, &w->dirs[w->ndirs - 1]);		/* the deepest directory found */
	for (i = 0; i < w->nfiles && i < 3; i++) {
		if (note(v->ops->lookup(w->files[i].s, w->files[i].n, n)) || (n->attr & (ATTR_RO | ATTR_DIR)))
			continue;
		if (i == 0) {				/* rewrite and grow a damaged file, then cut it */
			wr(v, n, 0, 100, 0x77);
			wr(v, n, n->size + 70000, 3000, 0x78);
			note(v->ops->truncate(n, n->size / 3));
		} else if (i == 1) {			/* rename it, then back */
			struct path *a = &w->files[i];

			if (!join(&w->tmp, &w->dirs[0], "FZR.TMP"))
				continue;
			if (!note(v->ops->rename(n, w->tmp.s, w->tmp.n)) &&
			    !note(v->ops->lookup(w->tmp.s, w->tmp.n, n)))
				note(v->ops->rename(n, a->s, a->n));
		} else {
			note(v->ops->remove(n));
		}
	}
	for (i = 1; i < w->ndirs && i < 4; i++) {	/* empty damaged directories can be removed */
		if (!note(v->ops->lookup(w->dirs[i].s, w->dirs[i].n, n)) && !n->is_root &&
		    (n->attr & ATTR_DIR) && v->ops->dir_empty(n))
			note(v->ops->remove(n));
	}
}

static void workload(void)
{
	struct volume *v;
	U32 i;

	vol_init();
	for (i = 0; (v = vol_index(i)); i++) {
		if (!v->ops)
			continue;
		sh->mounted++;
		walk(v);
		if (!v->ro) {
			sh->rw++;
			modify(v);
			walk(v);			/* read back what the changes left */
		}
	}
	ext4_unmount();
}

/* ---- the workload on a small guarded stack (fsfuzz-stk) ---- */

#if FSFUZZ_STACK
static ucontext_t main_ctx, work_ctx;
static U8 *stk_lo;

static void on_segv(int sig, siginfo_t *si, void *uc)
{
	U8 *a = si->si_addr;

	(void)uc;
	if (a >= stk_lo - 4096 && a < stk_lo) {
		static const char m[] = "STACK OVERFLOW: the workload needed more than the FILESYS task's stack\n";

		if (write(2, m, sizeof(m) - 1) < 0)
			_exit(3);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void run_small_stack(void)
{
	U8 *m = mmap(NULL, FSFUZZ_STACK + 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	static U8 altstack[65536];
	stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
	struct sigaction sa;

	if (m == MAP_FAILED)
		abort();
	mprotect(m, 4096, PROT_NONE);		/* the guard page below the stack */
	stk_lo = m + 4096;
	sigaltstack(&ss, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGSEGV, &sa, NULL);
	getcontext(&work_ctx);
	/* the stack ends 4 KB early: its top part stands in for the FILESYS task's own frames */
	work_ctx.uc_stack.ss_sp = stk_lo;
	work_ctx.uc_stack.ss_size = FSFUZZ_STACK;
	work_ctx.uc_link = &main_ctx;
	makecontext(&work_ctx, workload, 0);
	swapcontext(&main_ctx, &work_ctx);
}
#endif

/* ---- damage ---- */

static U64 rng;

static U64 rnd(void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 2685821657736338717ULL;
}

static U64 *meta_list, *data_list;
static U64 nmeta, ndata;

static U64 pick(void)
{
	if (ndata && (!nmeta || rnd() % 5 == 0))
		return data_list[rnd() % ndata];
	return meta_list[rnd() % nmeta];
}

static void damage(U8 *im, U32 iter, U64 seed)
{
	static const U32 edge32[] = { 0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
				      0x7FFFFFFF, 0x80000000U, 0xFFFFFFFFU, 0xFFFFFFFEU, 12, 4096 };
	U32 k, nm;

	rng = (seed ^ ((U64)iter * 0x9E3779B97F4A7C15ULL)) | 1;
	for (k = 0; k < 8; k++)
		rnd();
	nm = 1 + (U32)(rnd() % 8);
	if (rnd() % 3 == 0)
		nm = 1;					/* often a single change: deeper paths */
	for (k = 0; k < nm; k++) {
		U8 *s = im + pick() * 512;
		U32 off = (U32)(rnd() % 512), j, len;

		switch (rnd() % 7) {
		case 0:
			s[off] ^= (U8)(1 << (rnd() % 8));
			break;
		case 1:
			s[off] = (U8)rnd();
			break;
		case 2:
			off &= ~1U;
			s[off] = (U8)edge32[rnd() % 17];
			s[off + 1] = (U8)(rnd() % 2 ? 0 : 0xFF);
			break;
		case 3:
		case 4: {
			U32 v = rnd() % 4 ? edge32[rnd() % 17] : (U32)rnd() % (rnd() % 2 ? 1u << 16 : 1u << 24);

			off &= ~3U;
			if (rnd() % 4 == 0)
				v += (U32)(rnd() % 3) - 1;
			memcpy(s + off, &v, 4);
			break;
		}
		case 5: {					/* splice from another sector */
			U8 *o = im + pick() * 512;

			len = 1 + (U32)(rnd() % 32);
			if (off + len > 512)
				len = 512 - off;
			memmove(s + off, o + rnd() % (512 - len + 1), len);
			break;
		}
		default:
			len = 1 + (U32)(rnd() % 64);
			for (j = 0; j < len && off + j < 512; j++)
				s[off + j] = (U8)rnd();
		}
	}
}

/* ---- main ---- */

static int child(U32 iter, U64 seed, bool dodamage, unsigned timeout)
{
	if (dodamage)
		damage(img, iter, seed);
	alarm(timeout);
	bc_init();
	w = calloc(1, sizeof(*w));
#if FSFUZZ_STACK
	run_small_stack();
#else
	workload();
#endif
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: fsfuzz IMG [-n ITERS] [--seed S] [--timeout SEC] [--replay I [--dump OUT]] [-v]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *path = NULL, *dump = NULL;
	U64 seed = (U64)time(NULL), i, n = 1000, fails = 0;
	long replay = -1;
	unsigned timeout = 20;
	struct stat st;
	int fd, a, status;
	U64 bytes;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-n") && a + 1 < argc)
			n = strtoull(argv[++a], NULL, 0);
		else if (!strcmp(argv[a], "--seed") && a + 1 < argc)
			seed = strtoull(argv[++a], NULL, 0);
		else if (!strcmp(argv[a], "--timeout") && a + 1 < argc)
			timeout = (unsigned)atoi(argv[++a]);
		else if (!strcmp(argv[a], "--replay") && a + 1 < argc)
			replay = atol(argv[++a]);
		else if (!strcmp(argv[a], "--dump") && a + 1 < argc)
			dump = argv[++a];
		else if (!strcmp(argv[a], "-v"))
			verbose = true;
		else if (argv[a][0] != '-' && !path)
			path = argv[a];
		else
			usage();
	}
	if (!path || (fd = open(path, O_RDONLY)) < 0 || fstat(fd, &st))
		usage();
	img_sectors = (U64)st.st_size / 512;
	disk.nblocks = img_sectors;
	img = malloc(img_sectors * 512);	/* read, not mmap: /mnt/c (drvfs) can't map files privately */
	for (bytes = 0; img && bytes < img_sectors * 512;) {
		ssize_t r = read(fd, img + bytes, img_sectors * 512 - bytes);

		if (r <= 0)
			usage();
		bytes += (U64)r;
	}
	close(fd);
	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	bytes = (img_sectors + 7) / 8;
	seen = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	seen_data = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (!img || sh == MAP_FAILED || seen == MAP_FAILED || seen_data == MAP_FAILED)
		usage();
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* the clean run: must pass, and tells where the code reads */
	if (!fork())
		_exit(child(0, seed, false, timeout));
	wait(&status);
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		printf("FSFUZZ FAIL: %s: the undamaged image fails (status %#x)\n", path, status);
		return 1;
	}
	meta_list = malloc(img_sectors * sizeof(U64));
	data_list = malloc(img_sectors * sizeof(U64));
	for (i = 0; i < img_sectors; i++) {
		if (seen[i >> 3] & (1 << (i & 7)))
			meta_list[nmeta++] = i;
		else if (seen_data[i >> 3] & (1 << (i & 7)))
			data_list[ndata++] = i;
	}
	munmap(seen, bytes);
	munmap(seen_data, bytes);
	seen = seen_data = NULL;
	printf("fsfuzz %s: %lu sectors, %lu metadata and %lu data sectors read; %u volumes, %lu writable; seed %lu\n",
	       path, img_sectors, nmeta, ndata, (U32)sh->mounted, sh->rw, seed);
	if (!nmeta)
		return 1;

	if (replay >= 0) {			/* one damaged image, in this process */
		damage(img, (U32)replay, seed);
		if (dump) {
			int o = open(dump, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			if (o < 0 || write(o, img, img_sectors * 512) != (ssize_t)(img_sectors * 512))
				perror(dump);
			close(o);
			printf("damaged image written to %s\n", dump);
		}
		verbose = true;
		bc_init();
		w = calloc(1, sizeof(*w));
#if FSFUZZ_STACK
		run_small_stack();
#else
		workload();
#endif
		printf("replay %ld: no failure\n", replay);
		return 0;
	}

	memset(sh, 0, sizeof(*sh));
	for (i = 1; i <= n; i++) {
		pid_t p = fork();

		if (!p)
			_exit(child((U32)i, seed, true, timeout));
		waitpid(p, &status, 0);
		if (WIFEXITED(status) && !WEXITSTATUS(status))
			continue;
		fails++;
		if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM)
			printf("FAIL  iteration %lu: hang (over %u s)", i, timeout);
		else if (WIFSIGNALED(status))
			printf("FAIL  iteration %lu: signal %d (%s)", i, WTERMSIG(status), strsignal(WTERMSIG(status)));
		else
			printf("FAIL  iteration %lu: exit %d (sanitizer report above)", i, WEXITSTATUS(status));
		printf("; reproduce: %s %s --seed %lu --replay %lu\n", argv[0], path, seed, i);
		if (fails >= 20) {
			printf("stopping after 20 failures\n");
			break;
		}
	}
	printf("fsfuzz %s: %lu damaged images, %lu failures; %lu calls:", path, i > n ? n : i, fails, sh->ops);
	for (a = 0; a < NERC; a++)
		if (sh->erc[a] * 200 >= sh->ops || (a == 211 && sh->erc[a]))
			printf(" %d=%lu", a, sh->erc[a]);
	printf(" (error code=count, the common ones and 211 ErcBrokenFile)\n");
	printf("FSFUZZ %s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}
