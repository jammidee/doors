#!/usr/bin/env python3
# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
"""Kernel line coverage of a test run (ROADMAP 2.6, `make coverage`).

  python3 tools/coverage.py build/cov/kernel.elf build/cov/serial.log [report.txt]

The coverage kernel (COV=1) calls __sanitizer_cov_trace_pc at the start of every basic block, and
kernel/cov.c prints the return addresses it saw as a bitmap ("COV ..." lines in the serial log).
Every instrumented block is a `call __sanitizer_cov_trace_pc` in the kernel's code (objdump); its
return address is the block's id. addr2line maps blocks to source lines. A line is covered when
any of its blocks ran. Only the kernel's own C files count (kernel/, drivers/, arch/).
"""
import collections, os, re, subprocess, sys

PORT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
TEST_CODE = ("kernel/selftest.c",)      # listed, but not counted in the total


def blocks(elf):
    """Return addresses of every `call __sanitizer_cov_trace_pc` (the instrumented blocks)."""
    out = subprocess.run(["objdump", "-d", "--no-show-raw-insn", elf], capture_output=True,
                         text=True, check=True).stdout
    sites, prev_call = [], False
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(\S.*)$", line)
        if not m:
            continue
        if prev_call:
            sites.append(int(m.group(1), 16))      # the instruction after the call
        prev_call = "<__sanitizer_cov_trace_pc>" in m.group(2) and m.group(2).startswith("call")
    return sites


def covered(log):
    text, hit = None, set()
    for line in open(log, errors="replace"):
        if line.startswith("COV text "):
            text = int(line.split()[2], 16)
        elif line.startswith("COV ") and text is not None and not line.startswith("COV end"):
            _, off, bits = line.split()
            base = int(off, 16) * 8
            for i in range(32):
                b = int(bits[2 * i:2 * i + 2], 16)
                for k in range(8):
                    if b & (1 << k):
                        hit.add(text + base + i * 8 + k)
    if text is None:
        sys.exit("coverage: no COV lines in the log (not a COV=1 kernel?)")
    return hit


def lines_of(elf, addrs):
    """address -> (file relative to port64, line)"""
    p = subprocess.run(["addr2line", "-e", elf], input="\n".join(f"{a:x}" for a in addrs),
                       capture_output=True, text=True, check=True)
    res = {}
    for a, l in zip(addrs, p.stdout.splitlines()):
        f, _, n = l.rpartition(":")
        n = n.split()[0]
        f = os.path.relpath(f, PORT) if f.startswith("/") else f
        if n.isdigit() and re.match(r"(kernel|drivers|arch)/.*\.c$", f):
            res[a] = (f, int(n))
    return res


def main():
    elf, log = sys.argv[1], sys.argv[2]
    report = sys.argv[3] if len(sys.argv) > 3 else None
    sites = blocks(elf)
    hit = covered(log)
    where = lines_of(elf, sites)
    all_lines, cov_lines = collections.defaultdict(set), collections.defaultdict(set)
    nblk = nhit = 0
    for a, (f, n) in where.items():
        nblk += 1
        all_lines[f].add(n)
        if a in hit:
            nhit += 1
            cov_lines[f].add(n)
    rows = []
    for f in sorted(all_lines):
        t, c = len(all_lines[f]), len(cov_lines[f])
        rows.append((t - c, f, c, t))
    tot = sum(len(v) for f, v in all_lines.items() if f not in TEST_CODE)
    cov = sum(len(v) for f, v in cov_lines.items() if f not in TEST_CODE)
    out = [f"Kernel line coverage of this run, without the test code ({', '.join(TEST_CODE)}): "
           f"{cov} of {tot} lines ({100 * cov / tot:.1f} %); {nhit} of {nblk} blocks run in all", "",
           f"{'file':32} {'lines':>7} {'covered':>8} {'%':>6} {'not run':>8}"]
    for miss, f, c, t in sorted(rows, key=lambda r: r[1]):
        out.append(f"{f:32} {t:7} {c:8} {100 * c / t:6.1f} {miss:8}")
    out.append("")
    out.append("Most lines not run: " + ", ".join(f"{f} ({m})" for m, f, c, t in sorted(rows, reverse=True)[:8]))
    if report:
        with open(report, "w") as o:
            o.write("\n".join(out) + "\n")
            o.write("\nLines not run, per file:\n")
            for f in sorted(all_lines):
                miss = sorted(all_lines[f] - cov_lines[f])
                if miss:
                    o.write(f"{f}: {' '.join(map(str, miss))}\n")
    print("\n".join(out))
    print(f"COVERAGE {100 * cov / tot:.1f} %" + (f" (report: {report})" if report else ""))


if __name__ == "__main__":
    main()
