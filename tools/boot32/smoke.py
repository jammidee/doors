#!/usr/bin/env python3
"""Scripted version of docs/SMOKE_TEST.md for the 32-bit baseline (original MMURTL V1.0 binaries).

Boots a fresh, headless VM with its own images and monitor socket, so it can run alongside an
interactive `run.sh`. Each check types into the guest and polls the VGA text screen until an expected
string appears (or a timeout). Rows hitting known baseline bugs are XFAIL: they are expected to fail on
32-bit and must PASS on the 64-bit DOORS. An XFAIL row that passes is reported as XPASS.

Not run: F7 `rd` (hangs the whole system).

Usage (in WSL):  python3 tools/boot32/smoke.py [--keep] [--verbose]
Exit code: 0 if no unexpected failures.
"""
import argparse, os, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qscreen import mon, screen, text_keys  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(ROOT, "build", "boot32", "smoke")
SOCK = "/tmp/doors-smoke.mon"

results = []
verbose = False


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


def cli(cmd, expect, timeout=8.0):
    """Clear the CLI screen, run a command, wait for the expected text."""
    keys("esc")
    type_line("cls")
    time.sleep(0.4)
    type_line(cmd)
    return wait_for(expect, timeout)


def monitor_jobs():
    """On the Monitor: leave the Stats view if it is up (it eats the next key), then show Jobs."""
    if "Any key to dismiss" in scr():
        keys("spc")
        time.sleep(1)
    keys("f2")
    return wait_for("Job: 2", 5)


def goto(header, tries=8):
    """Cycle Ctrl-Alt-PgDn until the screen's first line contains `header` (str or tuple of
    alternatives; a running program can overwrite the CLI's status line)."""
    headers = (header,) if isinstance(header, str) else header
    for _ in range(tries):
        top = scr().split("\n", 1)[0]
        if any(h in top for h in headers):
            return
        keys("ctrl-alt-pgdn")
        time.sleep(1.0)
    raise TimeoutError(f"could not switch to screen {header!r}")


def check(row, title, fn, xfail=None):
    """Run one row. xfail = reason string if the row is a known baseline bug."""
    try:
        fn()
        status = "XPASS" if xfail else "PASS"
        detail = xfail or ""
    except Exception as e:  # noqa: BLE001 - report every failure the same way
        status = "XFAIL" if xfail else "FAIL"
        detail = xfail or str(e).splitlines()[0]
        if verbose or not xfail:
            print(str(e), file=sys.stderr)
    results.append((row, title, status, detail))
    print(f"{status:5}  {row:4} {title}" + (f"  ({detail})" if detail else ""), flush=True)


# ---------------------------------------------------------------- checks

def boot():
    s = wait_for("File System... Error:", 40)
    for line in ("Init KBD Service Error: 0", "Init Serial Comms Device Driver Error: 0",
                 "Init Parallel LPT Device Driver Error: 0", "Init floppy device driver... Error: 0",
                 "Init hard disk device driver... Error: 0", "File System... Error: 0"):
        assert line in s, f"missing {line!r}"

def fs_init():
    s = scr()
    assert "FAT12" in s and "C: Heads 16, Sec/Trk 17" in s and "FAT16" in s, "drive geometry lines"

def memory():
    s = scr()
    assert "Total memory (Kb): 16384" in s and "Free memory  (Kb):" in s

def ldcli():
    keys("f1")
    wait_for("CLI  V1.0", 15)

def dir_a():
    cli("dir", "CLI      RUN")

def dir_c():
    cli("dir C:\\", "MSAMPLES")

def dir_samples():
    cli("dir C:\\MSAMPLES", "BUSYLOOP")
    keys("esc")

def type_file():
    cli("type C:\\MSAMPLES\\READ.ME", "End of file")
    assert "This directory contains" in scr()

def md():
    cli("md C:\\SMOKE", ">")
    cli("dir C:\\", "SMOKE")

def copy_ok():
    cli("copy C:\\MSAMPLES\\READ.ME C:\\SMOKE\\READ.ME", ">", 10)
    s = cli("type C:\\SMOKE\\READ.ME", "End of file")
    assert "themselves" in s, "copied file is truncated"   # last line of the 627-byte READ.ME

