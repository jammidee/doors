/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS C library for user programs: formatted output, stdio over the MMURTL file calls (as CM32's
   FILEIO.C), strings, ctype and stdlib. memcpy, memset, memcmp and strlen are in libdoors.c.

   fopen opens a stream file. "r" reads (ModeRead), "w" creates or empties, "a" creates or appends,
   "+" adds modify access. Reads go through a 512-byte buffer per file. stdout and stderr write to
   the job's screen with TTYOut; stdin reads the keyboard (ReadKbd, no echo). */

#include <stdarg.h>
#include "doors.h"
#include "libc/ctype.h"
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/string.h"

/* ---- formatted output: one formatter, three sinks (screen, string, file) ---- */

struct sink {
	void (*put)(struct sink *s, char c);
	char *buf;			/* string sink */
	size_t pos, max;		/* max = room including the NUL */
	FILE *f;			/* file sink */
	char tty[128];			/* screen sink buffer */
	U32 n;
	int total;
};

static void put(struct sink *s, char c)
{
	s->put(s, c);
	s->total++;
}

static void field(struct sink *s, const char *p, int len, int width, bool left, char pad)
{
	int i;

	if (!left)
		for (i = len; i < width; i++)
			put(s, pad);
	for (i = 0; i < len; i++)
		put(s, p[i]);
	if (left)
		for (i = len; i < width; i++)
			put(s, ' ');
}

static int vformat(struct sink *s, const char *fmt, va_list ap)
{
	for (; *fmt; fmt++) {
		bool left = false, neg = false, upper = false;
		char pad = ' ', t[24];
		int width = 0, prec = -1, lng = 0, n = 0, len;
		U64 v = 0;
		unsigned base = 10;
		const char *str;

		if (*fmt != '%') {
			put(s, *fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-')
				left = true;
			else if (*fmt == '0')
				pad = '0';
			else if (*fmt != '+' && *fmt != ' ' && *fmt != '#')
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			}
			while (*fmt >= '0' && *fmt <= '9')
				prec = prec * 10 + (*fmt++ - '0');
		}
		while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z')
			lng += *fmt++ != 'h';
		if (left)
			pad = ' ';
		switch (*fmt) {
		case 'c':
			t[0] = (char)va_arg(ap, int);
			field(s, t, 1, width, left, ' ');
			continue;
		case 's':
			str = va_arg(ap, const char *);
			if (!str)
				str = "(null)";
			for (len = 0; str[len] && (prec < 0 || len < prec); len++)
				;
			field(s, str, len, width, left, ' ');
			continue;
		case 'd':
		case 'i': {
			S64 sv = lng ? va_arg(ap, S64) : va_arg(ap, int);

			neg = sv < 0;
			v = neg ? (U64)(-(sv + 1)) + 1 : (U64)sv;
			break;
		}
		case 'X':
			upper = true;
			/* fall through */
		case 'x':
			base = 16;
			/* fall through */
		case 'u':
			v = lng ? va_arg(ap, U64) : va_arg(ap, unsigned);
			break;
		case 'o':
			base = 8;
			v = lng ? va_arg(ap, U64) : va_arg(ap, unsigned);
			break;
		case 'p':
			put(s, '0');
			put(s, 'x');
			v = (U64)(uintptr_t)va_arg(ap, void *);
			base = 16;
			break;
		case '%':
			put(s, '%');
			continue;
		case '\0':
			return s->total;
		default:
			put(s, '%');
			put(s, *fmt);
			continue;
		}
		do {
			t[n++] = (upper ? "0123456789ABCDEF" : "0123456789abcdef")[v % base];
			v /= base;
		} while (v);
		if (neg && pad == '0') {
			put(s, '-');
			width--;
		} else if (neg) {
			t[n++] = '-';
		}
		for (len = 0; len < n / 2; len++) {		/* reverse the digits */
			char c = t[len];

			t[len] = t[n - 1 - len];
			t[n - 1 - len] = c;
		}
		field(s, t, n, width, left, pad);
	}
	return s->total;
}

static void tty_put(struct sink *s, char c)
{
	if (s->n == sizeof(s->tty)) {
		TTYOut(s->tty, s->n, 7);
		s->n = 0;
	}
	s->tty[s->n++] = c;
}

static void str_put(struct sink *s, char c)
{
	if (s->pos + 1 < s->max)
		s->buf[s->pos++] = c;
}

static void file_put(struct sink *s, char c)
{
	fputc(c, s->f);
}

int vprintf(const char *fmt, va_list ap)
{
	struct sink s = { .put = tty_put };
	int n = vformat(&s, fmt, ap);

	if (s.n)
		TTYOut(s.tty, s.n, 7);
	return n;
}

int printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vprintf(fmt, ap);
	va_end(ap);
	return n;
}

int vsnprintf(char *buf, size_t max, const char *fmt, va_list ap)
{
	struct sink s = { .put = str_put, .buf = buf, .max = max };
	int n = vformat(&s, fmt, ap);

	if (max)
		buf[s.pos] = 0;
	return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
	return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsprintf(buf, fmt, ap);
	va_end(ap);
	return n;
}

int snprintf(char *buf, size_t max, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, max, fmt, ap);
	va_end(ap);
	return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
	struct sink s = { .put = file_put, .f = f };
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vformat(&s, fmt, ap);
	va_end(ap);
	return n;
}

/* ---- stdio ---- */

#define NFILES 16

struct doors_file {
	bool used, screen, kbd;
	U32 h;
	U8 buf[512];
	U32 pos, len;			/* unread bytes are buf[pos..len) */
};

static struct doors_file files[NFILES], f_in = { .used = true, .kbd = true },
			  f_out = { .used = true, .screen = true };
FILE *const stdin = &f_in, *const stdout = &f_out, *const stderr = &f_out;

FILE *fopen(const char *name, const char *mode)
{
	U32 len = (U32)strlen(name), m = 0, i;
	bool create = false, discard = false, append = false;
	erc_t erc = ErcOk;
	FILE *f;

	if (*mode == 'w')
		m = ModeModify, create = discard = true;
	else if (*mode == 'a')
		m = ModeModify, create = append = true;
	else if (*mode != 'r')
		return NULL;
	if (strchr(mode, '+'))
		m = ModeModify;
	for (i = 0; i < NFILES && files[i].used; i++)
		;
	if (i == NFILES)
		return NULL;
	f = &files[i];
	if (create && (erc = CreateFile(name, len, 0x20)) == ErcDupName)	/* archive bit, as CM32 */
		erc = ErcOk;
	if (!erc)
		erc = OpenFile(name, len, m, 1, &f->h);
	if (erc)
		return NULL;
	if (append)
		erc = SetFileLFA(f->h, ~0UL);			/* end of file */
	if (!erc && discard)
		erc = SetFileSize(f->h, 0);
	if (erc) {
		CloseFile(f->h);
		return NULL;
	}
	f->used = true;
	f->screen = f->kbd = false;
	f->pos = f->len = 0;
	return f;
}

int fclose(FILE *f)
{
	if (!f || !f->used || f->screen || f->kbd)
		return EOF;
	f->used = false;
	return CloseFile(f->h) ? EOF : 0;
}

/* Bytes read ahead go back before the file position is used for anything else. */
static void unread(FILE *f)
{
	U64 lfa;

	if (f->pos < f->len && GetFileLFA(f->h, &lfa) == ErcOk)
		SetFileLFA(f->h, lfa - (f->len - f->pos));
	f->pos = f->len = 0;
}

int fgetc(FILE *f)
{
	U64 n;

	if (!f || !f->used || f->screen)
		return EOF;
	if (f->kbd) {
		U32 key;

		if (ReadKbd(&key, 1))
			return EOF;
		key &= 0x7F;
		return key == 0x0D ? '\n' : (int)key;
	}
	if (f->pos == f->len) {
		erc_t erc = ReadBytes(f->h, f->buf, sizeof(f->buf), &n);

		if ((erc && erc != ErcEOF) || !n)
			return EOF;
		f->pos = 0;
		f->len = (U32)n;
	}
	return f->buf[f->pos++];
}

int getc(FILE *f)
{
	return fgetc(f);
}

/* Read up to n-1 characters, through the end of a line (the LF is kept). NULL at end of file. */
char *fgets(char *s, int n, FILE *f)
{
	char *p = s;
	int c = 0;

	while (n > 1 && (c = fgetc(f)) != EOF) {
		*p++ = (char)c;
		n--;
		if (c == '\n')
			break;
	}
	*p = 0;
	return p == s ? NULL : s;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
	U64 done;

	if (!f || !f->used || f->kbd || !size)
		return 0;
	if (f->screen) {
		TTYOut(p, size * n, 7);
		return n;
	}
	unread(f);
	if (WriteBytes(f->h, p, size * n, &done) && !done)
		return 0;
	return (size_t)(done / size);
}

size_t fread(void *p, size_t size, size_t n, FILE *f)
{
	U8 *d = p;
	size_t i;
	int c;

	if (!size)
		return 0;
	for (i = 0; i < size * n && (c = fgetc(f)) != EOF; i++)
		d[i] = (U8)c;
	return i / size;
}

int fputc(int c, FILE *f)
{
	char ch = (char)c;

	return fwrite(&ch, 1, 1, f) == 1 ? (U8)ch : EOF;
}

