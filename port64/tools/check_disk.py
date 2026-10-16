#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""After `make test`: check, with the host's own FAT and ext4 tools, what the DOORS kernel wrote to
the test disk (port64/kernel/selftest.c, M7/M8 and user/progs/fsuser.c).

  python3 tools/check_disk.py build/disk-test.img

- fsck.fat -n on the FAT partitions and e2fsck -fn on the ext4 one must find nothing wrong;
- the files written by DOORS must have the expected names (long ones too), sizes, contents and
  plausible dates; the files and directories it deleted must be gone.
"""
import datetime, os, re, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkdisk import PARTS, SEC, HELLO_TXT, big_bin  # noqa: E402

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")
fails = []


def pat(n, mul):
    return bytes(((i * mul) & 0xFF) for i in range(n))


def check(what, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + what + (f"  ({detail})" if detail and not ok else ""))
    if not ok:
        fails.append(what)


def mtype(img, path):
    r = subprocess.run(["mcopy", "-n", "-i", img, "::" + path, "-"], env=ENV,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.stdout if r.returncode == 0 else None


def exists(img, path):
    return subprocess.run(["mdir", "-i", img, "::" + path], env=ENV, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def debugfs(img, cmd):
    return subprocess.run(["debugfs", "-R", cmd, img], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE).stdout


def e4exists(img, path):
    r = subprocess.run(["debugfs", "-R", f'stat "{path}"', img], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    return b"File not found" not in r.stdout


def e4cat(img, path):
    return debugfs(img, f'cat "{path}"') if e4exists(img, path) else None


def e4size(img, path):
    m = re.search(rb"Size: (\d+)", debugfs(img, f'stat "{path}"'))
    return int(m.group(1)) if m else None


def e4block(img, path, lblk):
    """Logical block lblk of a file, found through its extent list (for files too big to cat)."""
    for line in debugfs(img, f'ex "{path}"').decode().splitlines():
        m = re.match(r"\s*\d+/\s*\d+\s+\d+/\s*\d+\s+(\d+) -\s*(\d+)\s+(\d+) -", line)
        if m and int(m.group(1)) <= lblk <= int(m.group(2)):
            with open(img, "rb") as f:
                f.seek((int(m.group(3)) + lblk - int(m.group(1))) * 4096)
                return f.read(4096)
    return None


def check_ext4(f4):
    r = subprocess.run(["e2fsck", "-fn", f4], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    check("e2fsck -fn F: ext4: no errors", r.returncode == 0,
          r.stdout.decode(errors="replace").strip().splitlines()[-3:])
    check("F:\\NEW4.BIN = 100000 bytes of pattern 13", e4cat(f4, "/NEW4.BIN") == pat(100000, 13))
    frag = b"".join(bytes([ord("a") + i]) * 4096 + (bytes(4096) if i < 11 else b"") for i in range(12))
    check("F:\\FRAG.BIN = 12 blocks with holes (extent tree)", e4cat(f4, "/FRAG.BIN") == frag)
    check("F:\\HUGE.BIN is 5 GB", e4size(f4, "/HUGE.BIN") == 5 << 30)
    check("F:\\HUGE.BIN data at 4.5 GB", e4block(f4, "/HUGE.BIN", (9 << 29) // 4096) == pat(4096, 17))
    check("F:\\Long name created by DOORS on ext4.txt",
          e4cat(f4, "/Long name created by DOORS on ext4.txt") == b"long ext4\n")
    check("F:\\Case.txt and F:\\case.txt both exist", e4exists(f4, "/Case.txt") and e4exists(f4, "/case.txt"))
    check("F:\\NEWDIR4 deleted", not e4exists(f4, "/NEWDIR4"))
    check("F:\\deep\\MOVED4\\IN.TXT (moved directory)", e4cat(f4, "/deep/MOVED4/IN.TXT") == pat(3, 1))
    check("F:\\MOVE4 gone after the move", not e4exists(f4, "/MOVE4"))
    check("F:\\TRUNC4.BIN = 5000 pattern bytes + 4000 zeros",
          e4cat(f4, "/TRUNC4.BIN") == pat(5000, 3) + bytes(4000))
    check("F:\\GONE.BIN deleted", not e4exists(f4, "/GONE.BIN"))
    check("F:\\BIG.BIN untouched", e4cat(f4, "/BIG.BIN") == big_bin(300000))


def main():
    disk = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        imgs = []
        with open(disk, "rb") as f:
            for i, (start, n, *_rest) in enumerate(PARTS):
                f.seek(start * SEC)
                p = os.path.join(tmp, f"p{i}.img")
                open(p, "wb").write(f.read(n * SEC))
                imgs.append(p)
        c, d, e, f4 = imgs

        for p, name in zip(imgs[:3], ("C: FAT32", "D: FAT16", "E: FAT12")):
            r = subprocess.run(["fsck.fat", "-n", p], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            out = r.stdout.decode(errors="replace")
            check(f"fsck.fat {name}: no errors", r.returncode == 0, out.strip().splitlines()[-3:])

        check("C:\\NEW.TXT = 10000 bytes of pattern 13", mtype(c, "NEW.TXT") == pat(10000, 13))
        check("C:\\COPY.TXT = HELLO.TXT (stream copy)", mtype(c, "COPY.TXT") == HELLO_TXT)
        check("C:\\COPYB.TXT = HELLO.TXT (block copy + SetFileSize)", mtype(c, "COPYB.TXT") == HELLO_TXT)
        check("C:\\Long name created by DOORS.txt", mtype(c, "Long name created by DOORS.txt") == b"long\n")
        t = mtype(c, "TRUNC.TXT")
        check("C:\\TRUNC.TXT = 100 pattern bytes + 4900 zeros",
              t == pat(100, 3) + bytes(4900), f"{len(t) if t else None} bytes")
        check("C:\\NEWDIR deleted", not exists(c, "NEWDIR"))
        check("C:\\DIR1\\MOVED\\INSIDE.TXT (moved directory)", mtype(c, "DIR1/MOVED/INSIDE.TXT") == b"inside\n")
        check("C:\\MOVEME gone after the move", not exists(c, "MOVEME"))
        check("C:\\USER.TXT written by ring 3", mtype(c, "USER.TXT") == b"written by a ring 3 program\n")
        check("C:\\T9\\H.TXT copied by the CLI", mtype(c, "T9/H.TXT") == HELLO_TXT)
        check("C:\\T9\\EDITED_BY_DOORS.TXT written by the editor (long name kept)",
              mtype(c, "T9/EDITED_BY_DOORS.TXT") == b"Edited in DOORS")
        check("C:\\STDIO.TXT gone (renamed and removed by jobtest)",
              not exists(c, "STDIO.TXT") and not exists(c, "STDIO2.TXT"))
        check("C:\\BIG.BIN untouched", mtype(c, "BIG.BIN") == big_bin())
        check("D:\\D16.TXT = 3000 bytes of pattern 5", mtype(d, "D16.TXT") == pat(3000, 5))
        check("E:\\E12.BIN = 1 MB of pattern 11 (FAT12 chains)", mtype(e, "E12.BIN") == pat(1 << 20, 11))

        r = subprocess.run(["mdir", "-i", c, "::NEW.TXT"], env=ENV, stdout=subprocess.PIPE)
        m = re.search(rb"(\d{4})-(\d{2})-(\d{2})", r.stdout)
        year = int(m.group(1)) if m else 0
        check("C:\\NEW.TXT has a real date (from the RTC)", year >= 2024 and year <= datetime.date.today().year + 1,
              r.stdout.decode(errors="replace").strip())

        check_ext4(f4)

    print("CHECK_DISK " + ("PASS" if not fails else f"FAIL ({len(fails)})"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
