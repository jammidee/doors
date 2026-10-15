#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""Headless QEMU test runner for DOORS milestones (used by `make test`).

Boots the test ISO (kernel cmdline "test") with the serial log in a file, a QEMU monitor socket,
and the isa-debug-exit device. While it runs, it watches the serial log and, when a line matches
a --send trigger, types the given text into the guest keyboard through the monitor.

PASS = QEMU exits with status 33 (kernel wrote QEMU_EXIT_PASS) and every --expect string is in the log.

Usage:
  qemu_test.py --iso build/doors-test.iso --log build/serial.log \
      --expect "DOORS: M2 OK" --send "M2: type doors=doors\\n" [--timeout 60]
"""
import argparse, os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "boot32"))
from qscreen import text_keys  # noqa: E402  (text -> QEMU sendkey names)

EXIT_PASS, EXIT_FAIL = 33, 35


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def monitor(sock_path, cmd):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    s.settimeout(1)
    try:
        s.recv(4096)                     # banner / prompt
    except socket.timeout:
        pass
    s.sendall(cmd.encode() + b"\n")
    time.sleep(0.05)
    s.close()


def send_keys(text):
    """Text to QEMU sendkey names; {name} is a raw key, e.g. {f1}, {up}, {backspace}, {ctrl-alt-delete}."""
    keys = []
    while text:
        i = text.find("{")
        j = text.find("}", i)
        if i < 0 or j < 0:
            keys += text_keys(text)
            break
        keys += text_keys(text[:i]) + [text[i + 1:j]]
        text = text[j + 1:]
    return keys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--expect", action="append", default=[])
    ap.add_argument("--send", action="append", default=[],
                    help="TRIGGER=TEXT: when TRIGGER appears in the log, type TEXT ('\\n' = Enter, "
                         "{name} = a raw QEMU key such as {f1} or {backspace})")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--disk", help="raw disk image attached as the ATA primary master")
    a = ap.parse_args()

    sock = f"/tmp/doors-test-{os.getpid()}.mon"
    # QEMU writes the live log in /tmp: reading a file on a WSL /mnt/* drive while it is being
    # written fails with ENODATA. It is copied to --log at the end.
    live = f"/tmp/doors-test-{os.getpid()}.log"
    sends = []
    for s in a.send:
        trig, _, text = s.partition("=")
        sends.append([trig, text.replace("\\n", "\n"), False])

    if os.path.exists(live):
        os.remove(live)
    cmd = [a.qemu, "-m", "256M", "-no-reboot", "-display", "none", "-cdrom", a.iso,
           "-serial", "file:" + live, "-monitor", f"unix:{sock},server,nowait",
           "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    if a.disk:
        cmd += ["-drive", f"file={a.disk},format=raw,if=ide,index=0", "-boot", "d"]
    q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    start = time.time()
    rc = None
    while True:
        rc = q.poll()
        if rc is not None:
            break
        if time.time() - start > a.timeout:
            q.kill()
            q.wait()
            rc = "timeout"
            break
        log = read(live)
        for s in sends:
            if not s[2] and s[0] in log and os.path.exists(sock):
                for k in send_keys(s[1]):
                    monitor(sock, "sendkey " + k)
                s[2] = True
        time.sleep(0.2)

    log = read(live)
    sys.stdout.write(log)
    with open(a.log, "w") as f:
        f.write(log)
    for p in (sock, live):
        if os.path.exists(p):
            os.remove(p)

    missing = [e for e in a.expect if e not in log]
    if rc == EXIT_PASS and not missing:
        print("TEST PASS: " + ", ".join(a.expect))
        return 0
    why = {EXIT_FAIL: "kernel reported failure (panic)", "timeout": "timeout"}.get(rc, f"qemu exit {rc}")
    if missing:
        why += "; missing: " + ", ".join(missing)
    unsent = [s[0] for s in sends if not s[2]]
    if unsent:
        why += "; never saw trigger: " + ", ".join(unsent)
    print("TEST FAIL: " + why)
    return 1


if __name__ == "__main__":
    sys.exit(main())
