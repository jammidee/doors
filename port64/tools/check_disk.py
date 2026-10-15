#!/usr/bin/env python3
"""After `make test`: check, with the host's own FAT tools, what the DOORS kernel wrote to the
test disk (port64/kernel/selftest.c, M7 and user/progs/fsuser.c).

  python3 tools/check_disk.py build/disk-test.img

- fsck.fat -n on all three partitions must find nothing wrong;
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
        c, d, e = imgs

        for p, name in zip(imgs, ("C: FAT32", "D: FAT16", "E: FAT12")):
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
        check("C:\\BIG.BIN untouched", mtype(c, "BIG.BIN") == big_bin())
        check("D:\\D16.TXT = 3000 bytes of pattern 5", mtype(d, "D16.TXT") == pat(3000, 5))
        check("E:\\E12.BIN = 1 MB of pattern 11 (FAT12 chains)", mtype(e, "E12.BIN") == pat(1 << 20, 11))

        r = subprocess.run(["mdir", "-i", c, "::NEW.TXT"], env=ENV, stdout=subprocess.PIPE)
        m = re.search(rb"(\d{4})-(\d{2})-(\d{2})", r.stdout)
        year = int(m.group(1)) if m else 0
        check("C:\\NEW.TXT has a real date (from the RTC)", year >= 2024 and year <= datetime.date.today().year + 1,
              r.stdout.decode(errors="replace").strip())

    print("CHECK_DISK " + ("PASS" if not fails else f"FAIL ({len(fails)})"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
