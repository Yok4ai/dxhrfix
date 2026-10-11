# xref.py EXE STRING...: code references to the given strings (WIDE=1 for UTF-16)
import sys, struct
WIDE=False
import os
if os.environ.get("WIDE"): WIDE=True
data=open(sys.argv[1],'rb').read()
pe=struct.unpack_from('<I',data,0x3c)[0]
nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=struct.unpack_from('<H',data,pe+20)[0]
base=struct.unpack_from('<I',data,pe+24+28)[0]
secs=[]
for i in range(nsec):
    o=pe+24+opt+40*i
    name=data[o:o+8].rstrip(b'\0').decode(); vs,va,rs,ro=struct.unpack_from('<IIII',data,o+8)
    secs.append((name,va,vs,ro,rs))
def off2va(off):
    for n,va,vs,ro,rs in secs:
        if ro<=off<ro+rs: return base+va+off-ro
def va2off(v):
    for n,va,vs,ro,rs in secs:
        if base+va<=v<base+va+rs: return v-base-va+ro
text=[s for s in secs if s[0]=='.text'][0]
tb=data[text[3]:text[3]+text[4]]
for s in sys.argv[2:]:
    enc=(s.encode("utf-16le")+b"\0\0") if s.startswith("W:") is False and WIDE else s.encode()+b"\0"
    i=data.find(enc)
    while i!=-1:
        if data[i-1]==0 and (not WIDE or i%2==0):
            v=off2va(i); pat=struct.pack('<I',v)
            refs=[]; j=tb.find(pat)
            while j!=-1:
                refs.append(hex(base+text[1]+j-1)); j=tb.find(pat,j+1)
            print(s, hex(v), refs)
        i=data.find(enc,i+1)
