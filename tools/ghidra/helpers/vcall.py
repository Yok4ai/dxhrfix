# vcall.py OFF [OFF..]: indirect calls whose target was loaded from [reg+OFF] (vtable slot)
import sys,pickle,re,bisect,os
ins,fs=pickle.load(open(os.path.join(os.environ.get("DXHR_OUT","out"),"ins.pkl"),'rb'))
k=[f[0] for f in fs]
def fof(a):
    i=bisect.bisect_right(k,a)-1; return "%s"%(fs[i][1])
offs=[int(x,16) for x in sys.argv[1:]]
for i,(a,t) in enumerate(ins):
    m=re.match(r'call\s+(e[a-z]{2})$',t); off=None
    if m:
        r=m.group(1)
        for j in range(i-1,max(0,i-12),-1):
            mm=re.match(r'mov\s+'+r+r',DWORD PTR \[(e[a-z]{2})\+(0x[0-9a-f]+)\]$',ins[j][1])
            if mm: off=int(mm.group(2),16); break
            if re.match(r'(mov|lea|pop|xor)\s+'+r+',',ins[j][1]): break
    else:
        m=re.match(r'call\s+DWORD PTR \[(e[a-z]{2})\+(0x[0-9a-f]+)\]$',t)
        if m: off=int(m.group(2),16)
    if off in offs: print("%x\t0x%x\t%s"%(a,off,fof(a)))
