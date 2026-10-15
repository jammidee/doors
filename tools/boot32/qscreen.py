#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""Read the VGA text screen of a running QEMU (or send keys) via its monitor socket.

Usage:
  qscreen.py [--sock /tmp/mmurtl.mon]                 print the 80x25 text screen
  qscreen.py --keys "d i r ret"                       send QEMU sendkey names, then print
  qscreen.py --type 'dir c:\\msamples\\n' --wait 1   type text ('\\n' = Enter), wait, then print
  qscreen.py --cmd quit                               run a raw monitor command
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

_PLAIN = {" ": "spc", "\n": "ret", "\\": "backslash", ".": "dot", ",": "comma", "-": "minus",
          "=": "equal", "/": "slash", ";": "semicolon", "'": "apostrophe", "[": "bracket_left",
          "]": "bracket_right", "`": "grave_accent"}
_SHIFT = {":": "semicolon", "_": "minus", "+": "equal", "?": "slash", "*": "8", "!": "1",
          "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "(": "9", ")": "0",
          '"': "apostrophe", "<": "comma", ">": "dot", "|": "backslash", "{": "bracket_left",
          "}": "bracket_right", "~": "grave_accent"}

def text_keys(text):
    """Translate text into QEMU sendkey names ('\\n' = Enter)."""
    keys = []
    for ch in text:
        if ch.isalpha():
            keys.append(("shift-" if ch.isupper() else "") + ch.lower())
        elif ch.isdigit():
            keys.append(ch)
        elif ch in _PLAIN:
            keys.append(_PLAIN[ch])
        elif ch in _SHIFT:
            keys.append("shift-" + _SHIFT[ch])
        else:
            raise SystemExit(f"no key mapping for {ch!r}")
    return keys

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
    ap.add_argument("--type", dest="text", help="text to type; '\\n' (backslash-n) presses Enter")
    ap.add_argument("--enter", action="store_true", help="press Enter after --type text")
    ap.add_argument("--wait", type=float, default=0, help="seconds to wait before reading")
    a = ap.parse_args()
    if a.cmd:
        print(mon(a.sock, a.cmd).replace("(qemu) ", "").strip())
    keys = a.keys.split() if a.keys else []
    if a.text:
        keys += text_keys(a.text.replace("\\n", "\n"))
    if a.enter:
        keys.append("ret")
    for k in keys:
        mon(a.sock, "sendkey " + k)
        time.sleep(0.08)
    if keys:
        time.sleep(0.5)
    time.sleep(a.wait)
    print(screen(a.sock))
