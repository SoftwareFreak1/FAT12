/* _start, defined in boot.s, is the entry point */
ENTRY(_start)

/* One loadable segment per permission set */
PHDRS
{
    text   PT_LOAD FLAGS(5); /* R+X */
    rodata PT_LOAD FLAGS(4); /* R   */
    data   PT_LOAD FLAGS(6); /* R+W */
}

/* Lays out where each section goes in the final kernel binary */
SECTIONS
{
    /* Load above the BIOS and VGA memory in the first 1 MiB */
    . = 1M;

    .text : {
        /* The bootloader looks for the Multiboot header in the
           first 8 KiB, so it goes first */
        KEEP(*(.multiboot))
        *(.text*)
    } :text
    .rodata : { *(.rodata*) } :rodata
    .data   : { *(.data*) } :data
    .bss    : { *(COMMON) *(.bss*) } :data
}
