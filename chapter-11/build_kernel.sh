#!/bin/bash
for f in src/*.c platform/kernel/*.c; do
    gcc -m32 -ffreestanding -O0 -Wall -Wextra -iquote include -iquote src -iquote platform/kernel -c "$f"
done
as --32 platform/kernel/boot.s -o boot.o

ld -m elf_i386 -T kernel.ld -o kernel.bin *.o
