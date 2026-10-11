# d3dscan.py [window]: indirect calls that look like D3D11 device/context vtable calls
import re,sys,bisect,os
O=os.environ.get("DXHR_OUT","out")+"/"
funcs=[]
for l in open(O+"DXHRDC.exe.functions.tsv"):
    p=l.rstrip('\n').split('\t')
    try: funcs.append((int(p[0],16),p[1]))
    except: pass
funcs.sort(); fa=[f[0] for f in funcs]
def fof(a):
    i=bisect.bisect_right(fa,a)-1; return "%s@%x"%(funcs[i][1],funcs[i][0]) if i>=0 else "?"
ins=[]
rx=re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+)\t(.*)$')
for l in open(O+"DXHRDC.objdump.txt"):
    m=rx.match(l)
    if m: ins.append((int(m.group(1),16),m.group(3).strip()))
win=int(sys.argv[1]) if len(sys.argv)>1 else 14
callrx=re.compile(r'call\s+DWORD PTR \[(e[a-z]{2})\+(0x[0-9a-f]+)\]')
out=[]
for i,(a,t) in enumerate(ins):
    m=callrx.match(t)
    if m: off=int(m.group(2),16)
    else:
        m2=re.match(r'call\s+(e[a-z]{2})$',t)
        if not m2: continue
        r=m2.group(1); off=None
        for j in range(i-1,max(0,i-win),-1):
            mm=re.match(r'mov\s+'+r+r',DWORD PTR \[e[a-z]{2}\+(0x[0-9a-f]+)\]',ins[j][1])
            if mm: off=int(mm.group(1),16); break
            mm=re.match(r'mov\s+'+r+r',DWORD PTR \[e[a-z]{2}\]',ins[j][1])
            if mm: off=0; break
        if off is None: continue
    ctx=' | '.join(x[1] for x in ins[max(0,i-win):i])
    kind=None
    if '0x12ab940' in ctx:
        if re.search(r'\+0x24\]',ctx): kind='dev'
        if re.search(r'\+0x2c\]',ctx) and kind is None: kind='ctx'
    if re.search(r'\+0x10\]',ctx) and ('0x150]' in ctx): kind=kind or 'ctx_sm'
    if kind: out.append((a,kind,off,fof(a)))
for a,k,o,f in out: print("%x\t%s\t0x%x\t%s"%(a,k,o,f))
