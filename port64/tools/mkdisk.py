#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""Build the DOORS test disk: MBR + FAT32 (C:), FAT16 (D:), FAT12 (E:), made with the host's
mkfs.fat and mtools, so the kernel's FAT code is checked against an independent implementation.

  python3 tools/mkdisk.py OUT.img [--hello build/user/hello.elf] [--progs build/user] [--sys user/sys]
                          [--plain-journal] [--damage] [--disk2 OUT2.img]
  --damage leaves E: as after a crash (damage_fat12; `make test` checks DOORS's report of it)
  python3 tools/mkdisk.py OUT.img --system ext4 --boot build/doors.iso.d/boot [--hello ...] [--progs ...]
                          [--sys ...]     a disk that boots by itself, C: = ext4 (see make_sysdisk)

F:'s journal has CRC32C checksums (csum_v3), as e2fsprogs 1.47 makes it and Linux's mount sets it
on metadata_csum volumes; --plain-journal leaves it without checksums (as e2fsprogs 1.46 makes it).

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
import os, shutil, struct, subprocess, sys, tempfile

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
EXT_CSUMBAD_OLD = b"not replayed\n"     # F:\CSUMBAD.TXT: a second transaction rewrites it, but its
EXT_CSUMBAD_NEW = b"BAD REPLAY!!\n"     # logged block is damaged, so its checksum fails (csum_v3)
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


# ---- FAT12 volumes left as after a crash: the dirty flag and one of each problem DOORS's mount
# check (kernel/fat.c fat_check) reports; tools/check_disk.py checks that fsck.fat finds the same.
#   E: (stays writable, DOORS's FAT12 write test runs on it):
#     LONG.TXT   100 bytes, a 2-cluster chain              (chain longer than the size)
#     SHORT.TXT  5 clusters' size, a 1-cluster chain       (chain too short)
#     one cluster in use in FAT 1 only                     (lost; the FAT copies differ)
#   H: (disk 2; problems that make DOORS mount it read-only):
#     FREE.TXT   3 clusters, the first one marked free     (a free cluster in the chain; too short)
#     SHARE.TXT  an entry for ORIG.TXT's chain             (two files share clusters)
#     lost: FREE's last 2 clusters
# (A chain starting with a bad cluster is reported by DOORS too, but fsck.fat 4.2 aborts on it:
# "Internal error: next_cluster on bad cluster".)

def damage_fat12(img, tmp, unsafe):
    d = bytearray(open(img, "rb").read())
    spc = d[13]
    cs = spc * SEC
    if unsafe:
        for n in ("FREE.TXT", "ORIG.TXT"):
            put(img, b"x" * (2 * cs + 100), n, tmp)
    else:
        put(img, b"L" * 100, "LONG.TXT", tmp)
        put(img, b"S" * 100, "SHORT.TXT", tmp)
    d = bytearray(open(img, "rb").read())
    rsvd, nfats = struct.unpack_from("<H", d, 14)[0], d[16]
    rootents, fatsz = struct.unpack_from("<H", d, 17)[0], struct.unpack_from("<H", d, 22)[0]
    d = bytearray(open(img, "rb").read())
    total = struct.unpack_from("<H", d, 19)[0] or struct.unpack_from("<I", d, 32)[0]
    root = (rsvd + nfats * fatsz) * SEC
    nclus = (total - rsvd - nfats * fatsz - (rootents * 32 + 511) // 512) // spc

    def fat_set(c, v, copies=range(nfats)):
        for k in copies:
            o = (rsvd + k * fatsz) * SEC + c * 3 // 2
            w = struct.unpack_from("<H", d, o)[0]
            w = (w & 0x000F) | (v << 4) if c & 1 else (w & 0xF000) | v
            struct.pack_into("<H", d, o, w)

    def entry(name):
        n11 = (name.split(".")[0].ljust(8) + name.split(".")[1].ljust(3)).encode()
        for i in range(rootents):
            if d[root + 32 * i:root + 32 * i + 11] == n11:
                return root + 32 * i
        raise SystemExit("mkdisk: E:" + name + " not found")

    first = lambda name: struct.unpack_from("<H", d, entry(name) + 26)[0]
    if unsafe:
        fat_set(first("FREE.TXT"), 0)
        o = entry("ORIG.TXT")
        free = next(root + 32 * i for i in range(rootents) if d[root + 32 * i] in (0, 0xE5))
        d[free:free + 32] = d[o:o + 32]
        d[free:free + 11] = b"SHARE   TXT"
    else:
        c1, c2 = nclus + 1, nclus               # free clusters at the end of the volume
        fat_set(first("LONG.TXT"), c1)
        fat_set(c1, 0xFFF)
        struct.pack_into("<I", d, entry("SHORT.TXT") + 28, 5 * cs)
        fat_set(c2, 0xFFF, copies=[0])
    d[37] |= 1                                  # the dirty flag (FAT12/16: byte 37)
    open(img, "wb").write(d)


# ---- the second test disk (device 13): GPT; G: ext4 with 1K blocks, H: a damaged FAT12 ----
# ONE and TWO are htree directories (e2fsck -D indexes them): ONE has one index level, TWO two.
D2_TOTAL = 131072                # 64 MB
D2_START, D2_SECTORS = 2048, 65536
D2_H_START, D2_H_SECTORS = D2_START + D2_SECTORS, 8192
# I: a small ext4 (4K blocks, 128 inodes, journal, metadata_csum) for the full-volume tests (ROADMAP
# 1.7): filling F: or emptying 4,000 inodes would take minutes through ATA PIO
D2_I_START, D2_I_SECTORS = D2_H_START + D2_H_SECTORS, 32768
D2_README = b"1K-block ext4 on a GPT disk.\n"


def htree_name(prefix, i, fill):
    """Long names (197 bytes) so a few hundred fill many 1K leaves (kernel/selftest.c: same)."""
    return f"{prefix}{i:04}" + fill * 190


def gpt_disk(out, parts):
    """parts: [(start, sectors, type GUID, name, partition image)]"""
    import uuid, zlib
    entries = bytearray(128 * 128)
    for i, (start, n, typ, pname, _) in enumerate(parts):
        entries[128 * i:128 * (i + 1)] = (uuid.UUID(typ).bytes_le + uuid.uuid4().bytes_le +
                                          struct.pack("<QQQ", start, start + n - 1, 0) +
                                          pname.encode("utf-16-le").ljust(72, b"\0"))
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
        for start, _, _, _, img in parts:
            f.seek(start * SEC)
            f.write(open(img, "rb").read())


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
    h = os.path.join(tmp, "h.img")             # H: FAT12, dirty, with the problems that make it read-only
    with open(h, "wb") as f:
        f.truncate(D2_H_SECTORS * SEC)
    run("mkfs.fat", "-F", "12", "-n", "DOORSH", "-h", str(D2_H_START), "-S", "512", h)
    put(h, b"intact\n", "OK.TXT", tmp)
    damage_fat12(h, tmp, True)
    i4 = os.path.join(tmp, "i.img")
    with open(i4, "wb") as f:
        f.truncate(D2_I_SECTORS * SEC)
    run("mkfs.ext4", "-q", "-F", "-T", "default", "-b", "4096", "-I", "256", "-N", "128", "-L", "DOORS-SMALL",
        i4, str(D2_I_SECTORS // 8))
    gpt_disk(out, [(D2_START, D2_SECTORS, "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "DOORS-1K", img),
                   (D2_H_START, D2_H_SECTORS, "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "DOORS-H", h),
                   (D2_I_START, D2_I_SECTORS, "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "DOORS-SMALL", i4)])


def e4bmap(img, args):
    """debugfs bmap: the physical block of "FILE LBLK" (FILE may be <INODE>)."""
    return subprocess.run(["debugfs", "-R", "bmap " + args, img], check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL).stdout.decode().strip()


def opt(name):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else None


# ---- an ext4 system disk that boots by itself (ROADMAP 1.6: --system ext4 --boot DIR) ----
# MBR with GRUB, C: ext4 (the system: /boot with the kernel, modules and GRUB's files, \DOORS\,
# \MSAMPLES\), D: FAT16 for exchanging files. GRUB is installed as grub-bios-setup would do it, by
# hand (no loop devices in WSL): boot.img's code (bytes 0..0x1B7, not its tail, which would overlap
# the partition table) in the MBR, with the sector of core.img at 0x5C; core.img (grub-mkimage with
# biosdisk, part_msdos, ext2 and prefix (hd0,msdos1)/boot/grub) in the gap before partition 1.
# grub-mkimage already writes core.img's own length into its first sector's block list.
SYS_PARTS = [(2048, 131072, 0x83), (133120, 32768, 0x06)]      # C: ext4 64 MB, D: FAT16 16 MB
GRUB_DIR = "/usr/lib/grub/i386-pc"


def make_sysdisk(out, boot, progs, sysdir, hello, repo, tmp):
    tree = os.path.join(tmp, "sysroot")
    shutil.copytree(boot, os.path.join(tree, "boot"))
    gmods = os.path.join(tree, "boot", "grub", "i386-pc")
    os.makedirs(gmods, exist_ok=True)
    for f in os.listdir(GRUB_DIR):
        if f.endswith((".mod", ".lst")):
            shutil.copy(os.path.join(GRUB_DIR, f), gmods)
    os.makedirs(os.path.join(tree, "DOORS"))
    for d_, pat_ in ((sysdir, None), (progs, ".elf")):
        if d_:
            for name in sorted(os.listdir(d_)):
                p_ = os.path.join(d_, name)
                if os.path.isfile(p_) and (pat_ is None or name.endswith(pat_)):
                    shutil.copy(p_, os.path.join(tree, "DOORS", name.upper()))
    os.makedirs(os.path.join(tree, "MSAMPLES"))
    readme = os.path.join(repo, "MSAMPLES", "READ.ME")
    if os.path.exists(readme):
        shutil.copy(readme, os.path.join(tree, "MSAMPLES", "READ.ME"))
    os.makedirs(os.path.join(tree, "BIN"))
    if hello:
        shutil.copy(hello, os.path.join(tree, "BIN", "HELLO.ELF"))
    with open(os.path.join(tree, "HELLO.TXT"), "wb") as f:
        f.write(b"Hello from the ext4 system disk.\n")

    c, d = os.path.join(tmp, "c.img"), os.path.join(tmp, "d.img")
    for p, (start, n, _) in ((c, SYS_PARTS[0]), (d, SYS_PARTS[1])):
        with open(p, "wb") as f:
            f.truncate(n * SEC)
    run("mkfs.ext4", "-q", "-F", "-T", "default", "-b", "4096", "-I", "256", "-L", "DOORS-SYS", "-d", tree, c)
    run("mkfs.fat", "-F", "16", "-n", "DOORSXCH", "-h", str(SYS_PARTS[1][0]), "-S", "512", d)
    put(d, README_TXT, "README.TXT", tmp)
    core = os.path.join(tmp, "core.img")
    run("grub-mkimage", "-O", "i386-pc", "-o", core, "-p", "(hd0,msdos1)/boot/grub",
        "biosdisk", "part_msdos", "ext2")
    core_b = open(core, "rb").read()
    if len(core_b) > (SYS_PARTS[0][0] - 1) * SEC:
        raise SystemExit("mkdisk: GRUB's core.img doesn't fit before partition 1")

    mbr = bytearray(SEC)
    mbr[:0x1B8] = open(os.path.join(GRUB_DIR, "boot.img"), "rb").read()[:0x1B8]
    struct.pack_into("<Q", mbr, 0x5C, 1)        # core.img starts at LBA 1
    for i, (start, n, typ) in enumerate(SYS_PARTS):
        mbr[446 + 16 * i:462 + 16 * i] = struct.pack("<B3sB3sII", 0x80 if i == 0 else 0, b"\xfe\xff\xff",
                                                     typ, b"\xfe\xff\xff", start, n)
    mbr[510:512] = b"\x55\xaa"
    end = SYS_PARTS[-1][0] + SYS_PARTS[-1][1]
    with open(out, "wb") as f:
        f.truncate((end + 2048) * SEC)
        f.write(mbr)
        f.write(core_b)
        for (start, _, _), p in zip(SYS_PARTS, (c, d)):
            f.seek(start * SEC)
            f.write(open(p, "rb").read())


def main():
    out = sys.argv[1]
    hello = opt("--hello")
    progs = opt("--progs")      # build/user: every NAME.elf goes to C:\DOORS\NAME.ELF
    sysdir = opt("--sys")       # user/sys: HELP.CLI, COMMANDS.CLI go to C:\DOORS\
    repo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    if opt("--system") == "ext4":
        with tempfile.TemporaryDirectory() as tmp:
            make_sysdisk(out, opt("--boot"), progs, sysdir, hello, repo, tmp)
        print(out)
        return
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
        if "--damage" in sys.argv:              # make test only: E: as after a crash
            damage_fat12(e, tmp, False)

        # F: ext4 with Linux's default features, filled from a directory tree
        tree = os.path.join(tmp, "ext4root")
        for sub in ("deep/dir/tree", "BIN"):
            os.makedirs(os.path.join(tree, sub))
        files = {"HELLO.TXT": EXT_HELLO, "A long file name on ext4.txt": EXT_LONG,
                 "deep/dir/tree/FILE.TXT": EXT_DEEP, "MixedCase.Txt": EXT_CASE,
                 "JOURNAL.TXT": EXT_JOURNAL_OLD, "CSUMBAD.TXT": EXT_CSUMBAD_OLD,
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
        # With checksums (jo -c: csum_v3), a second transaction rewrites CSUMBAD.TXT, and its logged
        # block (journal block 5: descriptor 4, data 5, commit 6) is then damaged: the replay must
        # end the log there and leave CSUMBAD.TXT as it was.
        csum = "--plain-journal" not in sys.argv
        cmds = os.path.join(tmp, "jcmds")
        with open(cmds, "w") as cf:
            cf.write("jo -c\n" if csum else "jo\n")
            for path, data in (("/JOURNAL.TXT", EXT_JOURNAL_NEW),
                               ("/CSUMBAD.TXT", EXT_CSUMBAD_NEW))[:2 if csum else 1]:
                newblk = os.path.join(tmp, "newblk" + path[1:])
                with open(newblk, "wb") as nb:
                    nb.write(data.ljust(4096, b"\0"))
                cf.write(f"jw -b {e4bmap(f4, path + ' 0')} {newblk}\n")
            cf.write("jc\n")
        run("debugfs", "-w", "-f", cmds, f4, stderr=subprocess.DEVNULL)
        if csum:
            with open(f4, "r+b") as img:
                img.seek(int(e4bmap(f4, "<8> 5")) * 4096 + 100)
                img.write(b"X")

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
