# Multiboot header: identifies this binary to the bootloader as bootable
.set MAGIC,    0x1BADB002        # Magic number the bootloader scans for
.set FLAGS,    0                 # No optional Multiboot features requested
.set CHECKSUM, -(MAGIC + FLAGS)  # magic + flags + checksum must sum to zero
.section .multiboot              # Must land in the kernel's first 8 KB
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .text
.global _start                   # Make _start visible to the linker as our entry point

_start:
    mov $stack_top, %esp         # Point ESP at our stack before any C code runs
    call kernel_main             # Hand off to the C kernel

halt:                            # kernel_main returns here when done; nothing left to run
    cli                          # disable interrupts
    hlt                          # halt the CPU
    jmp halt                     # if an NMI wakes it anyway, halt again

# Reserve the stack _start points ESP at, growing down from stack_top
.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:
