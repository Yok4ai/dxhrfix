import pickle,re,bisect,os,sys
ins,fs=pickle.load(open(os.path.join(os.environ.get("DXHR_OUT","out"),"ins.pkl"),'rb'))
k=[f[0] for f in fs]
def fof(a): i=bisect.bisect_right(k,a)-1; return fs[i][1]
tgt=sys.argv[1]
from collections import defaultdict
use=defaultdict(list)
for i,(a,t) in enumerate(ins):
    if t=='call   0x'+tgt:
        for j in range(i+1,min(len(ins),i+14)):
            s=ins[j][1]
            m=re.match(r'call\s+DWORD PTR \[e[a-z]{2}\+(0x[0-9a-f]+)\]$',s)
            if m: use[int(m.group(1),16)].append((ins[j][0],fof(a))); break
            m=re.match(r'call\s+(e[a-z]{2})$',s)
            if m:
                r=m.group(1)
                for q in range(j-1,i,-1):
                    mm=re.match(r'mov\s+'+r+r',DWORD PTR \[e[a-z]{2}\+(0x[0-9a-f]+)\]$',ins[q][1])
                    if mm: use[int(mm.group(1),16)].append((ins[j][0],fof(a))); break
                break
            if s.startswith('call') or s.startswith('ret'): break
for off in sorted(use):
    print("slot +0x%x (%d sites): %s"%(off,len(use[off]),' '.join("%x@%s"%(x,f) for x,f in use[off][:40])))
