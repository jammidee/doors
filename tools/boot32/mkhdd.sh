#!/usr/bin/env bash
# Build a small IDE hard disk image (MBR + one FAT16 partition) for MMURTL V1.0.
#
# MMURTL's file system fails to initialise at all without hard disk 0 (Erc 655 from
# HardIDE, then Erc 505 from InitFS/read_PE), and COMMANDS.CLI expects C:\MSamples\...
#
# Geometry: 16 heads x 63 sectors/track, partition at sector 63, < 32 MB so the
# partition type is 04h (FAT16 <32M), as MS-DOS of that era would create it.
#
# Usage: tools/boot32/mkhdd.sh [output.img]     (run in WSL/Linux; needs mtools)
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$root/build/boot32/mmurtl-hdd.img}"
mkdir -p "$(dirname "$out")"

heads=16 spt=63 cyls=60                     # 60*16*63 sectors = ~29.5 MB
total=$((cyls * heads * spt))
start=$spt
dd if=/dev/zero of="$out" bs=512 count="$total" status=none

# MBR partition entry 1 (offset 446): active, CHS start 0/1/1, type 04h, CHS end, LBA start, size.
byte() { for b; do printf "\\$(printf %03o "$b")"; done; }
le32() { byte $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)); }
ec=$((cyls - 1)) eh=$((heads - 1))
{
	byte 0x80 1 1 0 4
	byte $eh $((spt | (ec >> 8) << 6)) $((ec & 255))
	le32 $start
	le32 $((total - start))
} | dd of="$out" bs=1 seek=446 conv=notrunc status=none
printf '\x55\xaa' | dd of="$out" bs=1 seek=510 conv=notrunc status=none

export MTOOLS_SKIP_CHECK=1
img="$out@@$((start * 512))"
mformat -i "$img" -T $((total - start)) -h $heads -s $spt -H $start -v MMURTL ::

mcopy -i "$img" -s "$root/MMSYS"    ::MMSYS
mcopy -i "$img" -s "$root/MSAMPLES" ::MSAMPLES

echo "$out"
