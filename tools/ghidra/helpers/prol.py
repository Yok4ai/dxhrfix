# prol.py ADDR...: (needs DXHR_EXE and DXHR_OUT/ins.pkl, see README)
# first 8 bytes from the exe file, min whole-insn span >= 5, relative-operand flag
import sys,os,pickle,bisect
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from pe import rd
ins,fs=pickle.load(open(os.path.join(os.environ.get("DXHR_OUT","out"),"ins.pkl"),'rb'))
ia=[i[0] for i in ins]
for t in sys.argv[1:]:
    a=int(t,16); b=rd(a,16)
    i=bisect.bisect_left(ia,a)
    if i>=len(ia) or ia[i]!=a: print("%x  %s  (not an insn boundary)"%(a,b[:8].hex(' '))); continue
    span=0; rel=False; j=i
    while span<5:
        n=ins[j+1][0]-ins[j][0]; s=ins[j][1]
        if s.split()[0] in ('call','jmp') and '0x' in s and 'PTR' not in s: rel=True
        if s.startswith('j') and 'PTR' not in s: rel=True
        span+=n; j+=1
    print("0x%X  %s  span=%d%s"%(a,b[:8].hex(' '),span,"  REL" if rel else ""))
