#!/bin/bash
SRC=""
for d in src platform/cli; do
    for f in "$d"/*.c; do
        [ -f "$f" ] && SRC="$SRC $f"
    done
done
gcc -Wall -Wextra -DDEBUG=${DEBUG:-0} -iquote include -iquote src -iquote platform/cli -o fat12-cli $SRC
