#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""The kernel's debugging aids and crash reports, each triggered on purpose (`make crashtest`,
ROADMAP 2.4 and 2.7).

For every case: a fresh disk (tools/mkdisk.py); boot 1 with "crash=NAME" must panic with the
expected message and a stack trace through the expected function, and save the report in the
crash area; boot 2 (the normal ISO, same disk) must show that report. For the first case a third
boot must not show it again (shown once; the monitor's `crash` shows it any time).

Usage (in WSL, from port64/, after `make build/doors-crash-*.iso`):  python3 tools/crashtest.py
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.abspath(os.path.join(HERE, ".."))
DISK = "/tmp/doors-crash.img"
LOG = "/tmp/doors-crash.log"

# name: (text in the panic, function in the stack trace)
CASES = {
    "stack":  ("stack smashing detected in smash", "__stack_chk_fail"),
    "uaf":    ("was written after it was freed (use after free", "pmm_alloc"),
    "double": ("is not allocated (a double free?)", "pmm_free"),
    "assert": ("assertion failed: one == 2 (kernel/debug.c", "crash_test"),
    "check":  ("kernel invariant broken: pmm: the free page count", "kcheck"),
    "fault":  ("unhandled exception 14 (Page fault)", "crash_test"),
    "panic":  ("crash test: a plain panic", "crash_test"),
}


def boot(iso, until, timeout=90):
    if os.path.exists(LOG):
        os.remove(LOG)
    q = subprocess.Popen(["qemu-system-x86_64", "-m", "256M", "-display", "none", "-no-reboot",
                          "-cdrom", iso, "-drive", f"file={DISK},format=raw,if=ide,index=0",
                          "-boot", "d", "-serial", "file:" + LOG],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end, log = time.time() + timeout, ""
    while time.time() < end and q.poll() is None:
        time.sleep(0.3)
        log = open(LOG, errors="replace").read() if os.path.exists(LOG) else ""
        if until in log:
            break
    q.kill()
    q.wait()
    return open(LOG, errors="replace").read() if os.path.exists(LOG) else ""


def run(name, want, fn, third):
    subprocess.run([sys.executable, os.path.join(HERE, "mkdisk.py"), DISK,
                    "--hello", os.path.join(PORT, "build", "user", "hello.elf")],
                   check=True, stdout=subprocess.DEVNULL)
    log = boot(os.path.join(PORT, "build", f"doors-crash-{name}.iso"), "Crash report saved")
    panic = log[log.find("DOORS PANIC"):] if "DOORS PANIC" in log else ""
    if want not in panic:
        return f"boot 1: no panic with {want!r}: {panic[:200]!r}"
    trace = panic[panic.find("Stack trace:"):] if "Stack trace:" in panic else ""
    if f" {fn}+0x" not in trace:
        return f"boot 1: {fn} not in the stack trace: {trace[:300]!r}"
    if "Crash report saved on disk 12" not in log:
        return "boot 1: the report was not saved"
    log = boot(os.path.join(PORT, "build", "doors.iso"), "End of the crash report")
    shown = log[log.find("*** Crash report of an earlier boot"):]
    if "*** Crash report of an earlier boot" not in log or want not in shown or f" {fn}+0x" not in shown:
        return "boot 2: the saved report was not shown"
    if third:
        log = boot(os.path.join(PORT, "build", "doors.iso"), "Type 'help' for commands", 60)
        if "Type 'help'" not in log or "Crash report of an earlier boot" in log:
            return "boot 3: the report was shown again (or no boot)"
    lines = [l.strip() for l in trace.splitlines()[1:4]]
    print(f"PASS  {name:7} {want!r}; trace: {' <- '.join(l.split()[1] for l in lines if l)}"
          f"{'; shown once' if third else ''}", flush=True)
    return None


def main():
    fails = 0
    for i, (name, (want, fn)) in enumerate(CASES.items()):
        err = run(name, want, fn, i == 0)
        if err:
            fails += 1
            print(f"FAIL  {name:7} {err}", flush=True)
    print(f"CRASHTEST {'PASS' if not fails else f'FAIL ({fails})'}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
