#!/usr/bin/env bash
# Build a bootable 1.44 MB MMURTL V1.0 floppy from the original files.
#
# Layout expected by MSCODE16/BOOTCODE/BOOTBLOK.ASM:
#   sector 0   : MMURTL boot sector (BOOTSECT.IMG, FAT12 1.44 MB BPB)
#   cluster 2+ : MMURTL.IMG (320 sectors, made from MMSYS/MMURTL.RUN by mkimg.py),
#                stored contiguously -> must be the first file copied
#   \MMSYS     : system directory read by the Monitor (INITIAL.JOB, CLI.RUN, ...)
#
# Usage: tools/boot32/mkfloppy.sh [output.img]     (run in WSL/Linux; needs mtools)
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$root/build/boot32/mmurtl-floppy.img}"
boot="$root/MSCODE16/BOOTCODE"
mkdir -p "$(dirname "$out")"

dd if=/dev/zero of="$out" bs=512 count=2880 status=none
mformat -i "$out" -f 1440 ::
# Boot sector = the 512 bytes of code in BOOTBLOK.EXE (after its 512-byte EXE header); this
# matches BOOTBLOK.LST. The shipped BOOTSECT.IMG is a different build with a bad BPB
# (nBytesPerSect = 2000h), so it is not used.
hdr=$(( $(od -An -tu2 -j8 -N2 "$boot/BOOTBLOK.EXE") * 16 ))
dd if="$boot/BOOTBLOK.EXE" of="$out" bs=1 skip="$hdr" count=512 conv=notrunc status=none
# BOOTBLOK.ASM ends with BootSig DW 0AA5Fh; modern BIOSes (SeaBIOS) require 0AA55h.
printf '\x55\xaa' | dd of="$out" bs=1 seek=510 count=2 conv=notrunc status=none

# OS image built from the current kernel (the prebuilt MMURTL.IMG is an older build).
osimg="$(dirname "$out")/MMURTL.IMG"
python3 "$(dirname "$0")/mkimg.py" "$root/MMSYS/MMURTL.RUN" "$osimg"

export MTOOLS_SKIP_CHECK=1
mcopy -i "$out" "$osimg" ::MMURTL.IMG
mmd   -i "$out" ::MMSYS
for f in "$root"/MMSYS/*; do
	case "$(basename "$f")" in MMLOADER.EXE) continue ;; esac
	mcopy -i "$out" "$f" "::MMSYS/$(basename "$f")"
done

# The boot sector does not read the FAT; verify the OS really starts at cluster 2.
first=$(mshowfat -i "$out" ::MMURTL.IMG | grep -oE '<[0-9]+' | head -1 | tr -d '<')
[ "$first" = "2" ] || { echo "MMURTL.IMG does not start at cluster 2 (got $first)" >&2; exit 1; }
mshowfat -i "$out" ::MMURTL.IMG | grep -qE '^.*<2-321>$' \
	|| { echo "MMURTL.IMG is not contiguous in clusters 2-321" >&2; exit 1; }

echo "$out"
