#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""Soak test (ROADMAP 2.3, `make soak SOAK_MINUTES=M`).

Boots build/doors-soak.iso ("soak minutes=M", kernel/soak.c) on a fresh disk and waits. The kernel
runs a random mix of programs for M minutes, kills them at random, and checks its invariants and
resource counts. The run fails on a panic, "SOAK FAIL", a program reporting bad data ("MISMATCH")
or an error it shouldn't get ("CHURN ERROR"), or a hang (no "SOAK PASS" by M minutes + 5).
Afterwards Linux checks the disk: fsck.fat -n on C: and e2fsck -fn on F: must be clean.

Usage (in WSL, from port64/):  python3 tools/soak.py MINUTES
"""
import os, re, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
from mkdisk import PARTS, SEC  # noqa: E402

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
DISK = "/tmp/doors-soak.img"
LOG = "/tmp/doors-soak.log"


def part(i, dst):
    s, n = PARTS[i][0], PARTS[i][1]
    with open(DISK, "rb") as f, open(dst, "wb") as o:
        f.seek(s * SEC)
        o.write(f.read(n * SEC))


def main():
    minutes = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    subprocess.run([sys.executable, os.path.join(HERE, "mkdisk.py"), DISK,
                    "--hello", os.path.join(PORT, "build", "user", "hello.elf"),
                    "--progs", os.path.join(PORT, "build", "user"),
                    "--sys", os.path.join(PORT, "user", "sys")], check=True, stdout=subprocess.DEVNULL)
    if os.path.exists(LOG):
        os.remove(LOG)
    q = subprocess.Popen(["qemu-system-x86_64", "-m", "256M", "-display", "none", "-no-reboot",
                          "-cdrom", os.path.join(PORT, "build", "doors-soak.iso"),
                          "-drive", f"file={DISK},format=raw,if=ide,index=0,cache=unsafe", "-boot", "d",
                          "-serial", "file:" + LOG, "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end, shown = time.time() + minutes * 60 + 300, 0
    while time.time() < end and q.poll() is None:
        time.sleep(2)
        log = open(LOG, errors="replace").read() if os.path.exists(LOG) else ""
        for l in re.findall(r"SOAK: round .*", log)[shown:]:
            print(l, flush=True)
        shown = len(re.findall(r"SOAK: round .*", log))
        if re.search(r"DOORS PANIC|SOAK FAIL|MISMATCH|CHURN ERROR", log):
            time.sleep(2)
            break
    if q.poll() is None:
        q.kill()
    q.wait()
    log = open(LOG, errors="replace").read() if os.path.exists(LOG) else ""
    fails = re.findall(r".*(?:DOORS PANIC|SOAK FAIL|MISMATCH|CHURN ERROR).*", log)
    if q.returncode != 33 or "SOAK PASS" not in log or fails:
        why = fails[:4] or ["no SOAK PASS (hang?), QEMU status %s" % q.returncode]
        print("FAIL  " + "\n      ".join(why))
        print("SOAKTEST FAIL")
        sys.exit(1)
    print(re.search(r"SOAK PASS.*", log).group(0))
    ops = sum(int(n) for n in re.findall(r"churn \d+: (\d+) operations", log))
    print(f"churn programs reported {ops} operations in steps of 200 (data verified)")
    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        c, f4 = os.path.join(tmp, "c.img"), os.path.join(tmp, "f.img")
        part(0, c)
        part(3, f4)
        r = subprocess.run(["fsck.fat", "-n", c], capture_output=True, text=True)
        if r.returncode:
            bad.append("fsck.fat C: " + " | ".join(r.stdout.strip().splitlines()[-3:]))
        r = subprocess.run(["e2fsck", "-fn", f4], capture_output=True, text=True)
        if r.returncode:
            bad.append("e2fsck F: " + " | ".join(r.stdout.strip().splitlines()[-3:]))
    if bad:
        print("FAIL  " + "\n      ".join(bad))
        print("SOAKTEST FAIL")
        sys.exit(1)
    print("fsck.fat C: and e2fsck -fn F: clean")
    print("SOAKTEST PASS")


if __name__ == "__main__":
    main()
