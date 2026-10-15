#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""Port of MSCODE16/BOOTCODE/MAKEIMG.C: turn the MMURTL OS run file into a boot image.

The image is the OS exactly as it sits in memory after loading at linear 0:
  data segment, zero-padded to 64 KB, then code segment, zero-padded to 96 KB (160 KB total).
No fixups are applied (the OS is assembled at virtual address 0).

The prebuilt MSCODE16/BOOTCODE/MMURTL.IMG comes from an older kernel than OSSOURCE / MMSYS
(different LoadCLI and boot messages), so the floppy is built from MMSYS/MMURTL.RUN.

Usage: mkimg.py MMURTL.RUN MMURTL.IMG
"""
import struct, sys

IDTAG, SEGTAG, CODETAG, DATATAG, ENDTAG = 0x80, 0x90, 0xB0, 0xB2, 0xFF
DATA_MAX, CODE_MAX = 2 * 32768, 3 * 32768


def make_image(run):
    pos, code, data, ftype = 0, None, None, None
    while pos + 5 <= len(run):
        tid, length = struct.unpack_from("<BI", run, pos)
        pos += 5
        if tid == IDTAG:
            ftype = run[pos]; pos += 1      # MAKEIMG reads 1 byte regardless of length
            continue
        if tid == ENDTAG:
            break
        body = run[pos:pos + length]
        pos += length
        if tid == CODETAG:
            code = body
        elif tid == DATATAG:
            data = body
    else:
        sys.exit("Invalid OS Run file. No End Tag Found.")
    if ftype != 1:
        sys.exit("File MUST be a run file")
    if code is None or data is None:
        sys.exit("Run file has no code or data segment")
    if len(data) > DATA_MAX or len(code) > CODE_MAX:
        sys.exit(f"Segments too large: data {len(data)} (max {DATA_MAX}), code {len(code)} (max {CODE_MAX})")
    print(f"  Code    {len(code)}\n  Data    {len(data)}", file=sys.stderr)
    return data.ljust(DATA_MAX, b"\0") + code.ljust(CODE_MAX, b"\0")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        img = make_image(f.read())
    with open(sys.argv[2], "wb") as f:
        f.write(img)
