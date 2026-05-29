.set MAGIC,    0x1BADB002
.set FLAGS,    0
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .text
.global _start

_start:
    mov $stack_top, %esp
    call kernel_main

halt:
    cli
    hlt
    jmp halt

.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:
