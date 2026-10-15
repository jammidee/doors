; DOORS user program entry. The kernel starts the first task of a job here with RSP 16-byte
; aligned (the top of the job's stack) and all other registers zero.
;   main() returns -> ExitJob(return value), as a MMURTL program ended with ExitJob.

bits 64
section .text._start

global _start
extern main
extern ExitJob

_start:
    xor ebp, ebp
    call main
    mov edi, eax
    call ExitJob
.hang:
    jmp .hang
