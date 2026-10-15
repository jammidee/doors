/* DOORS user programs: static ELF64, linked at 0x40000000 like MMURTL user code. Explicit
   program headers give each part its own permissions, which the loader maps as such:
   code R+X, read-only data R, data and bss R+W (no execute). */
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);    /* R X */
    rodata PT_LOAD FLAGS(4);    /* R   */
    data   PT_LOAD FLAGS(6);    /* R W */
}

SECTIONS
{
    . = 0x40000000;

    .text : ALIGN(4K)   { *(.text._start) *(.text .text.*) } :text
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) } :rodata
    .data : ALIGN(4K)   { *(.data .data.*) } :data
    .bss : ALIGN(4K)    { *(COMMON) *(.bss .bss.*) } :data

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) }
}
