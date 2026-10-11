#!/bin/bash
# usage: fn.sh <hexaddr> -> print function disassembly (uses functions.tsv sizes)
O=${DXHR_OUT:-out}
a=$(printf "%08x" $((16#$1)))
sz=$(awk -F'\t' -v a="$a" '($1"")==(a""){print $3}' $O/DXHRDC.exe.functions.tsv)
[ -z "$sz" ] && sz=${2:-256}
end=$((16#$1 + sz))
awk -v s=$((16#$1)) -v e=$end '{ if (match($1,/^[0-9a-f]+:$/)) { v=strtonum("0x" substr($1,1,length($1)-1)); if (v>=s && v<e) print; else if (v>=e) exit } }' $O/DXHRDC.objdump.txt
