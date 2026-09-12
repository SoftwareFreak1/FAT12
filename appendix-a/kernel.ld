/* _start, defined in boot.s, is the entry point */
ENTRY(_start)

/* Optional — fixes the linker's RWX LOAD segment warning by granting each segment only the permissions it needs */
PHDRS
{
    text   PT_LOAD FLAGS(5); /* R+X */
    rodata PT_LOAD FLAGS(4); /* R   */
    data   PT_LOAD FLAGS(6); /* R+W */
}

/* Lays out where each section goes in the final kernel binary */
SECTIONS
{
    /* Our kernel binary starts loading here — an address Multiboot loaders guarantee is free RAM */
    . = 1M;

    /* Groups the kernel's code, read-only data, and writable data/bss into their own PT_LOAD segments, each with only the permissions it needs */
    .text : {
        KEEP(*(.multiboot))  /* Keeps the Multiboot header from boot.s in the first 8 KB, where the bootloader looks for it */
        *(.text*)
    } :text
    .rodata : { *(.rodata*) } :rodata
    .data   : { *(.data*) } :data
    .bss    : { *(COMMON) *(.bss*) } :data
}
