# aNi clip + skeleton (.xbg NODE list) -> .glb with an armature and the animation (Blender: File > Import > glTF)
import struct, json, sys, math
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import ani_decode as ani

def skeleton(xbg):
    d = open(xbg, 'rb').read()
    i = d.find(b'EDON'); p = i + 20; e = p + struct.unpack_from('<I', d, i + 12)[0]
    n = struct.unpack_from('<I', d, p)[0]; p += 4; nodes = []
    for k in range(n):
        ph, fc, ns, par = struct.unpack_from('<I3i', d, p)
        q = struct.unpack_from('<4f', d, p + 16); t = struct.unpack_from('<3f', d, p + 32); s = struct.unpack_from('<3f', d, p + 44)
        ln = struct.unpack_from('<I', d, p + 68)[0]; nm = d[p + 72:p + 72 + ln].decode('latin1'); p += 72 + ln + 1
        nodes.append(dict(name=nm, parent=par, q=q, t=t, s=s))
    assert p == e, 'NODE parse mismatch'
    return nodes

def export(clipfile, xbg, out, trackmap=None):
    clip = ani.parse(open(clipfile, 'rb').read()); nodes = skeleton(xbg)
    ntr = clip['tracks']
    # track i -> skeleton node: clip + u32(clip+0x14) holds one crc32(bone name) per track
    import zlib
    d = open(clipfile, 'rb').read(); C = 0x18
    toff = struct.unpack_from('<I', d, C + 0x14)[0]
    byhash = {zlib.crc32(nd['name'].encode()): k for k, nd in enumerate(nodes)}
    tm = trackmap or [byhash.get(struct.unpack_from('<I', d, C + toff + 4 * i)[0]) for i in range(ntr)]
    missing = sum(1 for x in tm if x is None)
    if missing: print('warning:', missing, 'tracks have no matching bone')
    keys = clip['frames'] + 1
    fps = clip['frames'] / clip['length'] if clip['length'] > 0 else 30.0
    times = [k / fps for k in range(keys)]
    gl = {'asset': {'version': '2.0', 'generator': 'FC4Hook ani2glb'}, 'scene': 0, 'nodes': [], 'scenes': [{'nodes': [0]}],
          'buffers': [], 'bufferViews': [], 'accessors': [], 'animations': []}
    bin_ = bytearray()
    def acc(vals, comp, typ, minmax=False):
        off = len(bin_)
        for v in vals: bin_.extend(struct.pack('<%df' % comp, *(v if comp > 1 else [v])))
        while len(bin_) % 4: bin_.append(0)
        gl['bufferViews'].append({'buffer': 0, 'byteOffset': off, 'byteLength': len(vals) * 4 * comp})
        a = {'bufferView': len(gl['bufferViews']) - 1, 'componentType': 5126, 'count': len(vals), 'type': typ}
        if minmax: a['min'] = [min(vals)]; a['max'] = [max(vals)]
        gl['accessors'].append(a); return len(gl['accessors']) - 1
    # root: game is Z-up, glTF Y-up
    gl['nodes'].append({'name': 'FC4_ROOT', 'rotation': [-0.70710678, 0, 0, 0.70710678], 'children': []})
    base = 1
    for k, nd in enumerate(nodes):
        x, y, z, w = nd['q']
        gl['nodes'].append({'name': nd['name'], 'rotation': [x, y, z, w], 'translation': list(nd['t'])})
    for k, nd in enumerate(nodes):
        par = nd['parent']
        tgt = gl['nodes'][0] if par < 0 else gl['nodes'][base + par]
        tgt.setdefault('children', []).append(base + k)
    # a skin, so Blender builds an ARMATURE (otherwise every bone becomes a loose empty)
    import numpy as np
    def m_of(q, t):
        x, y, z, w = q
        R = np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)], [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)], [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
        M = np.eye(4); M[:3, :3] = R; M[:3, 3] = t; return M
    rootM = m_of((-0.70710678, 0, 0, 0.70710678), (0, 0, 0))
    W = [None] * len(nodes)
    for k, nd in enumerate(nodes):
        L = m_of(nd['q'], nd['t'])
        W[k] = (rootM if nd['parent'] < 0 else W[nd['parent']]) @ L
    ibm = []
    for k in range(len(nodes)):
        inv = np.linalg.inv(W[k]); ibm.append(tuple(float(v) for v in inv.T.flatten()))   # column-major
    gl['skins'] = [{'name': 'FC4_Skeleton', 'joints': [base + k for k in range(len(nodes))],
                    'inverseBindMatrices': acc(ibm, 16, 'MAT4'), 'skeleton': base}]
    # Additive layers (breathing, sway...): every constant rotation is the identity. The game adds them on top of
    # another pose, so shown alone the skeleton collapses. Export them on top of the rest pose instead.
    rc = [x for x in clip['rot'] if x and x[0] == 'c']
    additive = len(rc) > 0 and sum(1 for x in rc if abs(x[1][3]) > 0.9999) >= 0.9 * len(rc)
    def qmul(a, b):
        ax, ay, az, aw = a; bx, by, bz, bw = b
        return (aw*bx+ax*bw+ay*bz-az*by, aw*by-ax*bz+ay*bw+az*bx, aw*bz+ax*by-ay*bx+az*bw, aw*bw-ax*bx-ay*by-az*bz)
    if additive: print('additive layer: applied on top of the rest pose')
    tacc = acc(times, 1, 'SCALAR', True)
    samplers, channels = [], []
    for t in range(ntr):
        if t >= len(tm) or tm[t] is None or tm[t] >= len(nodes): continue
        node = base + tm[t]
        r = clip['rot'][t]
        if r and r[0] in ('a', 'c'):
            ks = r[1] if r[0] == 'a' else [r[1]] * keys
            if additive: ks = [qmul(nodes[tm[t]]['q'], q) for q in ks]
            # keep neighbouring quaternions on the same hemisphere
            fixed = [ks[0]]
            for q in ks[1:]:
                if sum(a * b for a, b in zip(q, fixed[-1])) < 0: q = tuple(-x for x in q)
                fixed.append(q)
            samplers.append({'input': tacc, 'output': acc(fixed, 4, 'VEC4'), 'interpolation': 'LINEAR'})
            channels.append({'sampler': len(samplers) - 1, 'target': {'node': node, 'path': 'rotation'}})
        p = clip['pos'][t]
        if p and p[0] in ('a', 'c'):
            ks = p[1] if p[0] == 'a' else [p[1]] * keys
            samplers.append({'input': tacc, 'output': acc(ks, 3, 'VEC3'), 'interpolation': 'LINEAR'})
            channels.append({'sampler': len(samplers) - 1, 'target': {'node': node, 'path': 'translation'}})
    gl['animations'].append({'name': clipfile.split('/')[-1].split('.')[0], 'samplers': samplers, 'channels': channels})
    gl['buffers'].append({'byteLength': len(bin_)})
    js = json.dumps(gl).encode(); js += b' ' * ((4 - len(js) % 4) % 4)
    with open(out, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)))
        f.write(struct.pack('<II', len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack('<II', len(bin_), 0x004E4942)); f.write(bin_)
    return len(nodes), ntr, len(channels)

if __name__ == '__main__':
    print(export(sys.argv[1], sys.argv[2], sys.argv[3]))
