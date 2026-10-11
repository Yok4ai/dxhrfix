# ctx.py before after ADDR...: show instructions around addresses
import sys,pickle,bisect,os
ins,fs=pickle.load(open(os.path.join(os.environ.get("DXHR_OUT","out"),"ins.pkl"),'rb'))
ia=[i[0] for i in ins]; k=[f[0] for f in fs]
b=int(sys.argv[1]); af=int(sys.argv[2])
for t in sys.argv[3:]:
    a=int(t,16); i=bisect.bisect_left(ia,a); j=bisect.bisect_right(k,a)-1
    print("---- %x in %s"%(a,fs[j][1]))
    for x in ins[max(0,i-b):i+af+1]: print("  %x%s %s"%(x[0],'*' if x[0]==a else ' ',x[1]))
