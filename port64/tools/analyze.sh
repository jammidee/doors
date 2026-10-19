#!/bin/sh
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
# Static analysis of the kernel (ROADMAP 2.5, `make analyze`): gcc -fanalyzer, cppcheck and
# clang --analyze over kernel/, drivers/ and arch/, with the kernel's own flags. Every finding fails
# the run, except the ones tools/analyze.allow lists (file + checker, each with its reason).
# Findings that are false alarms in the code itself are marked there with a
# "cppcheck-suppress" comment instead.
#
#   sh tools/analyze.sh            (from port64/; needs gcc, cppcheck, clang)
set -u
cd "$(dirname "$0")/.." || exit 2
OUT=build/analyze
mkdir -p "$OUT"
SRCS=$(find kernel drivers arch -name '*.c' | sort)
KFLAGS="-std=c11 -O2 -ffreestanding -fno-pic -fno-pie -fno-builtin -mno-red-zone -mcmodel=kernel \
 -mgeneral-regs-only -fno-stack-protector -Iinclude -Ibuild/gen"

: > "$OUT/all.txt"
for f in $SRCS; do                                       # 1. gcc's analyzer
	gcc $KFLAGS -fanalyzer -c "$f" -o /dev/null 2>&1 |
		sed -n 's/^\([^:]*\):\([0-9]*\):[0-9]*: warning: .*\[\(-Wanalyzer-[a-z-]*\)\]$/\1:\2 gcc \3/p'
done >> "$OUT/all.txt"
cppcheck -q --std=c11 --platform=unix64 --enable=warning,portability --inline-suppr \
	-Iinclude -Ibuild/gen --suppress=missingIncludeSystem \
	--template='{file}:{line} cppcheck {id}' kernel drivers arch 2>> "$OUT/all.txt"   # 2. cppcheck
for f in $SRCS; do                                       # 3. clang's analyzer
	clang --analyze $KFLAGS -Wno-everything \
		-Xclang -analyzer-checker=core,unix,deadcode,security.insecureAPI.UncheckedReturn \
		"$f" -o /dev/null 2>&1 |
		sed -n 's/^\([^:]*\):\([0-9]*\):[0-9]*: warning: .*\[\([a-zA-Z.]*\)\]$/\1:\2 clang \3/p'
done >> "$OUT/all.txt"

# allowed: "file checker" (any line), from analyze.allow without comments
fails=0
total=0
while read -r loc tool checker; do
	[ -n "$loc" ] || continue
	total=$((total + 1))
	file=${loc%%:*}
	if grep -v '^#' tools/analyze.allow | grep -q "^$file $checker "; then
		echo "allowed  $loc $tool $checker"
	else
		echo "FINDING  $loc $tool $checker"
		fails=$((fails + 1))
	fi
done < "$OUT/all.txt"
if [ "$fails" = 0 ]; then
	echo "ANALYZE PASS ($total findings, all on the allow list)"
else
	echo "ANALYZE FAIL ($fails of $total findings not allowed; details: rerun the tool on the file)"
	exit 1
fi