int putc(int c, FILE *f)
{
	return fputc(c, f);
}

int putchar(int c)
{
	return fputc(c, stdout);
}

int fputs(const char *s, FILE *f)
{
	size_t n = strlen(s);

	return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char *s)
{
	return fputs(s, stdout) || fputc('\n', stdout) == EOF ? EOF : 0;
}

long ftell(FILE *f)
{
	U64 lfa;

	if (!f || !f->used || f->screen || f->kbd || GetFileLFA(f->h, &lfa))
		return -1;
	return (long)(lfa - (f->len - f->pos));
}

int fseek(FILE *f, long off, int origin)
{
	U64 base = 0, size;

	if (!f || !f->used || f->screen || f->kbd)
		return -1;
	if (origin == SEEK_CUR)
		base = (U64)ftell(f);
	else if (origin == SEEK_END && GetFileSize(f->h, &size) == ErcOk)
		base = size;
	f->pos = f->len = 0;
	return SetFileLFA(f->h, base + (U64)off) ? -1 : 0;
}

void rewind(FILE *f)
{
	fseek(f, 0, SEEK_SET);
}

int remove(const char *name)
{
	U32 h;

	if (OpenFile(name, (U32)strlen(name), ModeModify, 0, &h))
		return -1;
	if (DeleteFile(h)) {
		CloseFile(h);
		return -1;
	}
	return 0;
}

int rename(const char *a, const char *b)
{
	return RenameFile(a, (U32)strlen(a), b, (U32)strlen(b)) ? -1 : 0;
}

/* ---- strings (memcpy, memset, memcmp and strlen are in libdoors.c) ---- */

void *memmove(void *d, const void *s, size_t n)
{
	if (d < s)
		return memcpy(d, s, n);
	CopyDataR(s, d, n);
	return d;
}

void *memchr(const void *s, int c, size_t n)
{
	const U8 *p = s;

	for (; n--; p++)
		if (*p == (U8)c)
			return (void *)p;
	return NULL;
}

char *strcpy(char *d, const char *s)
{
	char *r = d;

	while ((*d++ = *s++))
		;
	return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n && s[i]; i++)
		d[i] = s[i];
	for (; i < n; i++)
		d[i] = 0;
	return d;
}

char *strcat(char *d, const char *s)
{
	strcpy(d + strlen(d), s);
	return d;
}

char *strncat(char *d, const char *s, size_t n)
{
	char *p = d + strlen(d);

	while (n-- && *s)
		*p++ = *s++;
	*p = 0;
	return d;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (U8)*a - (U8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++)
		if (*a != *b || !*a)
			return (U8)*a - (U8)*b;
	return 0;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

char *strrchr(const char *s, int c)
{
	const char *r = NULL;

	for (;; s++) {
		if (*s == (char)c)
			r = s;
		if (!*s)
			return (char *)r;
	}
}

char *strstr(const char *s, const char *find)
{
	size_t n = strlen(find);

	for (; *s; s++)
		if (!strncmp(s, find, n))
			return (char *)s;
	return n ? NULL : (char *)s;
}

/* ---- ctype (ASCII) ---- */

int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7F; }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isgraph(int c)  { return c > ' ' && c < 0x7F; }
int isprint(int c)  { return c >= ' ' && c < 0x7F; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int toupper(int c)  { return islower(c) ? c - 32 : c; }
int tolower(int c)  { return isupper(c) ? c + 32 : c; }

/* ---- stdlib ---- */

void exit(int status)
{
	ExitJob(status);
}

unsigned long strtoul(const char *s, char **end, int base)
{
	unsigned long v = 0;
	int d;

	while (isspace(*s))
		s++;
	if (*s == '+')
		s++;
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2, base = 16;
	else if (base == 0)
		base = s[0] == '0' ? 8 : 10;
	for (;; s++) {
		if (isdigit(*s))
			d = *s - '0';
		else if (isalpha(*s))
			d = tolower(*s) - 'a' + 10;
		else
			break;
		if (d >= base)
			break;
		v = v * (unsigned long)base + (unsigned long)d;
	}
	if (end)
		*end = (char *)s;
	return v;
}

long strtol(const char *s, char **end, int base)
{
	bool neg;

	while (isspace(*s))
		s++;
	neg = *s == '-';
	if (neg)
		s++;
	return neg ? -(long)strtoul(s, end, base) : (long)strtoul(s, end, base);
}

long atol(const char *s) { return strtol(s, NULL, 10); }
int  atoi(const char *s) { return (int)strtol(s, NULL, 10); }
int  abs(int v)          { return v < 0 ? -v : v; }
long labs(long v)        { return v < 0 ? -v : v; }
