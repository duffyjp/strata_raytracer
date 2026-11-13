import struct
N = 8
def load(d,k):
    b=open('%s\\accum_%03d.bin'%(d,k),'rb').read(); w,h=struct.unpack('<II',b[:8])
    return w,h,struct.unpack('<%df'%(w*h*4), b[8:])
w,h,_=load('one8',0)
raw=[list(load('one8',k)[2]) for k in range(N)]
M=[raw[0]]+[[raw[k][i]-raw[k-1][i] for i in range(len(raw[0]))] for k in range(1,N)]
y0,y1=int(h*0.72), int(h*0.90)
pix=[(y*w+x)*4+c for y in range(y0,y1) for x in range(2,w-2) for c in range(3)]
V=[[M[k][p] for p in pix] for k in range(N)]
n=len(pix)
def rms(a): 
    return (sum(v*v for v in a)/len(a))**0.5
print("convergence measured in linear space, from one process.")
print("compare the mean of the first k frames with the mean of the rest; for independent")
print("frames that difference should shrink as sqrt(1/k + 1/(8-k)).\n")
base=None
rows=[]
for k in range(1,N):
    a=[sum(V[j][i] for j in range(k))/k for i in range(n)]
    b=[sum(V[j][i] for j in range(k,N))/(N-k) for i in range(n)]
    d=[a[i]-b[i] for i in range(n)]
    rows.append((k, rms(d)))
sig = rows[0][1]**2/ (1/1.0 + 1/(N-1))
for k,v in rows:
    pred = (sig*(1.0/k + 1.0/(N-k)))**0.5
    print("   k=%d   measured %.4f   independent prediction %.4f   ratio %.2f" % (k, v, pred, v/pred))
