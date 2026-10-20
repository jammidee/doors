<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# DOORS Roadmap: from 1.x to a stable operating system

`PLAN.md` took DOORS from MMURTL V1.0 to a working 64-bit system (1.0.0, then the ext4 journal in
1.1.0). This roadmap covers the next stretch: making DOORS **stable** in the sense people expect
from Linux, the BSDs or Windows. It doesn't crash, doesn't lose data, runs on real computers, and
behaves the same from one release to the next.

## What "stable" means here

DOORS is called stable when all of these hold, and stay true release after release:

| Area | Goal |
|---|---|
| **No crashes** | 72 hours of random mixed work (files, jobs, messaging, screens) without a kernel panic or hang. Fuzzing the system calls and the file system for 24 hours finds nothing. |
| **No data loss** | Power cuts at random points never lose a finished file operation and never leave a disk that needs repair. This holds on real disks, not only in QEMU. A damaged disk gives an error, never a crash. |
| **Bad programs can't hurt the system** | Any program, however buggy or hostile, can only end itself. It can't crash the kernel, read other programs' memory, or use up a resource so others stop working. |
| **The network can't hurt it** | Once DOORS has a network (Stage 8): no packet, however damaged or hostile, crashes or hangs it, and a flood slows it down without stopping it. |
| **Runs on real hardware** | Boots and works on at least 5 different real PCs (BIOS and UEFI, SATA and NVMe, USB keyboard) and in VirtualBox, VMware and Hyper-V, not only QEMU. |
| **Predictable releases** | Every change runs the full test set automatically. Released versions have no known data-loss or crash bugs. The MMURTL API stays compatible. |

## Where DOORS stands (1.1.0)

**Already solid:**
- Every user pointer is checked, and a faulting program is ended without harming the system.
- Jobs are torn down without leaks.
- ext4 has a journal that survives power cuts (`make powercut`).
- Linux's own tools check every disk the tests write.
- Self-tests cover M1–M9, plus the 64-bit smoke test (`make smoke`).

**Not yet:**
- **Hardware:** only QEMU has been tested. Disks use ATA PIO only, with no AHCI/NVMe/USB. Boot is BIOS + GRUB only (no UEFI). Timers and interrupts use the legacy PIC/PIT, and only one CPU is used.
- **Kernel internals:** tables have fixed sizes (64 jobs, 256 tasks, 256 request blocks), there is no kernel heap, and resource exhaustion paths are tested only partly.
- **Test coverage:** no continuous integration, no fuzzing, no long soak tests.
- **Security:** no users or file permissions are enforced, and the kernel doesn't use SMEP/SMAP or stack protection.
- **Networking:** none yet (MMURTL had none either). See Stage 8.

The order below puts stability before features. Each step is a release with a test that proves it.

---

## Stage 1: finish the storage work ✅ (1.2.0 – 1.8.0)

Data loss is the worst failure an OS can have, so storage comes first.

