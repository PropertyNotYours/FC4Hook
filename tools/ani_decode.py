import struct, math, sys
SQ=4.315969e-05; H=0.70710677
def cquat(v0,v1,v2):
    a=(v0&0x7fff)*SQ-H; b=(v1&0x7fff)*SQ-H; c=struct.unpack('<h',struct.pack('<H',v2))[0]*SQ-H
    r=math.sqrt(max(0,1-a*a-b*b-c*c)); t0=v0&0x8000; t1=v1&0x8000
    if t0 and t1: return (a,b,c,r)
    if not t0 and t1: return (a,b,r,c)
    if t0 and not t1: return (a,r,b,c)
    return (r,a,b,c)
class Bits:
    def __init__(s,d,base): s.d=d; s.base=base
    def get(s,pos,n):
        v=0
        for i in range(n):
            p=pos+i; v|=((s.d[s.base+(p>>3)]>>(p&7))&1)<<i
        return v
def parse(d):
    C=0x18; g=lambda o:struct.unpack_from('<H',d,C+o)[0]
    ntr=g(4)&0xfff; frames=g(6); length=struct.unpack_from('<f',d,C+8)[0]
    nconst=g(0x18); nanim=g(0x1c)
    coff=(d[C+0x3e]<<16)|g(0x28); aoff=(d[C+0x42]<<16)|g(0x30)
    foff=struct.unpack_from('<I',d,C+0x10)[0]
    flags=list(d[C+foff:C+foff+ntr])
    const=[cquat(*struct.unpack_from('<3H',d,C+coff+6*i)) for i in range(nconst)]
    rot=[None]*ntr; ci=0; anim=[]
    for t in range(ntr):
        k=flags[t]&0x30
        if k==0x30: rot[t]=('c',const[ci]); ci+=1
        elif k==0x10: anim.append(t)
        else: rot[t]=('none',None)
    keys=frames+1; nblocks=(keys+7)//8
    offs=[struct.unpack_from('<I',d,C+aoff+4*i)[0] for i in range(nblocks+1)]
    seq={t:[] for t in anim}
    for b in range(nblocks):
        nk=min(8,keys-8*b)
        bs=Bits(d,C+aoff+offs[b]); pos=0
        for t in anim:
            bits=(flags[t]&0xf)+1
            hdr=bs.get(pos,6); pos+=6
            sign=0
            if hdr&8: sign=bs.get(pos,nk); pos+=nk
            comps=[]
            for c in range(3):
                if (hdr>>c)&1:
                    v=bs.get(pos,16)*3.051851e-05-1; pos+=16; comps.append([v]*nk)
                else:
                    mn=bs.get(pos,8)/127-1; rg=bs.get(pos+8,8)/127.5; pos+=16
                    sc=1/((1<<bits)-1)
                    vals=[]
                    for k in range(nk): vals.append(rg*sc*bs.get(pos,bits)+mn); pos+=bits
                    comps.append(vals)
            big=(hdr>>4)&3
            for k in range(nk):
                a,b2,c=comps[0][k],comps[1][k],comps[2][k]
                r=math.sqrt(max(0,1-a*a-b2*b2-c*c))
                if (sign>>k)&1: r=-r
                q=[a,b2,c]; q.insert(big,r)
                seq[t].append(tuple(q))
    for t in anim: rot[t]=('a',seq[t])
    # ---- translations
    compact = d[C+3] & 0x20
    tcoff=(d[C+0x3f]<<16)|g(0x2a); taoff=(d[C+0x43]<<16)|g(0x32)
    tsoff=(d[C+0x41]<<16)|g(0x2e); nts=g(0x20); extra=(d[C+0x45]<<16)|g(0x36)
    nta=g(0x1e)
    pos=[None]*ntr; tanim=[]; p0=C+tcoff
    for t in range(ntr):
        k=flags[t]&0xc0
        if k==0xc0:
            if compact: v=tuple(x/16384 for x in struct.unpack_from('<3h',d,p0)); p0+=6
            else: v=struct.unpack_from('<3f',d,p0); p0+=12
            pos[t]=('c',v)
        elif k==0x40: tanim.append(t)
    scales=[]
    for i in range(nts):
        w=struct.unpack_from('<I',d,C+tsoff+4*i)[0]
        scales.append(((w&0xff), (w>>8)*0.00029802325))
    si=1 if extra else 0
    if tanim and nta:
        offs=[struct.unpack_from('<I',d,C+taoff+4*i)[0] for i in range(nblocks+1)]
        tseq={t:[] for t in tanim}
        for b in range(nblocks):
            nk=min(8,keys-8*b); bs=Bits(d,C+taoff+offs[b]); q=0
            for j,t in enumerate(tanim):
                bits,sc=scales[si+j]
                hdr=bs.get(q,3); q+=3
                comps=[]
                for c in range(3):
                    if (hdr>>c)&1: v=bs.get(q,16)*3.051851e-05-1; q+=16; comps.append([v]*nk)
                    else:
                        mn=bs.get(q,8)/127-1; rg=bs.get(q+8,8)/127.5; q+=16; f2=1/((1<<bits)-1); vals=[]
                        for kk in range(nk): vals.append(rg*f2*bs.get(q,bits)+mn); q+=bits
                        comps.append(vals)
                for kk in range(nk): tseq[t].append(tuple(sc*comps[c][kk] for c in range(3)))
        for t in tanim: pos[t]=('a',tseq[t])
    return dict(tracks=ntr,frames=frames,length=length,flags=flags,rot=rot,anim=anim,blocks=nblocks,pos=pos,tanim=tanim,scales=scales,extra=extra)
if __name__=='__main__':
    for f in sys.argv[1:]:
        d=open(f,'rb').read(); r=parse(d)
        print(f, 'frames',r['frames'],'len %.3f'%r['length'],'anim tracks',r['anim'])
        for t in r['anim'][:3]:
            ks=r['rot'][t][1]; worst=max(abs(1-sum(x*x for x in q)) for q in ks)
            jump=max(math.acos(min(1,abs(sum(a*b for a,b in zip(ks[i],ks[i+1])))))*2*57.3 for i in range(len(ks)-1)) if len(ks)>1 else 0
            print('   track',t,'keys',len(ks),'first',tuple(round(x,3) for x in ks[0]),'max step %.1f deg'%jump,'unit err %.3f'%worst)
        cs=[p[1] for p in r['pos'] if p and p[0]=='c']
        print('   const positions',len(cs),'e.g.',[tuple(round(x,3) for x in v) for v in cs[:4]])
        print('   animated positions',r['tanim'],'scales',[(b,round(s2,4)) for b,s2 in r['scales'][:6]],'extra',r['extra'])
        for t in r['tanim'][:3]:
            ks=r['pos'][t][1]; st=max(math.dist(ks[i],ks[i+1]) for i in range(len(ks)-1)) if len(ks)>1 else 0
            print('     pos track',t,'first',tuple(round(x,3) for x in ks[0]),'last',tuple(round(x,3) for x in ks[-1]),'max step %.4f m'%st)
