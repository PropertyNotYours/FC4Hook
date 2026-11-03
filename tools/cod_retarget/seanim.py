import struct
def read(path):
    d=open(path,'rb').read()
    assert d[:6]==b'SEAnim'
    anim_type=d[10]; flags=d[11]; pres=d[12]; prop=d[13]
    fps=struct.unpack_from('<f',d,16)[0]; frames,bones=struct.unpack_from('<II',d,20); mods=d[28]; notes=struct.unpack_from('<I',d,32)[0]
    p=36; names=[]
    for i in range(bones):
        e=d.index(b'\0',p); names.append(d[p:e].decode()); p=e+1
    isz=1 if bones<=0xff else (2 if bones<=0xffff else 4)
    fsz=1 if frames<=0xff else (2 if frames<=0xffff else 4)
    def ri(sz):
        nonlocal p
        v=int.from_bytes(d[p:p+sz],'little'); p+=sz; return v
    modt={}
    for i in range(mods): b=ri(isz); modt[names[b]]=d[p]; p+=1
    dbl = bool(prop & 1)
    fl='d' if dbl else 'f'; fz=8 if dbl else 4
    tracks={}
    for i in range(bones):
        bf=d[p]; p+=1
        t={}
        if pres & 1:
            n=ri(fsz); t['pos']=[(ri(fsz),)+struct.unpack_from('<3'+fl,d,p+0) if False else None for _ in range(0)]
            keys=[]
            for _ in range(n):
                f=ri(fsz); v=struct.unpack_from('<3'+fl,d,p); p+=3*fz; keys.append((f,v))
            t['pos']=keys
        if pres & 2:
            n=ri(fsz); keys=[]
            for _ in range(n):
                f=ri(fsz); v=struct.unpack_from('<4'+fl,d,p); p+=4*fz; keys.append((f,v))
            t['rot']=keys
        if pres & 4:
            n=ri(fsz); keys=[]
            for _ in range(n):
                f=ri(fsz); v=struct.unpack_from('<3'+fl,d,p); p+=3*fz; keys.append((f,v))
            t['scale']=keys
        tracks[names[i]]=t
    nl=[]
    if pres & 0x40:
        for _ in range(notes):
            f=ri(fsz); e=d.index(b'\0',p); nl.append((f,d[p:e].decode())); p=e+1
    return dict(type=anim_type,fps=fps,frames=frames,names=names,mods=modt,tracks=tracks,notes=nl,end=p,size=len(d))
