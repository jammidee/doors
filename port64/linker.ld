/* DOORS x86-64 kernel layout.
 *
 * The boot code (.boot*) is linked and loaded at its physical address (1 MiB), because it runs
 * before paging. Everything else is linked in the higher half (KERNEL_VMA + physical) and loaded
 * just after the boot code. boot.asm maps both views of the first 1 GiB.
 */
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 1M;

    .boot : ALIGN(4K)
    {
        KEEP(*(.multiboot))             /* must be in the first 32 KiB of the file */
        *(.boot.text)
        *(.boot.data)
        *(.boot.bss)
    }

    . = ALIGN(4K);
    . += KERNEL_VMA;
    _kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4K)
    {
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) ALIGN(4K)
    {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4K)
    {
        *(.data .data.*)
    }

    .bss : AT(ADDR(.bss) - KERNEL_VMA) ALIGN(4K)
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    _kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
