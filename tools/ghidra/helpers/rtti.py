# rtti.py > vtables.tsv: MSVC RTTI vtables (VA, class, offset) of $DXHR_EXE,
# input for Chains.java.
import os,struct,re,sys
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from pe import d,secs,off
def sec(n): return [s for s in secs if s[0]==n][0]
rn,rva,rvs,rra,rrs=sec('.rdata'); dn,dva,dvs,dra,drs=sec('.data')
# type descriptors in .data: name '.?AV' at td+8
tds={}
for m in re.finditer(rb'\.\?A[VU][^\x00]{1,200}\x00', d[dra:dra+drs]):
    tds[dva+m.start()-8]=m.group()[:-1].decode()
rdat=d[rra:rra+rrs]
cols={}
for i in range(0,len(rdat)-20,4):
    sig,o,cd,td,ch=struct.unpack_from('<5I',rdat,i)
    if sig==0 and td in tds and rva<=ch<rva+rvs:
        cols[rva+i]=(tds[td],o)
vt=[]
for i in range(0,len(rdat)-4,4):
    v=struct.unpack_from('<I',rdat,i)[0]
    if v in cols:
        vt.append((rva+i+4,cols[v][0],cols[v][1]))
for a,n,o in vt: print("%x\t%s\t+%x"%(a,n,o))
