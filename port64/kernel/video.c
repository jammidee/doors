/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Virtual text video (MMURTL VIDEO.ASM). See include/doors/video.h.

   Each job has an 80x25 cell buffer (character | attribute << 8). Every write goes into the job's
   buffer and, if the job is the video owner, also into VGA memory, so SetVidOwner only copies the
   new owner's buffer to the screen. All state is protected by irq_save (kernel messages can come
   from interrupt handlers). The video calls act on the calling task's job, as in MMURTL. */

#include <doors/console.h>
#include <doors/gtable.h>
#include <doors/job.h>
#include <doors/mem.h>
#include <doors/msg.h>
#include <doors/task.h>
#include <doors/video.h>
#include <doors/x86.h>

#define CELLS (VID_COLS * VID_ROWS)

struct vscreen {
	U16 cell[CELLS];
	U32 x, y;			/* TTY position (also the cursor) */
	U8 norm;			/* NormAttr: ClrScr and ScrollVid fill */
	U8 top;				/* first TTY row: 1 for job 1 (status line), else 0 */
};

/* Jobs 0-2 have static screens (job 1's is needed from the first boot message on, before the
   growable tables exist); job 3 and up have theirs in a growable table (ROADMAP 3.1). */
static struct vscreen vs_boot[3];
static struct gtable gt_screens;

static struct vscreen *vscr(U32 j)
{
	return j < 3 ? &vs_boot[j] : (struct vscreen *)(void *)gt_screens.base + j;
}
#define VS(j) (*vscr(j))

void vid_jobs_init(U32 first, U32 max)
{
	gt_init(&gt_screens, "screens", GT_SCREENS, sizeof(struct vscreen), max, first, 0);
}

bool vid_grow(U32 want)
{
	return gt_grow(&gt_screens, want);
}
static volatile U16 *vga = (volatile U16 *)PHYS_TO_VIRT(0xB8000);
static U32 owner = 1;
static U8 kattr = NORM_ATTR;
static U32 clock_base;

#define STATUS_ATTR 0x1F		/* bright white on blue */

static U16 blank(U8 attr) { return (U16)(attr << 8) | ' '; }

static void put(U32 job, U32 pos, U16 c)
{
	VS(job).cell[pos] = c;
	if (job == owner)
		vga[pos] = c;
}

static void cursor(U32 job)
{
	U16 pos;

	if (job != owner)
		return;
	pos = (U16)(VS(job).y * VID_COLS + VS(job).x);
	outb(0x3D4, 0x0F);
	outb(0x3D5, (U8)(pos & 0xFF));
	outb(0x3D4, 0x0E);
	outb(0x3D5, (U8)(pos >> 8));
}

/* Scroll the rectangle up (fUp) or down one line; the freed line gets NormAttr blanks. */
static void scroll(U32 job, U32 col, U32 line, U32 ncols, U32 nlines, bool up)
{
	struct vscreen *s = &VS(job);
	U32 r, c;

	if (!nlines || !ncols)
		return;
	if (up) {
		for (r = line; r + 1 < line + nlines; r++)
			for (c = col; c < col + ncols; c++)
				put(job, r * VID_COLS + c, s->cell[(r + 1) * VID_COLS + c]);
		r = line + nlines - 1;
	} else {
		for (r = line + nlines - 1; r > line; r--)
			for (c = col; c < col + ncols; c++)
				put(job, r * VID_COLS + c, s->cell[(r - 1) * VID_COLS + c]);
		r = line;
	}
	for (c = col; c < col + ncols; c++)
		put(job, r * VID_COLS + c, blank(s->norm));
}

/* TTY output (MMURTL TTYOut): LF = next line, column 0; CR = column 0; BS = one column left
   (not destructive). DOORS also expands TAB to the next multiple of 8. Past the last line the
   TTY area scrolls up. */
static void tty(U32 job, char ch, U8 attr)
{
	struct vscreen *s = &VS(job);

	switch (ch) {
	case '\n':
		s->x = 0;
		s->y++;
		break;
	case '\r':
		s->x = 0;
		break;
	case '\b':
		if (s->x)
			s->x--;
		break;
	case '\t':
		s->x = (s->x + 8) & ~7u;
		break;
	default:
		put(job, s->y * VID_COLS + s->x, (U16)(attr << 8) | (U8)ch);
		s->x++;
	}
	if (s->x >= VID_COLS) {
		s->x = 0;
		s->y++;
	}
	if (s->y >= VID_ROWS) {
		scroll(job, 0, s->top, VID_COLS, VID_ROWS - s->top, true);
		s->y = VID_ROWS - 1;
	}
}

