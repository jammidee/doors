#!/usr/bin/env python3
"""Read the VGA text screen of a running QEMU (or send keys) via its monitor socket.

Usage:
  qscreen.py [--sock /tmp/mmurtl.mon]                 print the 80x25 text screen
  qscreen.py --keys "dir ret"                         send QEMU sendkey names, then print
"""
import argparse, re, socket, time

def mon(sock_path, cmd):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    s.settimeout(2)
    buf = b""
    def drain():
        nonlocal buf
        try:
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
                if buf.endswith(b"(qemu) "):
                    break
        except socket.timeout:
            pass
    drain(); buf = b""
    s.sendall(cmd.encode() + b"\n")
    drain()
    s.close()
    return buf.decode("latin-1")

def screen(sock_path):
    out = mon(sock_path, "xp /4000bx 0xb8000")
    data = [int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})\b", out.split("\n", 1)[1])]
    chars = bytes(data[0::2])[:2000]
    cp = chars.decode("cp437", "replace")
    return "\n".join(cp[i:i + 80].rstrip() for i in range(0, 2000, 80))

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default="/tmp/mmurtl.mon")
    ap.add_argument("--keys", help="space-separated QEMU key names, e.g. 'h e l p ret'")
    ap.add_argument("--cmd", help="raw monitor command to run first, e.g. system_reset")
    ap.add_argument("--wait", type=float, default=0, help="seconds to wait before reading")
    a = ap.parse_args()
    if a.cmd:
        print(mon(a.sock, a.cmd).replace("(qemu) ", "").strip())
    time.sleep(a.wait)
    if a.keys:
        for k in a.keys.split():
            mon(a.sock, "sendkey " + k)
            time.sleep(0.08)
        time.sleep(0.5)
    print(screen(a.sock))
