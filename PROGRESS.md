<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# DOORS progress: what changed, and when

One row per change to the system, newest first. **When**, **version** and **commit** let you find it
in git (`git show v1.4.0`); **step** links it to [PLAN.md](PLAN.md) (milestones M0–M10) or
[ROADMAP.md](ROADMAP.md) (stages after 1.0); **devlog** has the details: what was found and why it
was done that way. What each version means for users is in [CHANGELOG.md](CHANGELOG.md).

Every commit that changes the system adds a row here (CLAUDE.md, "Versioning"). A commit can't
contain its own hash, so the newest row says *(this commit)* and the next commit fills in the hash;
`git describe` or the version tag always finds it.

| When | Version | Commit | Step | What changed | Devlog |
|---|---|---|---|---|---|
| 2026-10-21 | 1.12.0 | *(this commit)* | ROADMAP 3.2 ✅ | Resource exhaustion test; a waiting task gets its message without a link block | [10-21](devlogs/2026-10-21.md) |
| 2026-10-21 | 1.11.0 | `1286745` | ROADMAP 3.1 ✅ | Growable kernel tables and the kernel heap; job numbers reused; 1,000 jobs at once in `make test` | [10-21](devlogs/2026-10-21.md) |
| 2026-10-21 | 1.10.1 | `a22b692` | ROADMAP 2.3 (unfinished) | Soak test (`make soak`, `churn`); its first run found a FILESYS crash, left open for 3.4 | [10-21](devlogs/2026-10-21.md) |
| 2026-10-19 | 1.10.0 | `8aace56` | ROADMAP 2.6 | Test coverage: `make coverage`, 81.3 % of kernel lines by `make test` | [10-19](devlogs/2026-10-19.md) |
| 2026-10-19 | 1.9.0 | `514582e` | ROADMAP 2.1, 2.4, 2.5, 2.7 | Static analysis clean (3 real bugs fixed); stack protector, page poisoning, `kcheck`; panic stack traces; crash reports on disk; `make analyze`, `make crashtest`; CI workflow | [10-19](devlogs/2026-10-19.md) |
| 2026-10-19 | 1.8.0 | `b4ccc24` | ROADMAP 1.7 (Stage 1 ✅) | Full volumes: clean `ErcDiskFull`, ext4 rollback of failed operations; FILESYS path-length security fix; faster FAT writes; `df`; `make fulltest` | [10-19](devlogs/2026-10-19.md) |
| 2026-10-19 | 1.7.0 | `bb50bef` | ROADMAP 1.6 (M8b ✅) | Booting from an ext4 system disk (GRUB on the disk, no ISO); the CLI finds commands in any case; `make smoke` on both system disks | [10-19](devlogs/2026-10-19.md) |
| 2026-10-19 | 1.6.0 | `a0d2159` | ROADMAP 1.5 | FAT after a power cut: dirty flag, chkdsk-style check at mount that matches `fsck.fat`, read-only when writing is unsafe; FAT power-cut runs. Also: this file, ROADMAP Stage 8 (networking) | [10-19](devlogs/2026-10-19.md) |
| 2026-10-18 | 1.5.0 | `fc0e610` | ROADMAP 1.4 | Damaged-disk fuzzer (`make fuzz`); ext4 fixes for damaged extent trees, directory sizes and superblocks; hashed block cache (`make test` 240 → 106 s) | [10-18](devlogs/2026-10-18.md) |
| 2026-10-18 | 1.4.0 | `8a68574` | ROADMAP 1.3 | ext4 journals with checksums (csum_v3) written instead of read-only | [10-18](devlogs/2026-10-18.md) |
| 2026-10-18 | 1.3.0 | `0ad8e35` | ROADMAP 1.2 (M8b) | ext4 extent trees of any depth on write; faster journal commits | [10-18](devlogs/2026-10-18.md) |
| 2026-10-18 | 1.2.0 | `b592160` | ROADMAP 1.1 (M8b) | Writing big (htree) ext4 directories; GPT test disk | [10-18](devlogs/2026-10-18.md) |
| 2026-10-16 | 1.1.0 | `d16bc65` | M8b | The ext4 journal: replay and journaled writes; power-cut test | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 1.0.0 | `65b62bb` | M9 ✅ | The 64-bit port passes MMURTL's smoke test | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.9.3 | `3a454ad` | M9 (9d) | Editor and sample programs | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.9.2 | `5c909f6` | M9 (9c) | MMURTL's CLI on 64-bit | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.9.1 | `71f9732` | M9 (9b) | Job calls and a C library | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.9.0 | `8b15d30` | M9 (9a) | Virtual screens and the keyboard service | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.8.1 | `b6eec16` | — | System manual (devman/) and versioning | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.8.0 | `859f14d` | M8 ✅ | ext4 read/write, ReadDir, stress test; doors.bat | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.7.0 | `57ed784` | M7 ✅ | ATA disks, partitions, FAT12/16/32 with long names, file calls | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.6.0 | `caca727` | M6 ✅ | Ring 3, SYSCALL, ELF64 jobs, user-mode services | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.5.0 | `46509a3` | M5 ✅ | Exchanges, messages, request blocks, named services | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.4.0 | `6d7fb89` | M4 ✅ | Tasks, priority scheduler with time slicing, Sleep | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.3.0 | `706f199` | M3 ✅ | Physical/virtual memory, address spaces, MMURTL memory calls | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.2.0 | `0952cf5` | M2 ✅ | Interrupts, exceptions, 100 Hz timer, keyboard | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | 0.1.0 | `53f4be7` | M1 ✅ | DOORS boots in x86-64 long mode via GRUB/Multiboot2 | [10-16](devlogs/2026-10-16.md) |
| 2026-10-16 | — | `5088892` | M0 ✅ | Original 32-bit MMURTL boots in QEMU; smoke test complete; OS renamed DOORS | [10-16](devlogs/2026-10-16.md) |
| 2026-10-15 | — | `d775b74` | — | Pristine MMURTL V1.0 source imported | [10-15](devlogs/2026-10-15.md) |
