/* DOORS common types and helpers. */
#ifndef DOORS_TYPES_H
#define DOORS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MMURTL-style names, kept for code ported from the original sources. */
typedef uint8_t  U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;
typedef int8_t   S8;
typedef int16_t  S16;
typedef int32_t  S32;
typedef int64_t  S64;

/* Error/status code: 0 = OK (ErcOk). Stays 32-bit, as in MMURTL. */
typedef uint32_t erc_t;
#define ErcOk 0

#define KERNEL_VMA 0xFFFFFFFF80000000ULL
#define PHYS_TO_VIRT(p) ((void *)((uintptr_t)(p) + KERNEL_VMA))   /* first 1 GiB only (M1) */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NORETURN __attribute__((noreturn))
#define PACKED   __attribute__((packed))

#endif