def ren():
    cli("ren C:\\SMOKE\\READ.ME C:\\SMOKE\\NOTE.TXT", ">")
    cli("dir C:\\SMOKE", "NOTE     TXT")

def delete():
    cli("del C:\\SMOKE\\NOTE.TXT", "Done.")
    s = cli("dir C:\\SMOKE", "End of file")
    assert "NOTE" not in s

def path():
    cli("path C:\\MSAMPLES\\CLI\\", ">")
    s = cli("dir", "CLI      C")
    assert "Path: C:\\MSAMPLES\\CLI\\" in s
    cli("path A:\\MMSYS\\", ">")

def lowercase():
    cli("dir c:\\msamples", ("BUSYLOOP", "Error", "doesn't exist"))
    assert "BUSYLOOP" in scr(), "lower-case path not found"
    keys("esc")

def dump():
    cli("dump C:\\MSAMPLES\\READ.ME", "00000000 54 68 69 73")   # "This"
    keys("esc")

def undel():
    s = cli("undel C:\\SMOKE\\NOTE.TXT", (">",))
    time.sleep(1)
    assert "Command not found" not in scr(), "undel not implemented"

def help_():
    cli("help", "Internal CLI Commands")
    keys("esc")

def editor():
    _edit("C:\\HELLO.TXT")       # <= 13 chars: see editor_long_path
    cli("type C:\\HELLO.TXT", "Hello from DOORS")

def editor_long_path():
    _edit("C:\\SMOKE\\HELLO.TXT")
    cli("dir C:\\SMOKE", "End of file")
    assert "HELLO    TXT" in scr(), "file saved without its extension"

def _edit(name):
    cli("edit " + name, "Create??", 10)
    keys("y")
    wait_for("nChars:", 5)      # status line (it truncates the file name to 13 chars)
    type_line("Hello from DOORS")
    wait_for("nChars: 00017", 5)          # editor has consumed all the keystrokes
    keys("alt-s")
    time.sleep(2)                         # the save goes through the file system service
    keys("alt-q")
    time.sleep(1)
    if "SAVE IT?" in scr():               # only asked if the Alt-S save didn't take
        keys("y")
    wait_for("CLI  V1.0", 8)

def debugger():
    cli("debug", "SStep", 5)
    keys("f6")
    wait_for("Task#", 5)
    keys("esc")          # leave task list
    time.sleep(0.5)
    keys("esc")          # leave debugger
    wait_for("CLI  V1.0", 5)
    cli("dir", "End of file")

def service_ipc():
    # CLI job 3 runs the service; a second CLI (job 4) is the client.
    cli("Service", "NUMBERS Service Installed.", 10)
    goto("MMURTL Monitor")
    keys("f1")
    wait_for("New CLI Job Number is: 4", 15)
    goto("Job 04")
    cli("TestSvc", "gave out number: 0", 10)
    cli("TestSvc", "gave out number: 1", 10)
    goto(("Job 03", "NUMBERS Service Installed."))
    keys("x")            # any key stops the service
    wait_for(">", 5)

def jobs_stats():
    goto("MMURTL Monitor")
    s = monitor_jobs()
    assert "Job: 4" in s
    assert "MMOS Monitor" in s and "Debugger" in s and "CLI V1.0" in s
    keys("f3")
    s = wait_for("Task switches total", 5)
    assert "Preemptive task switches" in s and "Free Exchanges" in s
    # Stats view stays up until a key; monitor_jobs() dismisses it when needed.

def busyloop_kill():
    goto("Job 04")
    cli("run C:\\MSAMPLES\\BUSYLOOP\\BUSYLOOP.RUN", "\n", 3)
    time.sleep(2)
    a = [l for l in scr().splitlines() if l.strip().isdigit()]
    time.sleep(1.5)
    b = [l for l in scr().splitlines() if l.strip().isdigit()]
    assert a and b and int(b[-1]) > int(a[-1]), "timer tick not advancing under BUSYLOOP"
    keys("ctrl-alt-delete")
    # The Monitor logs "Job number 4 terminated. Error: 4", but not visibly if it is showing the
    # Jobs/Stats view, so check the job list instead.
    wait_for("MMURTL Monitor", 8)
    s = monitor_jobs()
    assert "Job: 4" not in s, "BUSYLOOP job 4 still listed after Ctrl-Alt-Del"

