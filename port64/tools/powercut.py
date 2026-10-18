#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""Power-cut test for DOORS's ext4 journal (`make powercut`, PLAN M8b).

Each run: a fresh disk (tools/mkdisk.py), DOORS boots with "powercut" and writes files in F:\\PC
(kernel/powercut.c), and the run kills QEMU (SIGKILL: the power cut) at a random point. Run 0
instead boots "powercut stop=60", which halts inside the 60th journal commit: the transaction is in
the journal but not yet in place, so a replay is needed. Then:
  A. Linux: on a copy of the ext4 partition, e2fsck -fy (replays DOORS's journal), then e2fsck -fn
     must be clean, and the files must be as the operation model says;
  B. DOORS: boot "pccheck" on the disk: the mount replays the journal, every file in F:\\PC must
     read back. Then e2fsck -fn must be clean without any repair, and the model must hold.
Even runs give F: a csum_v3 journal (so Linux checks every checksum DOORS wrote), odd runs a plain one.
FAT runs (--fat-runs) write C:\\PC instead and cut at random. FAT has no journal, so the cut may leave
damage: fsck.fat -n on a copy must find the dirty flag and exactly the problems that DOORS's mount
check then reports when it boots "pccheck vol=C"; every finished operation must be there.
"Every operation that printed `done` is there": all slots match the model after the last done op N;
the slots touched by op N+1 (in progress at the cut) may hold either state.

Usage (in WSL, from port64/, after `make build/doors-pc.iso ...`):  python3 tools/powercut.py [--runs N]
"""
import argparse, os, random, re, signal, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
from mkdisk import PARTS, SEC  # noqa: E402
from check_disk import fsck_findings, doors_findings  # noqa: E402

os.environ["PATH"] = os.environ.get("PATH", "") + ":/sbin:/usr/sbin"
DISK = "/tmp/doors-pc.img"
LOG = "/tmp/doors-pc.log"
NSLOTS = 16


# ---- the operation model (kernel/powercut.c op()) ----

def content(i):
    n = (i * 7919) % 50000 + 1
    return bytes(((k * 31 + i) & 0xFF) for k in range(n))


def model(nops):
    """slot -> op number that wrote it; also returns the slots op nops+1 touches."""
    state = {}
    for i in range(1, nops + 2):
        s, t = i % NSLOTS, (i * 3) % NSLOTS
        touched = set()
        if i % 5 == 0:
            if s in state:
                del state[s]
                touched = {s}
        elif i % 7 == 0:
            if s in state and t not in state and s != t:
                state[t] = state.pop(s)
                touched = {s, t}
        else:
            state[s] = i
            touched = {s}
        if i == nops:
            final = dict(state)
        if i == nops + 1:
            return final, touched
    return {}, set()


def name(s):
    return f"F{s:02}.BIN"


# ---- helpers ----

def boot(iso, until, timeout, pcexit=False):
    """Boot QEMU on DISK; return (log, exit status or None). Kill it when `until` appears."""
    if os.path.exists(LOG):
        os.remove(LOG)
    cmd = ["qemu-system-x86_64", "-m", "256M", "-display", "none", "-no-reboot", "-cdrom", iso,
           "-drive", f"file={DISK},format=raw,if=ide,index=0,cache=unsafe", "-boot", "d",
           "-serial", "file:" + LOG, "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + timeout
    while time.time() < end:
        if q.poll() is not None:
            return read_log(), q.returncode
        if until and until(read_log()):
            q.send_signal(signal.SIGKILL)       # the power cut
            q.wait()
            return read_log(), None
        time.sleep(0.05)
    q.kill()
    q.wait()
    raise TimeoutError("QEMU did not reach the expected point\n" + read_log()[-800:])


def read_log():
    try:
        return open(LOG, errors="replace").read()
    except OSError:
        return ""


def ext4_part(dst, part=3):
    start, n = PARTS[part][0], PARTS[part][1]
    with open(DISK, "rb") as f, open(dst, "wb") as o:
        f.seek(start * SEC)
        o.write(f.read(n * SEC))


def e2fsck(img, opts):
    r = subprocess.run(["e2fsck", opts, img], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode, r.stdout.decode(errors="replace")


def ls_pc(img):
    out = subprocess.run(["debugfs", "-R", "ls -p /PC", img], stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL).stdout.decode()
    return {f.split("/")[5] for f in out.splitlines()   # inode 0 = an unused (deleted) entry
            if f.count("/") >= 6 and f.split("/")[1] != "0" and f.split("/")[5] not in (".", "..")}


def cat(img, path):
    return subprocess.run(["debugfs", "-R", f"cat {path}", img], stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL).stdout


def check_model(img, nops, ls=ls_pc, rd=cat):
    final, touched = model(nops)
    names = ls(img)
    for s in range(NSLOTS):
        if s in touched:
            continue                    # op nops+1 was in progress: either state is fine
        want = final.get(s)
        have = name(s) in names
        if (want is not None) != have:
            return f"{name(s)}: {'missing' if want is not None else 'should not exist'} after op {nops}"
        if want is not None and rd(img, f"/PC/{name(s)}") != content(want):
            return f"{name(s)}: content is not op {want}'s"
    extra = names - {name(s) for s in range(NSLOTS)}
    return f"unexpected files {sorted(extra)}" if extra else None


# ---- FAT (C:): no journal; DOORS's mount check must report what fsck.fat finds ----

MENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")


def ls_fat(img):
    out = subprocess.run(["mdir", "-b", "-i", img, "::/PC"], stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, env=MENV).stdout.decode()
    return {l.rsplit("/", 1)[-1] for l in out.split() if l.upper().endswith(".BIN")}


def cat_fat(img, path):
    return subprocess.run(["mtype", "-i", img, "::" + path], stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, env=MENV).stdout


def run_fat(n):
    subprocess.run([sys.executable, os.path.join(HERE, "mkdisk.py"), DISK,
                    "--hello", os.path.join(PORT, "build", "user", "hello.elf"),
                    "--progs", os.path.join(PORT, "build", "user"),
                    "--sys", os.path.join(PORT, "user", "sys")], check=True, stdout=subprocess.DEVNULL)
    k = random.randint(15, 90)
    log, _ = boot(os.path.join(PORT, "build", "doors-pcfat.iso"), lambda l, k=k: f"PC: op {k} done" in l, 120)
    if "failed" in log:
        return f"the writer failed: {re.findall(r'PC: .*failed.*', log)}"
    nops = last_done(log)
    _, touched = model(nops)
    with tempfile.TemporaryDirectory() as tmp:
        a = os.path.join(tmp, "c.img")
        ext4_part(a, 0)
        r = subprocess.run(["fsck.fat", "-n", a], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        fsck, unknown = fsck_findings(r.stdout.decode(errors="replace"))
        if unknown is not None:
            return f"A: fsck.fat said something the test doesn't know: {unknown!r}"
        if ("dirty",) not in fsck:
            return "A: fsck.fat doesn't see the dirty flag"
        err = check_model(a, nops, ls_fat, cat_fat)     # every finished operation is there
        if err:
            return "A (FAT, before any repair): " + err
    log, status = boot(os.path.join(PORT, "build", "doors-pcfatcheck.iso"), None, 120)
    if status != 33 or "PC: check ok" not in log:
        return "B: DOORS check failed: " + " | ".join(l for l in log.splitlines() if "PC:" in l or "FAT on" in l)
    doors = doors_findings(log)
    if doors != fsck:
        return f"B: DOORS predicted {sorted(doors, key=str)}, fsck.fat found {sorted(fsck, key=str)}"
    bad = {m for m in re.findall(r"PC: unreadable (\S+):", log)}
    if bad - {name(s) for s in touched}:
        return f"B: DOORS can't read {sorted(bad)}, but only op {nops + 1}'s files may be damaged"
    shown = ", ".join(f[0] + (f" {f[1]}" if len(f) > 1 else "") for f in sorted(doors - {("dirty",)}, key=str))
    print(f"PASS  FAT run {n}: cut after op {k} done, {nops} ops done; fsck.fat and DOORS both found: "
          f"dirty{', ' + shown if shown else ''}", flush=True)
    return None


def last_done(log):
    d = re.findall(r"PC: op (\d+) done", log)
    return int(d[-1]) if d else 0


# ---- a run ----

def run(n, stop):
    jkind = "plain journal" if n % 2 else "csum_v3 journal"     # odd runs: no journal checksums
    subprocess.run([sys.executable, os.path.join(HERE, "mkdisk.py"), DISK,
                    "--hello", os.path.join(PORT, "build", "user", "hello.elf"),
                    "--progs", os.path.join(PORT, "build", "user"),
                    "--sys", os.path.join(PORT, "user", "sys")] + (["--plain-journal"] if n % 2 else []),
                   check=True, stdout=subprocess.DEVNULL)
    if stop:
        log, _ = boot(os.path.join(PORT, "build", "doors-pcstop.iso"),
                      lambda l: "PC: stopped inside commit" in l, 120)
        how = "stopped inside commit 60"
    else:
        k = random.randint(15, 90)
        log, _ = boot(os.path.join(PORT, "build", "doors-pc.iso"),
                      lambda l, k=k: f"PC: op {k} done" in l, 120)
        how = f"cut after op {k} done"
    if "failed" in log:
        return f"the writer failed: {re.findall(r'PC: .*failed.*', log)}"
    nops = last_done(log)
    with tempfile.TemporaryDirectory() as tmp:
        a = os.path.join(tmp, "a.img")
        ext4_part(a)
        rc, out = e2fsck(a, "-fy")              # A. Linux replays DOORS's journal
        if rc > 1:
            return f"A: e2fsck -fy exit {rc}: {out.strip().splitlines()[-3:]}"
        rc, out = e2fsck(a, "-fn")
        if rc:
            return f"A: e2fsck -fn after Linux's replay: {out.strip().splitlines()[-3:]}"
        err = check_model(a, nops)
        if err:
            return "A (Linux replay): " + err

    log, status = boot(os.path.join(PORT, "build", "doors-pccheck.iso"), None, 120)
    if status != 33 or "PC: check ok" not in log:
        return "B: DOORS check failed: " + " | ".join(l for l in log.splitlines() if "PC:" in l or "ext4" in l)
    replayed = re.search(r"journal replayed: (\d+) transactions", log)
    if stop and not replayed:
        return "B: DOORS did not replay the stopped commit"
    with tempfile.TemporaryDirectory() as tmp:
        b = os.path.join(tmp, "b.img")
        ext4_part(b)
        rc, out = e2fsck(b, "-fn")              # B. after DOORS's replay: clean, no repair at all
        if rc:
            return f"B: e2fsck -fn after DOORS's replay: {out.strip().splitlines()[-4:]}"
        err = check_model(b, nops)
        if err:
            return "B (DOORS replay): " + err
    print(f"PASS  run {n} ({jkind}): {how}, {nops} ops done, "
          f"DOORS replayed {replayed.group(1) if replayed else 0} transaction(s); Linux and DOORS clean",
          flush=True)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5, help="ext4 runs (run 0 stops inside a commit)")
    ap.add_argument("--fat-runs", type=int, default=5, help="runs on FAT (C:)")
    ap.add_argument("--seed", type=int, help="random seed for the cut points")
    a = ap.parse_args()
    seed = a.seed if a.seed is not None else int(time.time())
    random.seed(seed)
    print(f"power-cut test: {a.runs} ext4 runs, {a.fat_runs} FAT runs, seed {seed}", flush=True)
    fails = 0
    for n in range(a.runs):
        err = run(n, n == 0)
        if err:
            fails += 1
            print(f"FAIL  run {n}: {err}", flush=True)
    for n in range(a.fat_runs):
        err = run_fat(n)
        if err:
            fails += 1
            print(f"FAIL  FAT run {n}: {err}", flush=True)
    print(f"POWERCUT {'PASS' if not fails else f'FAIL ({fails})'}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
