#!/usr/bin/env bash
# Boot the original 32-bit MMURTL V1.0 in QEMU (floppy A: boots the OS, IDE disk is C:).
# Builds the disk images if missing. A QEMU window opens (WSLg on Windows 11).
# The monitor socket /tmp/mmurtl.mon lets tools/boot32/qscreen.py read the screen and send keys.
#
# Usage: tools/boot32/run.sh [--rebuild] [extra qemu args...]
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
build="$here/../../build/boot32"
fd="$build/mmurtl-floppy.img" hd="$build/mmurtl-hdd.img"

if [ "${1:-}" = "--rebuild" ]; then shift; rm -f "$fd" "$hd"; fi
[ -f "$fd" ] || bash "$here/mkfloppy.sh" "$fd"
[ -f "$hd" ] || bash "$here/mkhdd.sh" "$hd"

exec qemu-system-i386 -name "MMURTL V1.0" -m 16 -net none \
	-drive file="$fd",if=floppy,format=raw \
	-drive file="$hd",if=none,id=hd0,format=raw \
	-device ide-hd,drive=hd0,bus=ide.0,unit=0,cyls=216,heads=16,secs=17,bios-chs-trans=none \
	-boot a -monitor unix:/tmp/mmurtl.mon,server,nowait "$@"
