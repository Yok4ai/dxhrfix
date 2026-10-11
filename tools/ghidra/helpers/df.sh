#!/bin/bash
# df.sh file addr...: print decompiled functions by entry address
f=$1; shift
for a in "$@"; do a=$(printf "%08x" $((16#$a))); awk -v a="$a" '/^\/\/ =====/{p=($3==a)} p' "$f"; done
