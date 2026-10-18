#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""The undamaged disks for the file system fuzzer (tests/fsfuzz, `make fuzz`, ROADMAP step 1.4).

  python3 tools/fuzzdisk.py OUTDIR      -> OUTDIR/fat.img, OUTDIR/ext4.img, OUTDIR/ext4c.img

Each is an MBR disk whose volumes hold the structures worth damaging:
  fat.img    FAT12 4 MB, FAT16 16 MB, FAT32 36 MB: long names, nested directories, a directory of
             150 files (several clusters), a fragmented file (cluster chain with jumps)
  ext4.img   ext4 4K blocks 16 MB and ext4 1K blocks 8 MB, without metadata_csum (so damage reaches
             the parsing code instead of stopping at a checksum): htree directories (e2fsck -D), a
             file of 300 one-block extents (an extent tree with index blocks), a symlink, deep
             directories; the 4K volume has a committed, unreplayed journal transaction
  ext4c.img  the same with metadata_csum and a csum_v3 journal (debugfs jo -c), as Linux makes them
"""
import os, struct, subprocess, sys, tempfile

SEC = 512
os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"


def run(*cmd, **kw):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)


def disk(out, parts):
    """parts: [(mbr type, partition image)] -> an MBR disk, partitions from sector 2048 on."""
    mbr = bytearray(SEC)
    start, layout = 2048, []
    for i, (typ, p) in enumerate(parts):
        n = os.path.getsize(p) // SEC
        mbr[446 + 16 * i:462 + 16 * i] = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", typ,
                                                     b"\xfe\xff\xff", start, n)
        layout.append((start, p))
        start += n
    mbr[510:512] = b"\x55\xaa"
    with open(out, "wb") as f:
        f.truncate((start + 2048) * SEC)
        f.write(mbr)
        for s, p in layout:
            f.seek(s * SEC)
            f.write(open(p, "rb").read())


def blank(p, mb):
    with open(p, "wb") as f:
        f.truncate(mb << 20)


def tree(root, n_files):
    """A directory tree for mkfs.ext4 -d."""
    for d in ("deep/a/b/c/d", "BIG", "small"):
        os.makedirs(os.path.join(root, d))
    for i in range(n_files):
        with open(os.path.join(root, "BIG", f"file number {i:04} with a long name.txt"), "wb") as f:
            f.write(b"x" * (i % 7))
    for i in range(5):
        with open(os.path.join(root, "small", f"S{i}.TXT"), "wb") as f:
            f.write(bytes([65 + i]) * (1000 * i + 1))
    with open(os.path.join(root, "deep/a/b/c/d/DEEP.TXT"), "wb") as f:
        f.write(b"deep\n")
    with open(os.path.join(root, "FRAG.BIN"), "wb") as f:      # 300 one-block extents (holes between)
        for i in range(300):
            f.seek(i * 2 * 4096)
            f.write(bytes([i & 0xFF]) * 4096)
    with open(os.path.join(root, "HELLO.TXT"), "wb") as f:
        f.write(b"hello, ext4\n")
    os.symlink("HELLO.TXT", os.path.join(root, "LINK"))


def ext4(p, mb, bs, csum, tmp, journal_txn):
    root = os.path.join(tmp, f"root{bs}{csum}")
    tree(root, 400 if bs == 1024 else 250)
    blank(p, mb)
    feats = "metadata_csum" if csum else "^metadata_csum,^uninit_bg"
    run("mkfs.ext4", "-q", "-F", "-b", str(bs), "-I", "256", "-O", feats, "-d", root, p)
    subprocess.run(["e2fsck", "-fyD", p], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if journal_txn:                     # a committed transaction that the mount must replay
        blk = subprocess.run(["debugfs", "-R", "bmap /HELLO.TXT 0", p], check=True, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL).stdout.decode().strip()
        nb = os.path.join(tmp, "nb")
        with open(nb, "wb") as f:
            f.write(b"replayed!!!\n".ljust(bs, b"\0"))
        cmds = os.path.join(tmp, "jc")
        with open(cmds, "w") as f:
            f.write(("jo -c\n" if csum else "jo\n") + f"jw -b {blk} {nb}\njc\n")
        run("debugfs", "-w", "-f", cmds, p)


def fat(p, mb, bits, label, start, tmp):
    blank(p, mb)
    spc = "4" if bits == 12 else "1"   # FAT12: at most 4084 clusters
    run("mkfs.fat", "-F", str(bits), "-n", label, "-h", str(start), "-S", "512", "-s", spc, p)
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    m = lambda *a: run(*a, env=env)
    m("mmd", "-i", p, "::DIR1", "::DIR1/SUB", "::DIR1/SUB/DEEPER", "::MANY")
    src = os.path.join(tmp, f"src{bits}")
    os.makedirs(src)
    def put(name, data):
        f = os.path.join(src, "x")
        with open(f, "wb") as o:
            o.write(data)
        m("mcopy", "-i", p, f, "::" + name)
    put("HELLO.TXT", b"hello, FAT\n")
    put("A Long File Name.txt", b"long\n")
    put("DIR1/SUB/DEEPER/DEEP.TXT", b"deep\n")
    for i in range(150 if bits != 12 else 40):
        put(f"MANY/F{i:03}.TXT", bytes([48 + i % 10]) * (i * 13))
    for i in range(8):                  # fragment: small files, every other one deleted, then a big one
        put(f"GAP{i}.TMP", b"g" * 3000)
    for i in range(0, 8, 2):
        m("mdel", "-i", p, f"::GAP{i}.TMP")
    put("FRAG.BIN", bytes(range(256)) * 200)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        f12, f16, f32 = (os.path.join(tmp, f"f{b}.img") for b in (12, 16, 32))
        fat(f12, 4, 12, "FZ12", 2048, tmp)
        fat(f16, 16, 16, "FZ16", 2048 + 8192, tmp)
        fat(f32, 36, 32, "FZ32", 2048 + 8192 + 32768, tmp)
        disk(os.path.join(out, "fat.img"), [(0x01, f12), (0x06, f16), (0x0C, f32)])
        for name, csum in (("ext4.img", False), ("ext4c.img", True)):
            a, b = os.path.join(tmp, "a.img"), os.path.join(tmp, "b.img")
            ext4(a, 16, 4096, csum, tmp, True)
            ext4(b, 8, 1024, csum, tmp, False)
            disk(os.path.join(out, name), [(0x83, a), (0x83, b)])
            os.remove(a)
            os.remove(b)
    for n in ("fat.img", "ext4.img", "ext4c.img"):
        print(os.path.join(out, n))


if __name__ == "__main__":
    main()
