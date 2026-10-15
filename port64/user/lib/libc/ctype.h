/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS C library: character classes (CM32's CTYPE.H), ASCII only. */
#ifndef DOORS_CTYPE_H
#define DOORS_CTYPE_H

int iscntrl(int c);
int isspace(int c);
int isdigit(int c);
int isupper(int c);
int islower(int c);
int ispunct(int c);
int isalpha(int c);
int isxdigit(int c);
int isalnum(int c);
int isgraph(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

#endif
