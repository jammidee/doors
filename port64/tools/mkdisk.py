#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""Build the DOORS test disk: MBR + FAT32 (C:), FAT16 (D:), FAT12 (E:), made with the host's
mkfs.fat and mtools, so the kernel's FAT code is checked against an independent implementation.

  python3 tools/mkdisk.py OUT.img [--hello build/user/hello.elf] [--progs build/user] [--sys user/sys]

Layout (512-byte sectors):  2048.. FAT32 40 MB | FAT16 16 MB | FAT12 4 MB | ext4 64 MB | free (scratch)
Files:
  C:\\HELLO.TXT                 "Hello from a FAT32 file.\\n"
  C:\\A Long File Name.txt      long name (VFAT)
  C:\\DIR1\\SUB\\DEEP.TXT         nested directories
  C:\\BIG.BIN                   200000 bytes, byte i = (i * 7 + i // 251) & 0xFF (many clusters)
  C:\\BIN\\HELLO.ELF             the hello user program (LoadNewJob from a file)
  C:\\DOORS\\                    system directory: every program (--progs), HELP.CLI and
                                COMMANDS.CLI (--sys); the CLI is C:\\DOORS\\CLI.ELF
  C:\\MSAMPLES\\READ.ME          MMURTL's sample READ.ME (smoke test)
  D:\\README.TXT, E:\\TINY.TXT
"""
import os, struct, subprocess, sys, tempfile

SEC = 512
PARTS = [  # (start, sectors, mbr type, fat bits, label)
    (2048, 81920, 0x0C, 32, "DOORS32"),
    (83968, 32768, 0x06, 16, "DOORS16"),
    (116736, 8192, 0x01, 12, "DOORS12"),
    (124928, 131072, 0x83, "ext4", "DOORS-EXT4"),	# F: ext4 (mkfs.ext4 -T default)
]
TOTAL = 262144  # 128 MB

HELLO_TXT = b"Hello from a FAT32 file.\n"
LONG_TXT = b"This file has a long name.\n"
DEEP_TXT = b"Three levels down.\n"
README_TXT = b"FAT16 volume D:\n"
EXT_HELLO = b"Hello from ext4.\n"
EXT_LONG = b"A long name on ext4.\n"
EXT_DEEP = b"Deep in an ext4 tree.\n"
EXT_CASE = b"case matters\n"
EXT_JOURNAL_OLD = b"before replay\n"    # F:\JOURNAL.TXT as mkfs writes it ...
EXT_JOURNAL_NEW = b"after replay!\n"    # ... and as the pending journal transaction rewrites it
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


# ---- the second test disk (device 13, volume G:): GPT, one ext4 partition with 1K blocks ----
# ONE and TWO are htree directories (e2fsck -D indexes them): ONE has one index level, TWO two.
D2_TOTAL = 98304                 # 48 MB
D2_START, D2_SECTORS = 2048, 65536
D2_README = b"1K-block ext4 on a GPT disk.\n"


def htree_name(prefix, i, fill):
    """Long names (197 bytes) so a few hundred fill many 1K leaves (kernel/selftest.c: same)."""
    return f"{prefix}{i:04}" + fill * 190


def gpt_disk(out, part_img):
    import uuid, zlib
    entries = bytearray(128 * 128)
    linux = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4").bytes_le
    name = "DOORS-1K".encode("utf-16-le")
    entries[0:128] = (linux + uuid.uuid4().bytes_le +
                      struct.pack("<QQQ", D2_START, D2_START + D2_SECTORS - 1, 0) + name.ljust(72, b"\0"))
    ecrc = zlib.crc32(entries) & 0xFFFFFFFF
    disk_guid = uuid.uuid4().bytes_le

    def header(my, alt, elba):
        h = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, my, alt,
                                  34, D2_TOTAL - 34, disk_guid, elba, 128, 128, ecrc))
        h[16:20] = struct.pack("<I", zlib.crc32(bytes(h)) & 0xFFFFFFFF)
        return bytes(h).ljust(SEC, b"\0")

    mbr = bytearray(SEC)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, D2_TOTAL - 1)
    mbr[510:512] = b"\x55\xaa"
    with open(out, "wb") as f:
        f.truncate(D2_TOTAL * SEC)
        f.seek(0)
        f.write(mbr)
        f.write(header(1, D2_TOTAL - 1, 2))
        f.write(entries)
        f.seek((D2_TOTAL - 33) * SEC)
        f.write(entries)
        f.write(header(D2_TOTAL - 1, 1, D2_TOTAL - 33))
        f.seek(D2_START * SEC)
        f.write(open(part_img, "rb").read())


def make_disk2(out, tmp):
    tree = os.path.join(tmp, "d2tree")
    for sub in ("ONE", "TWO"):
        os.makedirs(os.path.join(tree, sub))
    for i in range(1, 151):
        open(os.path.join(tree, "ONE", htree_name("one", i, "o")), "wb").close()
    for i in range(1, 601):
        open(os.path.join(tree, "TWO", htree_name("two", i, "t")), "wb").close()
    with open(os.path.join(tree, "README.TXT"), "wb") as f:
        f.write(D2_README)
    img = os.path.join(tmp, "d2.img")
    with open(img, "wb") as f:
        f.truncate(D2_SECTORS * SEC)
    run("mkfs.ext4", "-q", "-F", "-T", "default", "-b", "1024", "-I", "256", "-L", "DOORS-1K",
        "-d", tree, img, str(D2_SECTORS // 2))
    subprocess.run(["e2fsck", "-fyD", img], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    gpt_disk(out, img)


def opt(name):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else None


def main():
    out = sys.argv[1]
    hello = opt("--hello")
    progs = opt("--progs")      # build/user: every NAME.elf goes to C:\DOORS\NAME.ELF
    sysdir = opt("--sys")       # user/sys: HELP.CLI, COMMANDS.CLI go to C:\DOORS\
    repo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    disk2 = opt("--disk2")      # also build the second test disk (GPT, 1K-block ext4 with htrees)
    if disk2:
        with tempfile.TemporaryDirectory() as tmp:
            make_disk2(disk2, tmp)
    with tempfile.TemporaryDirectory() as tmp:
        imgs = []
        for i, (start, n, typ, bits, label) in enumerate(PARTS):
            p = os.path.join(tmp, f"p{i}.img")
            with open(p, "wb") as f:
                f.truncate(n * SEC)
            imgs.append(p)
            if bits == "ext4":
                continue                # made below, from a directory tree
            run("mkfs.fat", "-F", str(bits), "-n", label, "-h", str(start), "-S", "512", p)
        c, d, e, f4 = imgs
        put(c, HELLO_TXT, "HELLO.TXT", tmp)
        put(c, LONG_TXT, "A Long File Name.txt", tmp)
        run("mmd", "-i", c, "::DIR1", "::DIR1/SUB", "::BIN", env=env())
        put(c, DEEP_TXT, "DIR1/SUB/DEEP.TXT", tmp)
        put(c, big_bin(), "BIG.BIN", tmp)
        if hello:
            put(c, open(hello, "rb").read(), "BIN/HELLO.ELF", tmp)
        # the system directory (MMURTL's \MMSYS\): the CLI, its files, the programs
        run("mmd", "-i", c, "::DOORS", "::MSAMPLES", env=env())
        for d_, pat_ in ((sysdir, None), (progs, ".elf")):
            if d_:
                for name in sorted(os.listdir(d_)):
                    p_ = os.path.join(d_, name)
                    if os.path.isfile(p_) and (pat_ is None or name.endswith(pat_)):
                        put(c, open(p_, "rb").read(), "DOORS/" + name.upper(), tmp)
        readme = os.path.join(repo, "MSAMPLES", "READ.ME")
        if os.path.exists(readme):
            put(c, open(readme, "rb").read(), "MSAMPLES/READ.ME", tmp)
        put(d, README_TXT, "README.TXT", tmp)
        put(e, TINY_TXT, "TINY.TXT", tmp)

        # F: ext4 with Linux's default features, filled from a directory tree
        tree = os.path.join(tmp, "ext4root")
        for sub in ("deep/dir/tree", "BIN"):
            os.makedirs(os.path.join(tree, sub))
        files = {"HELLO.TXT": EXT_HELLO, "A long file name on ext4.txt": EXT_LONG,
                 "deep/dir/tree/FILE.TXT": EXT_DEEP, "MixedCase.Txt": EXT_CASE,
                 "JOURNAL.TXT": EXT_JOURNAL_OLD,
                 "BIG.BIN": big_bin(300000)}
        if hello:
            files["BIN/HELLO.ELF"] = open(hello, "rb").read()
        for name, data in files.items():
            with open(os.path.join(tree, name), "wb") as out_f:
                out_f.write(data)
        with open(os.path.join(tree, "SPARSE.BIN"), "wb") as sp:   # 4 KB data, 1 MB hole, 4 KB data
            sp.write(b"A" * 4096)
            sp.seek(4096 + (1 << 20))
            sp.write(b"B" * 4096)
        run("mkfs.ext4", "-q", "-F", "-T", "default", "-b", "4096", "-I", "256", "-L", PARTS[3][4],
            "-d", tree, f4, str(PARTS[3][1] // 8))
        # A transaction in the journal that Linux committed but never replayed (as after a crash):
        # it rewrites JOURNAL.TXT's block, and leaves needs_recovery set. DOORS must replay it.
        blk = subprocess.run(["debugfs", "-R", "bmap /JOURNAL.TXT 0", f4], check=True,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.decode().strip()
        newblk = os.path.join(tmp, "newblk")
        with open(newblk, "wb") as nb:
            nb.write(EXT_JOURNAL_NEW.ljust(4096, b"\0"))
        cmds = os.path.join(tmp, "jcmds")
        with open(cmds, "w") as cf:
            cf.write(f"jo\njw -b {blk} {newblk}\njc\n")
        run("debugfs", "-w", "-f", cmds, f4, stderr=subprocess.DEVNULL)

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