static void clear(U32 job, U32 from_row)
{
	U32 i;

	for (i = from_row * VID_COLS; i < CELLS; i++)
		put(job, i, blank(VS(job).norm));
	VS(job).x = 0;
	VS(job).y = from_row;
}

static void show(U32 job)
{
	U32 i;

	owner = job;
	for (i = 0; i < CELLS; i++)
		vga[i] = VS(job).cell[i];
	cursor(job);
}

void vid_init(void)
{
	U32 j;

	for (j = 0; j < 3; j++) {		/* the others: vid_job_new */
		VS(j).norm = NORM_ATTR;
		clear(j, 0);
	}
	VS(1).top = 1;
	VS(1).y = 1;
	show(1);
}

void vid_remap(void)
{
	vga = (volatile U16 *)phys_to_virt(0xB8000);
}

void vid_job_new(U32 job)
{
	U64 f = irq_save();

	VS(job).norm = NORM_ATTR;
	VS(job).top = 0;
	clear(job, 0);
	irq_restore(f);
}

void vid_job_gone(U32 job)
{
	U64 f = irq_save();

	if (owner == job)
		show(1);
	irq_restore(f);
}

void vid_force_owner(U32 job)
{
	show(job);
}

U32 vid_owner(void)
{
	return owner;
}

/* Ctrl-Alt-PgDn: the next job that is running (job 1 and loaded programs), in number order. */
void vid_next_owner(void)
{
	U64 f = irq_save();
	U32 j = owner;
	const struct job *jb;

	do {
		j = (j + 1) % job_limit();
		jb = job_get(j);
	} while (j != owner && (j == 0 || j == 2 || !jb || !jb->alive));
	show(j);
	irq_restore(f);
}

/* ---- job 1: kernel console and status line ---- */

void vid_kputc(char c)
{
	U64 f = irq_save();

	tty(1, c, kattr);
	cursor(1);
	irq_restore(f);
}

void vid_kattr(U8 attr)
{
	kattr = attr;
}

void vid_set_title(const char *title)
{
	U32 i;

	for (i = 0; i < 44; i++)
		put(1, i, (U16)(STATUS_ATTR << 8) | (U8)(*title ? *title++ : ' '));
}

void vid_clock_base(U32 t)
{
	clock_base = t;
}

