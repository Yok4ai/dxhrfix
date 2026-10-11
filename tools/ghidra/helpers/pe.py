# pe.py secs | b VA [n] | dw VA n | s VA | dbl VA | flt VA: read the exe by VA.
# The exe path comes from the DXHR_EXE environment variable.
import os,struct,sys
EXE=os.environ.get("DXHR_EXE")
if not EXE: sys.exit("set DXHR_EXE to the path of your DXHRDC.exe")
d=open(EXE,'rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]
ns=struct.unpack_from('<H',d,pe+6)[0]; so=struct.unpack_from('<H',d,pe+20)[0]
secs=[]
for i in range(ns):
    o=pe+24+so+i*40
    name=d[o:o+8].rstrip(b'\0').decode(); vs,va,rs,ra=struct.unpack_from('<IIII',d,o+8)
    secs.append((name,0x400000+va,vs,ra,rs))
def off(v):
    for n,va,vs,ra,rs in secs:
        if va<=v<va+max(vs,rs): return ra+(v-va)
def rd(v,n): o=off(v); return d[o:o+n]
if __name__=="__main__":
    cmd=sys.argv[1]
    if cmd=="secs":
        for s in secs: print(s[0],hex(s[1]),hex(s[2]),hex(s[3]),hex(s[4]))
    elif cmd=="b":  # bytes
        v=int(sys.argv[2],16); n=int(sys.argv[3]) if len(sys.argv)>3 else 8
        print(hex(v), rd(v,n).hex(' '))
    elif cmd=="dw":
        v=int(sys.argv[2],16); n=int(sys.argv[3])
        print(' '.join(hex(x) for x in struct.unpack_from('<%dI'%n, rd(v,4*n))))
    elif cmd=="s":
        v=int(sys.argv[2],16); b=rd(v,256); print(b.split(b'\0')[0])
    elif cmd=="dbl":
        v=int(sys.argv[2],16); print(struct.unpack('<d',rd(v,8))[0])
    elif cmd=="flt":
        v=int(sys.argv[2],16); print(struct.unpack('<f',rd(v,4))[0])
