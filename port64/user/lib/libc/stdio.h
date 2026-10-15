/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS C library: standard I/O (the subset of CM32's STDIO.H, CM32/LIB/FILEIO.C, FMTDIO.C).
   Files are MMURTL stream files; stdout and stderr are the job's screen (TTYOut). */
#ifndef DOORS_STDIO_H
#define DOORS_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct doors_file FILE;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern FILE *const stdin, *const stdout, *const stderr;

FILE  *fopen(const char *name, const char *mode);	/* "r", "w", "a", with optional "+" */
int    fclose(FILE *f);
int    fgetc(FILE *f);
char  *fgets(char *s, int n, FILE *f);
int    fputc(int c, FILE *f);
int    fputs(const char *s, FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
long   ftell(FILE *f);
int    fseek(FILE *f, long off, int origin);
void   rewind(FILE *f);
int    remove(const char *name);
int    rename(const char *oldname, const char *newname);
int    getc(FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    puts(const char *s);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *s, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *fmt, va_list ap);
int vsprintf(char *s, const char *fmt, va_list ap);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);

#endif
