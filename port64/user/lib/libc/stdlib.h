/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS C library: CM32's STDLIB.H (exit) plus number conversion. No malloc yet: use AllocPage. */
#ifndef DOORS_STDLIB_H
#define DOORS_STDLIB_H

#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

__attribute__((noreturn)) void exit(int status);	/* ExitJob(status) */
int  atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int  abs(int v);
long labs(long v);

#endif
