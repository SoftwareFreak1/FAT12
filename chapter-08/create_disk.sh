#!/bin/bash
# Create a fresh 4 MiB FAT12 disk image
dd if=/dev/zero of=disk.img bs=1M count=4
mkfs.fat -F 12 -s 2 -S 512 -n MYDISK disk.img
