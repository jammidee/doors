#!/usr/bin/env bash
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
# Regenerate port64/include/doors/syscalls.def from MMURTL's call gate table (OSSOURCE/MPUBLICS.ASM).
# System call number = (gate selector - 40h) / 8.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
{
	printf '/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */\n'
	printf '/* DOORS system call numbers, generated from OSSOURCE/MPUBLICS.ASM (MMURTL call gates).\n'
	printf '   Number = (gate selector - 40h) / 8, so the order and numbering are exactly MMURTL'"'"'s.\n'
	printf '   Used as an X-macro: #define X(num, name) ...  #include <doors/syscalls.def>\n'
	printf '   Regenerate with tools/gen_syscalls.sh; never renumber. */\n'
	grep -oE "PUBLIC _[A-Za-z]+\s+DF 00000000h:[0-9A-Fa-f]+h" "$root/OSSOURCE/MPUBLICS.ASM" |
		awk '{name=$2; sub(/^_/,"",name); split($4,a,":"); sel=a[2]; sub(/h$/,"",sel);
		      printf "X(%3d, %s)\n", (strtonum("0x" sel)-64)/8, name}'
} > "$root/port64/include/doors/syscalls.def"
