#!/bin/bash
gcc -m32 -ffreestanding -O0 -Wall -Wextra -iquote include -iquote src -iquote platform/kernel -c src/fat12.c
gcc -m32 -ffreestanding -O0 -Wall -Wextra -iquote include -iquote src -iquote platform/kernel -c platform/kernel/main.c
gcc -m32 -ffreestanding -O0 -Wall -Wextra -iquote include -iquote src -iquote platform/kernel -c platform/kernel/ata_block_device.c
gcc -m32 -ffreestanding -O0 -Wall -Wextra -iquote include -iquote src -iquote platform/kernel -c platform/kernel/lib.c
gcc -m32 -ffreestanding -O0 -Wall -Wextra -iquote include -iquote src -iquote platform/kernel -c platform/kernel/vga.c
as --32 platform/kernel/boot.s -o boot.o

ld -m elf_i386 -T kernel.ld -o kernel.bin \
    boot.o main.o ata_block_device.o lib.o vga.o fat12.o
