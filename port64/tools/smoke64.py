#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""docs/SMOKE_TEST.md on the 64-bit DOORS: the same rows as tools/boot32/smoke.py (the 32-bit
MMURTL baseline), adapted where DOORS differs on purpose (system directory C:\\DOORS\\, .ELF programs,
the DOORS monitor's screens, a 4-volume disk instead of floppy + FAT16).

Boots build/doors.iso on a fresh copy of the DOORS disk (tools/mkdisk.py) in a headless QEMU with its
own monitor socket, types into the guest and polls the VGA text screen.
Rows that were XFAIL on 32-bit (known MMURTL bugs) must PASS here. P7 (debugger) is DEFERRED: the
debugger comes in M10.

Usage (in WSL, from port64/, after `make`):  python3 tools/smoke64.py [--keep] [--only P2,P4]
Exit code: 0 if no row failed.
"""
import argparse, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(PORT, "..", "tools", "boot32"))
from qscreen import mon, screen, text_keys  # noqa: E402

SOCK = "/tmp/doors-smoke64.mon"
DISK = "/tmp/doors-smoke64.img"
EXT4 = False                    # --ext4: C: is an ext4 system disk that boots by itself
results = []


def scr():
    return screen(SOCK)


def keys(*names):
    for k in names:
        mon(SOCK, "sendkey " + k)
        time.sleep(0.06)


def type_line(text, enter=True):
    keys(*text_keys(text))
    if enter:
        keys("ret")


def wait_for(needle, timeout=10.0):
    """Poll the screen until `needle` (str or tuple of alternatives) is visible. Returns the screen."""
    needles = (needle,) if isinstance(needle, str) else needle
    end = time.time() + timeout
    s = ""
    while time.time() < end:
        s = scr()
        if any(n in s for n in needles):
            return s
        time.sleep(0.3)
    raise TimeoutError(f"timed out waiting for {needles!r}\n--- screen ---\n{s}")


def wait_log(needle, timeout=10.0):
    """Wait for a line in the serial log (kernel messages, and everything programs print)."""
    end = time.time() + timeout
    while time.time() < end:
        if needle in open(LOG, errors="replace").read():
            return
        time.sleep(0.3)
    raise TimeoutError(f"timed out waiting for {needle!r} in the serial log")


def cli(cmd, expect, timeout=8.0):
    """Clear the CLI screen, run a command, wait for the expected text."""
    keys("esc")
    type_line("cls")
    time.sleep(0.4)
    type_line(cmd)
    return wait_for(expect, timeout)


def top():
    return scr().split("\n", 1)[0]


def goto(header, tries=10):
    """Cycle Ctrl-Alt-PgDn until the first line contains `header` (str or tuple), or until
    header(first line) is true when it is a function."""
    match = header if callable(header) else \
        (lambda t, hs=((header,) if isinstance(header, str) else header): any(h in t for h in hs))
    for _ in range(tries):
        if match(top()):
            return
        keys("ctrl-alt-pgdn")
        time.sleep(0.8)
    raise TimeoutError(f"could not switch to screen {header!r}")


MONITOR = "Monitor  "          # the DOORS monitor's status line: " DOORS x.y.z (x86-64)  Monitor"


def monitor_cmd(cmd, expect, timeout=5.0):
    """Run a monitor command; returns the screen from the last prompt with that command on."""
    goto(MONITOR)
    type_line(cmd)
    s = wait_for(expect, timeout)
    time.sleep(0.5)
    s = scr()
    # the command follows "> ", or starts a line when a kernel message came after the prompt
    return s[max(s.rfind("> " + cmd), s.rfind("\n" + cmd)):]


def check(row, title, fn, note=None):
    try:
        fn()
        status = "PASS"
    except Exception as e:  # noqa: BLE001 - every failure is reported the same way
        status = "FAIL"
        note = (str(e) or type(e).__name__).splitlines()[0]
        print(str(e), file=sys.stderr)
    results.append((row, title, status, note or ""))
    print(f"{status:8} {row:4} {title}" + (f"  ({note})" if note else ""), flush=True)


def defer(row, title, reason):
    results.append((row, title, "DEFERRED", reason))
    print(f"{'DEFERRED':8} {row:4} {title}  ({reason})", flush=True)


# ---------------------------------------------------------------- checks

def boot():
    s = wait_for("Type 'help' for commands", 60)
    for line in ("DOORS: M1 OK", "Interrupts: GDT+TSS", "keyboard IRQ1", "Disk 12: ATA primary master"):
        assert line in s, f"missing {line!r}"
    assert re.search(r"Tick:\d+", top()), "no tick counter on the status line"
    assert re.search(r"\d\d:\d\d:\d\d", top()), "no clock on the status line"


def memory():
    log = open(LOG).read()
    m = re.search(r"Memory:\s+(\d+) MB usable", log)
    assert m and int(m.group(1)) >= 250, "memory detection (QEMU -m 256M)"
    assert re.search(r"Memory:\s+\d+ KB free of \d+ KB", log)


def fs_init():
    s = scr()
    vols = (("Volume C: disk 12 part 1, 64 MB, ext4 DOORS-SYS", "Volume D: disk 12 part 2, 16 MB, FAT16")
            if EXT4 else
            ("Volume C: disk 12 part 1, 40 MB, FAT32", "Volume D: disk 12 part 2, 16 MB, FAT16",
             "Volume E: disk 12 part 3, 4 MB, FAT12", "Volume F: disk 12 part 4, 64 MB, ext4"))
    for v in vols:
        assert v in s, f"missing {v!r}"


def ldcli():
    keys("f1")
    wait_log("New CLI Job Number is: 4", 15)
    wait_for("Path: C:\\DOORS\\", 10)       # the CLI's status task fills it in within a second
    assert "CLI  V1.0" in top() and "Job 04" in top(), "CLI status line"


def dir_sys():
    cli("dir", "CLI.ELF")
    assert "HELP.CLI" in scr()


def dir_c():
    cli("dir C:\\", "MSAMPLES")
    cli("dir C:\\MSAMPLES", "READ.ME")


def type_file():
    s = cli("type C:\\MSAMPLES\\READ.ME", "End of file")
    assert "This directory contains" in s


def md():
    cli("md C:\\SMOKE", ">")
    cli("dir C:\\", "MSAMPLES")
    end, lines = time.time() + 5, []
    while not lines and time.time() < end:      # the dir line itself, not the typed command
        lines = [l for l in scr().splitlines() if "SMOKE" in l and "<DIR>" in l]
        time.sleep(0.3)
    assert lines, "SMOKE not listed by dir"
    line = lines[0]
    assert "<DIR>" in line and re.search(r"\d\d-\d\d-2[4-9] \d\d:\d\d:\d\d", line), \
        f"md: no real timestamp: {line!r}"


def copy_ok():
    cli("copy C:\\MSAMPLES\\READ.ME C:\\SMOKE\\READ.ME", ">", 10)
    s = cli("dir C:\\SMOKE", "READ.ME")
    assert any(l.split()[:1] == ["627"] for l in s.splitlines()), "copy size is not 627"
    s = cli("type C:\\SMOKE\\READ.ME", "End of file")
    assert "themselves" in s, "copied file is truncated"     # last line of the 627-byte READ.ME


def ren():
    cli("ren C:\\SMOKE\\READ.ME C:\\SMOKE\\NOTE.TXT", ">")
    cli("dir C:\\SMOKE", "NOTE.TXT")


def delete():
    cli("del C:\\SMOKE\\NOTE.TXT", "Done.")
    s = cli("dir C:\\SMOKE", ">")
    time.sleep(1)
    assert "NOTE" not in scr()


def rd():
    cli("rd C:\\SMOKE", ">")
    cli("dir C:\\SMOKE", "Directory doesn't exist")
    cli("dir", "CLI.ELF")                                    # and the system is still alive


def path():
    cli("path C:\\MSAMPLES\\", ">")
    s = cli("dir", "READ.ME")
    time.sleep(1.2)                                          # the status task updates each second
    assert "Path: C:\\MSAMPLES\\" in top(), "status line path"
    cli("path C:\\DOORS\\", ">")


def lowercase():
    if EXT4:            # ext4 names are case-sensitive (devman 6); the drive letter is not
        cli("dir c:\\msamples", ("READ.ME", "doesn't exist"))
        assert "doesn't exist" in scr(), "ext4 found a name in the wrong case"
        cli("dir c:\\MSAMPLES", ("READ.ME", "doesn't exist"))
        assert "READ.ME" in scr(), "lower-case drive letter not accepted"
        return
    cli("dir c:\\msamples", ("READ.ME", "doesn't exist"))
    assert "READ.ME" in scr(), "lower-case path not found"


def dump():
    s = cli("dump C:\\MSAMPLES\\READ.ME", "00000000 54 68 69 73")   # "This"
    assert "00000000 54 68 69 73 20 64 69 72 65 63 74 6f 72 79 20 63 This directory c" in s, \
        "dump ASCII column"


def undel():
    s = cli("help", "Internal CLI Commands")
    keys("esc")
    assert "undel" not in s.lower(), "help still advertises undel"


def help_():
    cli("help", "Internal CLI Commands")
    keys("esc")


def _edit(name):
    cli("edit " + name, "Create??", 10)
    keys("y")
    wait_for("nChars:", 5)
    type_line("Hello from DOORS")
    wait_for("nChars: 00017", 5)          # the editor has consumed all the keystrokes
    keys("alt-s")
    time.sleep(1.5)
    keys("alt-q")
    time.sleep(1)
    if "SAVE IT?" in scr():
        keys("y")
    wait_for("CLI  V1.0", 8)


def editor():
    _edit("C:\\EDITED.TXT")
    cli("type C:\\EDITED.TXT", "Hello from DOORS")


def editor_long_path():
    cli("md C:\\SMOKE", ">")
    _edit("C:\\SMOKE\\HELLO.TXT")
    s = cli("dir C:\\SMOKE", "HELLO")
    assert "HELLO.TXT" in s, "file saved without its extension"


def service_ipc():
    # CLI job 4 runs the service; a second CLI (job 5) is the client.
    cli("Service", "NUMBERS Service Installed.", 10)
    goto(MONITOR)
    keys("f1")
    wait_log("New CLI Job Number is: 5", 15)
    goto("Job 05")
    cli("TestSvc", "gave out number: 0", 10)
    cli("TestSvc", "gave out number: 1", 10)
    goto(("Job 04", "NUMBERS Service Installed."))
    keys("x")                             # any key stops the service
    wait_for("CLI  V1.0", 8)              # the CLI comes back (exit run file)


def jobs_stats():
    goto(MONITOR)
    keys("f2")
    s = wait_for("Job  5", 5)
    assert "Job  1  DOORS OS" in s and "CLI V1.0" in s
    keys("f3")
    s = wait_for("Switches", 5)
    assert "preemptive" in s and "Free: exchanges" in s


def screens():
    goto(MONITOR)
    seen = []
    for _ in range(4):
        keys("ctrl-alt-pgdn")
        time.sleep(0.8)
        seen.append(top())
    assert any("Job 04" in t for t in seen) and any("Job 05" in t for t in seen) and \
        any(MONITOR in t for t in seen), f"Ctrl-Alt-PgDn cycle: {seen}"


def busyloop_kill():
    goto("Job 05")
    cli("busyloop", "\n", 3)
    time.sleep(2.5)
    a = [l for l in scr().splitlines() if l.strip().isdigit()]
    time.sleep(1.5)
    b = [l for l in scr().splitlines() if l.strip().isdigit()]
    assert a and b and int(b[-1]) > int(a[-1]), "timer tick not advancing under BUSYLOOP"
    s = monitor_cmd("jobs", "Modules:")                    # the monitor still answers
    assert re.search(r"Job  5  BusyLoop +running", s), "BusyLoop not in the job list"
    goto(lambda t: t.strip().isdigit())                    # its ticks scrolled the status line away
    keys("ctrl-alt-delete")
    s = wait_for("Job number 5 terminated. Error: 4", 8)
    s = monitor_cmd("jobs", "Modules:")
    assert not re.search(r"Job  5 .*running", s), "BUSYLOOP job 5 still running after Ctrl-Alt-Del: " + repr(s)


def monitor_and_exit():
    goto("Job 04")
    cli("monitor", MONITOR, 5)             # the CLI's Monitor command shows the monitor
    assert MONITOR in top()
    goto("Job 04")
    cli("exit", MONITOR, 8)
    s = monitor_cmd("jobs", "Modules:")
    assert not re.search(r"Job  4 .*running", s), "CLI job 4 still running after exit"


def ext4_after():
    """After the session (QEMU quit, no sync): C: must be clean for Linux, with the files the rows
    wrote, and GRUB must still boot DOORS from it."""
    sys.path.insert(0, HERE)
    from mkdisk import SYS_PARTS, SEC
    c = "/tmp/doors-smoke64-c.img"
    with open(DISK, "rb") as f, open(c, "wb") as o:
        f.seek(SYS_PARTS[0][0] * SEC)
        o.write(f.read(SYS_PARTS[0][1] * SEC))
    r = subprocess.run(["e2fsck", "-fn", c], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert r.returncode == 0, "e2fsck: " + " | ".join(r.stdout.decode(errors="replace").splitlines()[-3:])
    out = subprocess.run(["debugfs", "-R", "ls /", c], stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL).stdout.decode()
    assert "EDITED.TXT" in out and "SMOKE" in out, "the files the smoke rows wrote are missing"
    os.remove(c)
    log2 = LOG + ".2"
    if os.path.exists(log2):
        os.remove(log2)
    q = subprocess.Popen(["qemu-system-x86_64", "-m", "256M", "-display", "none", "-no-reboot",
                          "-drive", f"file={DISK},format=raw,if=ide,index=0", "-boot", "c",
                          "-serial", "file:" + log2], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end, ok = time.time() + 60, False
    while time.time() < end and q.poll() is None and not ok:
        time.sleep(0.5)
        ok = os.path.exists(log2) and "Type 'help' for commands" in open(log2, errors="replace").read()
    q.kill()
    q.wait()
    assert ok, "the second boot failed"


ROWS = [
    ("B1", "boot to the monitor, clock and ticks, drivers", boot),
    ("B2", "memory detection", memory),
    ("B4", "file system init (the system disk's volumes)", fs_init),
    ("B5", "F1 LdCLI", ldcli),
    ("F1", "dir (C:\\DOORS)", dir_sys),
    ("F2", "dir C:\\ and C:\\MSAMPLES", dir_c),
    ("F3", "type", type_file),
    ("F4", "md (with a real timestamp)", md),
    ("F5", "copy is byte-accurate", copy_ok),
    ("F6", "ren", ren),
    ("F6b", "del", delete),
    ("F7", "rd (hung MMURTL)", rd),
    ("F8", "path", path),
    ("F9", "lower-case paths (ext4: case-sensitive)", lowercase),
    ("F10", "dump (ASCII column)", dump),
    ("F10b", "undel no longer advertised", undel),
    ("P1", "help", help_),
    ("P2", "editor create/save/quit", editor),
    ("P2b", "editor with a path > 13 chars", editor_long_path),
    ("P4", "NUMBERS service IPC across two CLIs (P5)", service_ipc),
    ("B6", "monitor jobs (F2) / stats (F3)", jobs_stats),
    ("B7", "Ctrl-Alt-PgDn cycles the screens", screens),
    ("P3", "BUSYLOOP preemption + Ctrl-Alt-Del (P6)", busyloop_kill),
    ("P8", "CLI monitor command, exit", monitor_and_exit),
]
LOG = "/tmp/doors-smoke64.log"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="leave the VM running afterwards")
    ap.add_argument("--only", help="comma-separated rows (B1 and B5 always run), e.g. P2,P4")
    ap.add_argument("--ext4", action="store_true",
                    help="boot from an ext4 system disk (mkdisk --system ext4: GRUB on the disk, no ISO)")
    a = ap.parse_args()
    only = set(a.only.split(",")) | {"B1", "B5"} if a.only else None
    global EXT4
    EXT4 = a.ext4
    print("smoke test on the " + ("ext4 system disk (booted from the disk)" if EXT4 else
                                  "FAT32 system disk (booted from the ISO)"), flush=True)

    args = ["--hello", os.path.join(PORT, "build", "user", "hello.elf"),
            "--progs", os.path.join(PORT, "build", "user"), "--sys", os.path.join(PORT, "user", "sys")]
    if EXT4:
        args += ["--system", "ext4", "--boot", os.path.join(PORT, "build", "doors.iso.d", "boot")]
        boot = ["-boot", "c"]
    else:
        iso = "/tmp/doors-smoke64.iso"      # a local copy: reads from /mnt/* can be slow
        shutil.copyfile(os.path.join(PORT, "build", "doors.iso"), iso)
        boot = ["-cdrom", iso, "-boot", "d"]
    subprocess.run([sys.executable, os.path.join(HERE, "mkdisk.py"), DISK] + args, check=True,
                   stdout=subprocess.DEVNULL)
    for p in (SOCK, LOG):
        if os.path.exists(p):
            os.remove(p)
    qemu = subprocess.Popen(["qemu-system-x86_64", "-m", "256M", "-display", "none",
                             "-drive", f"file={DISK},format=raw,if=ide,index=0"] + boot +
                            ["-serial", "file:" + LOG, "-monitor", f"unix:{SOCK},server,nowait"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        end = time.time() + 30
        while not os.path.exists(SOCK):
            if qemu.poll() is not None or time.time() > end:
                sys.exit("QEMU did not start")
            time.sleep(0.3)
        for row, title, fn in ROWS:
            if only and row not in only:
                continue
            check(row, title, fn)
            if row == "B1" and results[-1][2] == "FAIL":
                break
            if row == "P2b":
                defer("P7", "debugger + F6 tasks + ESC", "the debugger comes in M10; CLI debug says so")
    finally:
        if not a.keep:
            try:
                mon(SOCK, "quit")
            except OSError:
                pass
            try:
                qemu.wait(timeout=10)
            except subprocess.TimeoutExpired:
                qemu.kill()

    if EXT4:
        check("B8", "the system disk DOORS wrote: e2fsck clean, and it boots again", ext4_after)

    counts = {k: sum(1 for r in results if r[2] == k) for k in ("PASS", "FAIL", "DEFERRED")}
    print("\n" + "  ".join(f"{k} {v}" for k, v in counts.items()))
    sys.exit(1 if counts["FAIL"] else 0)


if __name__ == "__main__":
    main()