/* "hh:mm:ss  Tick:n" at the right of job 1's row 0 (MMURTL's Monitor shows time and ticks). */
void vid_status_tick(U64 ticks)
{
	char s[36];
	U32 n = 0, i, t = clock_base + (U32)(ticks / 100), d = 0;
	char num[21];
	U64 v = ticks;

	if (clock_base) {
		t %= 86400;
		s[n++] = (char)('0' + t / 36000);
		s[n++] = (char)('0' + t / 3600 % 10);
		s[n++] = ':';
		s[n++] = (char)('0' + t % 3600 / 600);
		s[n++] = (char)('0' + t % 600 / 60);
		s[n++] = ':';
		s[n++] = (char)('0' + t % 60 / 10);
		s[n++] = (char)('0' + t % 10);
		s[n++] = ' ';
		s[n++] = ' ';
	}
	s[n++] = 'T'; s[n++] = 'i'; s[n++] = 'c'; s[n++] = 'k'; s[n++] = ':';
	do {
		num[d++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && d < 20);
	while (d)
		s[n++] = num[--d];
	s[n++] = ' ';
	for (i = 44; i < VID_COLS; i++)
		put(1, i, (U16)(STATUS_ATTR << 8) | ' ');
	for (i = 0; i < n; i++)
		put(1, VID_COLS - n + i, (U16)(STATUS_ATTR << 8) | (U8)s[i]);
}

/* ---- MMURTL video calls (act on the caller's job) ---- */

static U32 me(void)
{
	return current->job;
}

erc_t SetVidOwner(U32 dJobNum)
{
	const struct job *jb = job_get(dJobNum);
	U64 f;

	if (dJobNum >= job_limit() || !jb || !jb->alive || dJobNum == 2)
		return ErcVidNum;
	f = irq_save();
	show(dJobNum);
	irq_restore(f);
	return ErcOk;
}

erc_t GetVidOwner(U32 *pdVidNumRet)
{
	*pdVidNumRet = owner;
	return ErcOk;
}

erc_t SetNormVid(U32 dAttr)
{
	VS(me()).norm = (U8)dAttr;
	return ErcOk;
}

erc_t GetNormVid(U32 *pdNormVidRet)
{
	*pdNormVidRet = VS(me()).norm;
	return ErcOk;
}

erc_t ClrScr(void)
{
	U64 f = irq_save();
	U32 j = me();

	clear(j, VS(j).top);
	cursor(j);
	irq_restore(f);
	return ErcOk;
}

erc_t TTYOut(const char *pTextOut, U64 ddTextOut, U32 ddAttrib)
{
	U64 f = irq_save();
	U32 j = me();

	while (ddTextOut--)
		tty(j, *pTextOut++, (U8)ddAttrib);
	cursor(j);
	irq_restore(f);
	return ErcOk;
}

erc_t PutVidChars(U32 ddCol, U32 ddLine, const char *pChars, U32 sChars, U32 ddAttrib)
{
	U32 pos = ddLine * VID_COLS + ddCol, j = me(), i;
	U64 f;

	if (ddCol >= VID_COLS || ddLine >= VID_ROWS || sChars > CELLS - pos)
		return ErcVidParam;
	f = irq_save();
	for (i = 0; i < sChars; i++)
		put(j, pos + i, (U16)((U8)ddAttrib << 8) | (U8)pChars[i]);
	irq_restore(f);
	return ErcOk;
}

erc_t PutVidAttrs(U32 ddCol, U32 ddLine, U32 sChars, U32 dAttr)
{
	U32 pos = ddLine * VID_COLS + ddCol, j = me(), i;
	U64 f;

	if (ddCol >= VID_COLS || ddLine >= VID_ROWS || sChars > CELLS - pos)
		return ErcVidParam;
	f = irq_save();
	for (i = 0; i < sChars; i++)
		put(j, pos + i, (U16)((U8)dAttr << 8) | (VS(j).cell[pos + i] & 0xFF));
	irq_restore(f);
	return ErcOk;
}

erc_t GetVidChar(U32 ddCol, U32 ddLine, U8 *pCharRet, U8 *pAttrRet)
{
	U16 c;

	if (ddCol >= VID_COLS || ddLine >= VID_ROWS)
		return ErcVidParam;
	c = VS(me()).cell[ddLine * VID_COLS + ddCol];
	*pCharRet = (U8)c;
	*pAttrRet = (U8)(c >> 8);
	return ErcOk;
}

erc_t SetXY(U32 NewX, U32 NewY)
{
	U32 j = me();
	U64 f;

	if (NewX >= VID_COLS || NewY >= VID_ROWS)
		return ErcVidParam;
	f = irq_save();
	VS(j).x = NewX;
	VS(j).y = NewY;
	cursor(j);
	irq_restore(f);
	return ErcOk;
}

erc_t GetXY(U32 *pXRet, U32 *pYRet)
{
	*pXRet = VS(me()).x;
	*pYRet = VS(me()).y;
	return ErcOk;
}

erc_t ScrollVid(U32 ddULCol, U32 ddULline, U32 nddCols, U32 nddLines, U32 ddfUp)
{
	U64 f;

	if (ddULCol >= VID_COLS || ddULline >= VID_ROWS || nddCols > VID_COLS - ddULCol ||
	    nddLines > VID_ROWS - ddULline)
		return ErcVidParam;
	f = irq_save();
	scroll(me(), ddULCol, ddULline, nddCols, nddLines, ddfUp != 0);
	irq_restore(f);
	return ErcOk;
}

/* PC speaker through PIT channel 2 (MMURTL Beep_Work). QEMU plays it with -audiodev ... -machine
   pcspk-audiodev=...; without that the tone is silent but the call still takes its time. */
erc_t Tone(U32 dHz, U32 dTicks10ms)
{
	U32 div;
	U8 p;

	if (dHz < 20 || dHz > 20000)
		return ErcVidParam;
	div = 1193182 / dHz;
	outb(0x43, 0xB6);
	outb(0x42, (U8)div);
	outb(0x42, (U8)(div >> 8));
	p = inb(0x61);
	outb(0x61, p | 3);
	Sleep(dTicks10ms);
	outb(0x61, inb(0x61) & ~3);
	return ErcOk;
}

erc_t Beep(void)
{
	return Tone(800, 35);			/* MMURTL: 800 Hz, 350 ms */
}
