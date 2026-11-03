import struct
def read(path):
    d=open(path,'rb').read(); assert d[:7]==b'SEModel'
    bpres=d[12]; nb=struct.unpack_from('<I',d,14)[0]
    p=29; names=[]
    for i in range(nb):
        e=d.index(b'\0',p); names.append(d[p:e].decode()); p=e+1
    bones=[]
    for i in range(nb):
        flags=d[p]; par=struct.unpack_from('<i',d,p+1)[0]; p+=5
        b={'name':names[i],'parent':par}
        if bpres&1: b['gpos']=struct.unpack_from('<3f',d,p); b['grot']=struct.unpack_from('<4f',d,p+12); p+=28
        if bpres&2: b['lpos']=struct.unpack_from('<3f',d,p); b['lrot']=struct.unpack_from('<4f',d,p+12); p+=28
        if bpres&4: b['scale']=struct.unpack_from('<3f',d,p); p+=12
        bones.append(b)
    return bones
