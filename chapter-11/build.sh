#!/bin/bash
gcc -Wall -Wextra -iquote include -iquote src -iquote platform/cli \
    src/fat12.c platform/cli/main.c platform/cli/file_block_device.c \
    -o fat12-cli
