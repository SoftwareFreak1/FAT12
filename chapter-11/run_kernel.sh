#!/bin/bash
qemu-system-x86_64 \
    -kernel kernel.bin \
    -drive file=disk.img,format=raw,if=ide
