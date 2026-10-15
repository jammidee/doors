#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""Build the DOORS test disk: MBR + FAT32 (C:), FAT16 (D:), FAT12 (E:), made with the host's
mkfs.fat and mtools, so the kernel's FAT code is checked against an independent implementation.

  python3 tools/mkdisk.py OUT.img [--hello build/user/hello.elf]

Layout (512-byte sectors):  2048.. FAT32 40 MB | FAT16 16 MB | FAT12 4 MB | free (scratch)
Files:
  C:\\HELLO.TXT                 "Hello from a FAT32 file.\\n"
  C:\\A Long File Name.txt      long name (VFAT)
  C:\\DIR1\\SUB\\DEEP.TXT         nested directories
  C:\\BIG.BIN                   200000 bytes, byte i = (i * 7 + i // 251) & 0xFF (many clusters)
  C:\\BIN\\HELLO.ELF             the hello user program (LoadNewJob from a file)
  D:\\README.TXT, E:\\TINY.TXT
"""
import os, struct, subprocess, sys, tempfile

SEC = 512
PARTS = [  # (start, sectors, mbr type, fat bits, label)
    (2048, 81920, 0x0C, 32, "DOORS32"),
    (83968, 32768, 0x06, 16, "DOORS16"),
    (116736, 8192, 0x01, 12, "DOORS12"),
]
TOTAL = 196608  # 96 MB

HELLO_TXT = b"Hello from a FAT32 file.\n"
LONG_TXT = b"This file has a long name.\n"
DEEP_TXT = b"Three levels down.\n"
README_TXT = b"FAT16 volume D:\n"
TINY_TXT = b"FAT12 volume E:\n"


def big_bin(n=200000):
    return bytes(((i * 7 + i // 251) & 0xFF) for i in range(n))


def run(*cmd, **kw):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, **kw)


os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"   # mkfs.fat lives there


def env():
    e = dict(os.environ)
    e["MTOOLS_SKIP_CHECK"] = "1"
    e["MTOOLS_NO_VFAT"] = "0"
    return e


def put(img, data, path, tmp):
    src = os.path.join(tmp, "f")
    with open(src, "wb") as f:
        f.write(data)
    run("mcopy", "-i", img, src, "::" + path, env=env())


def main():
    out = sys.argv[1]
    hello = sys.argv[sys.argv.index("--hello") + 1] if "--hello" in sys.argv else None
    with tempfile.TemporaryDirectory() as tmp:
        imgs = []
        for i, (start, n, typ, bits, label) in enumerate(PARTS):
            p = os.path.join(tmp, f"p{i}.img")
            with open(p, "wb") as f:
                f.truncate(n * SEC)
            run("mkfs.fat", "-F", str(bits), "-n", label, "-h", str(start), "-S", "512", p)
            imgs.append(p)
        c, d, e = imgs
        put(c, HELLO_TXT, "HELLO.TXT", tmp)
        put(c, LONG_TXT, "A Long File Name.txt", tmp)
        run("mmd", "-i", c, "::DIR1", "::DIR1/SUB", "::BIN", env=env())
        put(c, DEEP_TXT, "DIR1/SUB/DEEP.TXT", tmp)
        put(c, big_bin(), "BIG.BIN", tmp)
        if hello:
            put(c, open(hello, "rb").read(), "BIN/HELLO.ELF", tmp)
        put(d, README_TXT, "README.TXT", tmp)
        put(e, TINY_TXT, "TINY.TXT", tmp)

        mbr = bytearray(SEC)
        for i, (start, n, typ, bits, label) in enumerate(PARTS):
            # CHS fields 0xFE/0xFF/0xFF ("use LBA"), as modern partitioners write
            mbr[446 + 16 * i:446 + 16 * (i + 1)] = struct.pack(
                "<B3sB3sII", 0x00, b"\xfe\xff\xff", typ, b"\xfe\xff\xff", start, n)
        mbr[510:512] = b"\x55\xaa"
        with open(out, "wb") as f:
            f.truncate(TOTAL * SEC)
            f.seek(0)
            f.write(mbr)
            for (start, n, *_), p in zip(PARTS, imgs):
                f.seek(start * SEC)
                f.write(open(p, "rb").read())
    print(out)


if __name__ == "__main__":
    main()
