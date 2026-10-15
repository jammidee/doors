#!/usr/bin/env bash
# Build a small IDE hard disk image (MBR + one FAT16 partition) for MMURTL V1.0.
#
# MMURTL's file system fails to initialise at all without hard disk 0 (Erc 655 from
# HardIDE, then Erc 505 from InitFS/read_PE), and COMMANDS.CLI expects C:\MSamples\...
#
# Geometry: 16 heads x 17 sectors/track (classic MFM/early IDE), partition at sector 17
# (head 1, sector 1), < 32 MB so the partition type is 04h (FAT16 <32M).
# 17 sectors/track matters: HardIDE.C starts with a hardcoded 16x17 geometry and FSYS.C
# (read_PE -> GetBSInfo) reads the partition boot sector with it *before* learning the real
# geometry. With 63 sectors/track that read lands on the wrong sector, the geometry becomes
# garbage, and subdirectory reads on C: return wrong sectors (seen as Erc 668).
#
# HeadEnd quirk: the shipped MMURTL.RUN takes the head COUNT from the partition entry's
# HeadEnd byte (FSYS.C:579 `PDrvs[d].nHeads = partab[i].HeadEnd`), which by the standard is
# the highest head INDEX (heads-1). The ReadCMOS(0x1d) override in FSYS.C's SetDriveGeometry is
# not in that binary. So HeadEnd is written as `heads` here; otherwise the driver programs the
# drive and computes CHS with one head too few (QEMU trace: Device/Head 0xAE for a 16-head disk).
# mtools and QEMU only use the LBA fields, so this is harmless to them.
#
# Usage: tools/boot32/mkhdd.sh [output.img]     (run in WSL/Linux; needs mtools)
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$root/build/boot32/mmurtl-hdd.img}"
mkdir -p "$(dirname "$out")"

heads=16 spt=17 cyls=216                    # 216*16*17 sectors = ~28.7 MB
total=$((cyls * heads * spt))
start=$spt
dd if=/dev/zero of="$out" bs=512 count="$total" status=none

# MBR partition entry 1 (offset 446): active, CHS start 0/1/1, type 04h, CHS end, LBA start, size.
byte() { for b; do printf "\\$(printf %03o "$b")"; done; }
le32() { byte $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)); }
ec=$((cyls - 1)) eh=$heads                  # eh: see "HeadEnd quirk" above (standard: heads-1)
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
