# xr.py ADDR [ADDR...]: call/jmp/imm references to each address, with containing function
import sys,bisect,re,pickle,os
# Reads $DXHR_OUT/DXHRDC.exe.functions.tsv and $DXHR_OUT/DXHRDC.objdump.txt and
# caches the parsed instructions in $DXHR_OUT/ins.pkl (used by ctx, fsuse, vcall, prol).
O=os.environ.get("DXHR_OUT","out")+"/"
C=os.path.join(os.environ.get("DXHR_OUT","out"),"ins.pkl")
if os.path.exists(C): ins,fs=pickle.load(open(C,'rb'))
else:
    fs=[]
    for l in open(O+"DXHRDC.exe.functions.tsv"):
        p=l.split('\t')
        try: fs.append((int(p[0],16),p[1]))
        except: pass
    fs.sort()
    rx=re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+)\t(.*)$')
    ins=[]
    for l in open(O+"DXHRDC.objdump.txt"):
        m=rx.match(l)
        if m: ins.append((int(m.group(1),16),m.group(3).strip()))
    pickle.dump((ins,fs),open(C,'wb'))
k=[f[0] for f in fs]
def fof(a):
    i=bisect.bisect_right(k,a)-1; return fs[i][1] if i>=0 else '?'
for t in sys.argv[1:]:
    t=t.lower().lstrip('0x') if t.startswith('0x') else t.lower()
    pat=re.compile(r'(^|[^0-9a-fx])0x0*'+t+r'($|[^0-9a-f])')
    print("== 0x"+t)
    for a,s in ins:
        if pat.search(s): print("  %x  %-40s in %s"%(a,s[:60],fof(a)))
