#!/bin/bash
gcc -Wall -Wextra -iquote include -iquote platform/cli -iquote src -o fat12-cli \
    platform/cli/main.c src/fat12.c \
    platform/cli/file_block_device.c platform/cli/debug.c