| Step | What | Done when |
|---|---|---|
| 1.1 | Finish htree directory writes ✅ (1.2.0) | `make test` passes, e2fsck clean on G: ✅ |
| 1.2 | Extent trees deeper than one level on write ✅ (1.3.0) | A heavily fragmented file can be written, cut and grown; e2fsck clean ✅ (480 one-block extents, depth 2, in `make test`; 1,200 passed once but is too slow for every run. 10,000 waits for faster disk I/O) |
| 1.3 | Write journals with checksums (JBD2 `csum_v3`, the default of newer e2fsprogs) ✅ (1.4.0) | Volumes made by e2fsprogs 1.47 mount read/write; `make powercut` passes on them ✅ (F: has a csum_v3 journal made by `debugfs jo -c`, the same format 1.47 writes; powercut alternates csum_v3 and plain journals) |
| 1.4 | Damaged disks never crash DOORS: every on-disk value is range-checked ✅ (1.5.0) | A fuzzer that corrupts ext4 and FAT images at random (thousands of images) finds no panic; DOORS reports `ErcBrokenFile` ✅ (`make fuzz`: 9,000 images a run; 30,000 clean after the fixes. It runs the kernel's file system code on Linux with sanitizers, plus a 12 KB guarded stack) |
| 1.5 | FAT safety: the "dirty" flag set while mounted, and `chkdsk`-style warnings at mount ✅ (1.6.0) | A power cut on FAT never makes DOORS crash; Linux `fsck.fat` reports what DOORS predicted ✅ (`make powercut` FAT runs; 30 of 30 matched while it was developed; planted cases of each problem on E: and H: in `make test`; a volume where writing is unsafe is mounted read-only) |
| 1.6 | ext4 system disk: boot from an ext4 volume (GRUB on the disk), `\DOORS\` on ext4 ✅ (1.7.0) | `make smoke` passes with C: = ext4 ✅ (`smoke64.py --ext4`, part of `make smoke`: 25 rows, the last one checks the written disk with e2fsck and boots it again) |
| 1.7 | Free-space and quota edge cases: disk full, inode full, 0-byte files, maximum name lengths ✅ (1.8.0) | A test fills each volume to 100 % and back; e2fsck clean ✅ (`make fulltest`: FAT32, FAT16, FAT12, ext4 1K and 4K filled and freed, free space back exactly; edge cases, inode-full and root-full in every `make test`) |

## Stage 2: test and quality infrastructure (1.4)

Other operating systems stay stable because every change is tested automatically. Build that next.

| Step | What | Done when |
|---|---|---|
| 2.1 | **Continuous integration**: `make test`, `make smoke`, `make powercut` on every push (GitHub Actions, QEMU) ◐ (1.9.0: `.github/workflows/ci.yml` written, its steps pass in a clean checkout; it runs once the repository is on GitHub) | Every commit shows pass/fail; `main` only gets passing commits (branch protection, a GitHub setting) |
| 2.2 | **System-call fuzzer**: a ring-3 program calling all 111 calls with random, edge-case and hostile arguments | 24 h of fuzzing with no panic, hang or leak (free-page count returns to baseline) |
| 2.3 | **Soak test**: a mixed random workload (CLIs, editor, services, file churn, job kills) for hours, with checks of resource counts ◐ (1.10.1: `make soak` written; its first run found a FILESYS crash when a program is stopped mid-request, fixed under 3.4) | 72 h without a failure |
| 2.4 | **Kernel debugging aids**: stack protector (`-fstack-protector-strong`), poisoned freed pages, assertions on all internal invariants, a panic report with a stack trace and the last kernel messages ✅ (1.9.0) | Each aid has a test that triggers it on purpose ✅ (`make crashtest`: 7 cases; `kcheck` covers the scheduler, messaging and page allocator, more modules can add checks) |
| 2.5 | **Static analysis**: `gcc -fanalyzer`, `cppcheck` and `clang --analyze` clean in CI ✅ (1.9.0) | Zero warnings, kept at zero ✅ (`make analyze`; 1 intended finding on the allow list) |
| 2.6 | **Coverage**: measure which kernel lines the tests run (gcov through QEMU) and add tests for the gaps ✅ (1.10.0; `-fsanitize-coverage=trace-pc` instead of gcov) | ≥ 80 % of kernel lines covered ✅ (`make coverage`: 81.3 % by `make test` alone, test code not counted; smoke, powercut and crashtest runs not merged in yet) |
| 2.7 | **Crash reports**: a panic writes its report to serial and to a reserved disk area; the next boot shows it ✅ (1.9.0) | A forced panic is readable after reboot ✅ (sectors 1984–2047 of the boot disk; `make crashtest`) |

## Stage 3: harden the kernel core (1.5 – 1.6)

| Step | What | Done when |
|---|---|---|
| 3.1 | **Kernel heap** (`kmalloc`) and growable tables instead of fixed arrays (jobs, tasks, exchanges, request blocks, open files) ✅ (1.11.0) | Limits come from memory, not compile-time constants; the soak test runs 1,000 jobs ✅ (`make test` runs 1,000 jobs at once, three times, with no leaks; the soak test itself waits for 3.4) |
| 3.2 | **Resource exhaustion**: every "out of X" path returns its MMURTL error and leaves the system working | A test exhausts each resource in turn (pages, exchanges, link blocks, RQBs, timers, tasks, files) and the system recovers |
| 3.3 | **Per-job limits**: no single program can take all memory or all exchanges | The fuzzer at full speed can't starve the monitor or the CLIs |
| 3.4 | **Service robustness**: requests can time out, and a service that crashes (FILESYS, KEYBOARD, a user service) is restarted or reported, not left hanging its clients | Killing a service mid-request gives its clients an error, not a hang |
| 3.5 | **Watchdog**: detects a task that holds the CPU at high priority or a kernel stuck with interrupts off | A runaway priority-1 task is reported and can be killed from the monitor |
| 3.6 | **Fix the small known issues**: renaming a directory into itself, upsvc's WaitMsg handling, rare problem 3a in PROBLEM-SOLN.md (a CLI loaded during heavy file system load) | Each one has a regression test |

## Stage 4: real hardware (2.0)

QEMU is friendly. Real machines are not, and this is where most young operating systems fail.

| Step | What | Done when |
|---|---|---|
| 4.1 | **ACPI**: read the tables, power off and reboot properly | `shutdown` and `reboot` commands work on real PCs |
| 4.2 | **APIC, I/O APIC, HPET/TSC timers** instead of the legacy PIC and PIT | Interrupts and timing are correct on machines without legacy hardware |
| 4.3 | **PCI enumeration** and a driver model (drivers found by device ID) | `devices` lists everything on the bus |
| 4.4 | **AHCI (SATA) driver** with DMA and interrupts; keep ATA PIO as a fallback | Disk throughput ≥ 10× PIO; `make powercut` passes on it |
| 4.5 | **NVMe driver** | Boots and runs from NVMe |
| 4.6 | **USB**: xHCI host controller, HID keyboard, USB mass storage | A USB keyboard works and a USB stick shows up as a FAT volume |
| 4.7 | **UEFI boot** and a framebuffer text console (no VGA text mode on UEFI machines) | Boots on UEFI-only PCs |
| 4.8 | **Hardware test lab**: a list of real machines and virtual machines tested before each release | 5 real PCs and 3 hypervisors pass `make smoke`'s checklist |

## Stage 5: several processors (2.x)

| Step | What | Done when |
|---|---|---|
| 5.1 | Start the other CPUs; per-CPU data and run queues | `-smp 4` boots, all CPUs run tasks |
| 5.2 | Replace `irq_save` (the only lock today) with spinlocks and per-object locks; lock-order checking in debug builds | No deadlocks or races found by the soak test with 4 and 8 CPUs |
| 5.3 | TLB shootdown for shared memory (`AliasMem`), safe page freeing | Aliasing stress test on 8 CPUs |
| 5.4 | Keep MMURTL's message semantics exact under concurrency | All M5 messaging tests pass with `-smp 8`, repeatedly |

## Stage 6: security and isolation (2.x)

| Step | What | Done when |
|---|---|---|
| 6.1 | SMEP, SMAP and stricter page permissions in the kernel; kernel address randomisation | The fuzzer can't make the kernel run or read user memory by accident |
| 6.2 | Address randomisation for user programs (stack, heap, load address) | Two runs of a program use different addresses |
| 6.3 | **Users and permissions**: log-in, the user name MMURTL already has (`SetUserName`), and ext4 permission bits enforced | A user can't change another user's files |
| 6.4 | Service permissions: which jobs may register which service names, and which may call `KillJob` | A program can't impersonate FILESYS or kill other users' jobs |
| 6.5 | Review: a security audit of the system-call layer and the file systems | Findings fixed; regression tests added |

## Stage 7: a usable system (2.x – 3.0)

| Step | What | Done when |
|---|---|---|
| 7.1 | `malloc` and a fuller C library (or a newlib port) | Common C programs compile unchanged |
| 7.2 | CLI improvements: wildcards (`*.*`), history, redirection with `SetSysIn`/`SetSysOut`, batch files | The file commands take wildcards; output can go to a file |
| 7.3 | The **debugger** (MMURTL's, rewritten for 64-bit, with a disassembler): the deferred smoke test row P7 | Smoke test 25/25 |
| 7.4 | **Installer**: put DOORS on a disk with GRUB, from the ISO | Installs to a real disk and boots from it |
| 7.5 | **Networking**: its own stage now, [Stage 8](#stage-8-networking-2x--30) | — |
| 7.6 | **Self-hosting**: CM32 and DASM on 64-bit (and/or a GCC port), so DOORS can build its own programs | A user program is built and run inside DOORS |

## Stage 8: networking (2.x – 3.0)

MMURTL had no network. DOORS adds one the MMURTL way: the network is a **system service**, like
FILESYS. Programs reach it by name through `Request`/`Respond`, and the driver only moves frames.
A BSD-socket layer in the C library sits on top, so ported programs work unchanged. Everything is
tested in QEMU first (user-mode networking, with a host-side peer written in Python), then on the
hardware lab of Stage 4. Needs PCI (4.3), the kernel heap (3.1) and `malloc` (7.1).

| Step | What | Done when |
|---|---|---|
| 8.1 | **Network card drivers**: virtio-net (QEMU, KVM) first, then Intel e1000/e1000e (QEMU, VirtualBox, VMware, many real PCs), with interrupts and DMA rings. A `NETDEV` service sends and receives Ethernet frames; the monitor's `net` command shows the card, MAC address and counters | Frames go both ways with QEMU's user-mode network; counters match the host's capture (`-object filter-dump`) |
| 8.2 | **IPv4 core**: ARP, IPv4, ICMP and UDP, with a fixed address first | `ping 10.0.2.2` from the monitor gets replies; the host can ping DOORS (QEMU `hostfwd`) |
| 8.3 | **DHCP and DNS clients**: an address, gateway and DNS server from the network; `resolve NAME` | DOORS gets its address from QEMU's DHCP and from a real router; `resolve` returns the same addresses as the host |
| 8.4 | **TCP**: connections, retransmission, flow and congestion control, timeouts. Own small stack, or a port of lwIP behind the service, chosen by a spike first | 100 MB transfer to a host peer arrives byte for byte, also with 5 % of packets dropped and reordered by a host-side proxy; 100 connections open and close without a leak |
| 8.5 | **The `TCPIP` service and its calls**: MMURTL-style request codes (open, connect, listen, accept, send, receive, close) with error codes in `erc.h`; a BSD-socket shim (`socket`, `connect`, `send`, `recv`, `select`, ...) in the C library | A DOORS program fetches a page from the host's web server; a small BSD-socket sample compiles unchanged |
| 8.6 | **Network tools**: `ping`, `net` (configure and show), a TFTP and HTTP client (`get URL file`), and a file transfer to and from the host | A file copied host → DOORS → host comes back byte for byte, to FAT and to ext4 |
| 8.7 | **Remote use**: a remote CLI (telnet first, SSH later) and a small HTTP server for files | Two remote sessions and a local CLI work at once; killing a session frees everything |
| 8.8 | **Robustness**: the packet parsers (Ethernet, ARP, IP, ICMP, UDP, TCP, DHCP, DNS) built for Linux with sanitizers, as `make fuzz` does for the file systems, and fed random and damaged packets; limits on buffers, connections and half-open sockets | Millions of fuzzed packets: no crash, hang or leak; a flood (SYN flood, ping flood) slows DOORS down but doesn't stop the CLI or the disk |
| 8.9 | **Tests in CI**: `make nettest` boots DOORS in QEMU with a host-side peer and runs 8.1–8.7 | Runs on every push (Stage 2.1) |
| 8.10 | **Later**: IPv6 (with SLAAC), TLS (a port of BearSSL or mbedTLS) for HTTPS and SSH, a network file system client (NFS or SMB) | Each gets its own test when it is planned |

Security for the network (who may open ports, a default-deny inbound rule) belongs with Stage 6:
service permissions (6.4) decide which jobs may use `TCPIP`.

## Stage 9: release engineering (from 1.4 on, running alongside)

| Step | What |
|---|---|
| 9.1 | Release checklist: CI green, soak 72 h, fuzz 24 h, power-cut test, network tests, hardware lab, CHANGELOG, manual updated |
| 9.2 | Long-term support branch: fixes only, for users who need a version that stays put |
| 9.3 | Public bug tracker; every bug fix comes with a regression test |
| 9.4 | Compatibility promise: the MMURTL API (names, parameters, error codes) and the DOORS additions (system calls numbered from 110) never change incompatibly within a major version |
| 9.5 | Security fixes released quickly, with notes on what was affected |

---

## Order and priorities

1. **Stage 1 (storage) and Stage 2 (testing) first.** Losing data or crashing are the failures users
   never forgive, and without automatic testing every later change risks breaking what works.
2. **Stage 3 (core hardening)** before new features, so the kernel survives resource exhaustion and
   misbehaving services.
3. **Stage 4 (real hardware)** next. It is the biggest jump in effort, and it is what turns DOORS
   from an emulator project into an OS people can install. It is the natural point for **DOORS 2.0**.
4. **Stages 5–8** in the order users ask for them. SMP and security matter more for servers; the
   usable-system steps matter more for hobbyists and teaching. **Networking (Stage 8)** can start in
   QEMU as soon as PCI enumeration (4.3) and the kernel heap (3.1) exist, before the rest of
   Stage 4; ping and UDP (8.1–8.2) need no more than that. TCP and the socket calls want `malloc`
   (7.1).
5. **Stage 9** starts with continuous integration in Stage 2 and grows with every release.

Each step follows the same rule as the 64-bit port: a release only when its test passes, the
manual updated, the CHANGELOG entry written, and the step logged in `devlogs/`.

## Effort, roughly

| Stage | Size | Why |
|---|---|---|
| 1 Storage | medium | builds on the working ext4/FAT code |
| 2 Testing | medium | mostly tools and scripts around what exists |
| 3 Core hardening | medium | careful work in kernel internals |
| 4 Real hardware | **large** | many drivers, and real-machine debugging is slow |
| 5 SMP | large | touches every lock in the kernel |
| 6 Security | medium–large | new concepts (users, permissions) on top of MMURTL's design |
| 7 Usable system | large | many independent pieces |
| 8 Networking | **large** | drivers, a TCP/IP stack, a new service and socket API; TCP alone is big (less if lwIP is ported) |
| 9 Releases | small, ongoing | process more than code |
