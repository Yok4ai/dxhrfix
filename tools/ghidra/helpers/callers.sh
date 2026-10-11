#!/bin/bash
# callers.sh <hexaddr>: list "call 0xADDR" / "jmp 0xADDR" sites with containing function
O=${DXHR_OUT:-out}
grep -E "(call|jmp) +0x$1\$" $O/DXHRDC.objdump.txt | awk '{print $1}' | tr -d ':' | while read a; do
  python3 -I -c "
import bisect
fs=[]
for l in open('$O/DXHRDC.exe.functions.tsv'):
  p=l.split('\t')
  try: fs.append((int(p[0],16),p[1]))
  except: pass
fs.sort(); k=[f[0] for f in fs]; a=int('$a',16); i=bisect.bisect_right(k,a)-1
print('%x in %s'%(a,fs[i][1]))"; done