def exit_cli():
    goto("Job 03")
    cli("exit", "MMURTL Monitor", 8)
    s = monitor_jobs()
    assert "Job: 3" not in s, "CLI job 3 still listed after exit"


ROWS = [
    ("B1", "boot to Monitor, drivers init", boot, None),
    ("B2", "memory detection", memory, None),
    ("B4", "file system init (A: FAT12, C: FAT16 16x17)", fs_init, None),
    ("B5", "F1 LdCLI", ldcli, None),
    ("F1", "dir (A:\\MMSYS)", dir_a, None),
    ("F2", "dir C:\\ and C:\\MSAMPLES", lambda: (dir_c(), dir_samples()), None),
    ("F3", "type", type_file, None),
    ("F4", "md", md, None),
    ("F5", "copy is byte-accurate", copy_ok, "copy truncates last partial block (Erc 217)"),
    ("F6", "ren", ren, None),
    ("F6b", "del", delete, None),
    ("F8", "path", path, None),
    ("F9", "lower-case paths", lowercase, None),
    ("F10", "dump", dump, None),
    ("F10b", "undel", undel, "undel listed in HELP.CLI but not implemented"),
    ("P1", "help", help_, None),
    ("P2", "editor create/save/quit", editor, None),
    ("P2b", "editor with a path > 13 chars", editor_long_path,
     "editor keeps only 13 chars of the file name (C:\\SMOKE\\HELLO.TXT -> HELLO, no extension)"),
    ("P7", "debugger + F6 tasks + ESC", debugger, None),
    ("P4", "NUMBERS service IPC across two CLIs (P5)", service_ipc, None),
    ("B6", "Monitor Jobs/Stats", jobs_stats, None),
    ("P3", "BUSYLOOP preemption + Ctrl-Alt-Del (P6)", busyloop_kill, None),
    ("P8", "exit CLI", exit_cli, None),
]


def main():
    global verbose
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="leave the VM running afterwards")
    ap.add_argument("--verbose", action="store_true", help="print screens for XFAIL rows too")
    ap.add_argument("--only", help="comma-separated rows to run (B1 and B5 always run), e.g. P2,P4")
    a = ap.parse_args()
    verbose = a.verbose
    only = set(a.only.split(",")) | {"B1", "B5"} if a.only else None

    shutil.rmtree(BUILD, ignore_errors=True)
    os.makedirs(BUILD)
    if os.path.exists(SOCK):
        os.remove(SOCK)
    env = dict(os.environ, BOOT32_BUILD=BUILD, BOOT32_MON=SOCK)
    log = open(os.path.join(BUILD, "qemu.log"), "w")
    qemu = subprocess.Popen(["bash", os.path.join(HERE, "run.sh"), "-display", "none"],
                            env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        end = time.time() + 120          # image build on /mnt/* is slow
        while not os.path.exists(SOCK):
            if qemu.poll() is not None:
                sys.exit("QEMU exited, see " + log.name)
            if time.time() > end:
                sys.exit("QEMU monitor socket did not appear")
            time.sleep(0.5)
        for row, title, fn, xfail in ROWS:
            if only and row not in only:
                continue
            check(row, title, fn, xfail)
            if row == "B1" and results[-1][2] == "FAIL":
                break
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
                subprocess.run(["pkill", "-f", "unix:" + SOCK])

    counts = {k: sum(1 for r in results if r[2] == k) for k in ("PASS", "FAIL", "XFAIL", "XPASS")}
    print("\n" + "  ".join(f"{k} {v}" for k, v in counts.items()))
    sys.exit(1 if counts["FAIL"] else 0)


if __name__ == "__main__":
    main()
