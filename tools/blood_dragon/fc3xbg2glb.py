r"""Far Cry 3 / Blood Dragon model (.xbg, HSEM v52) -> .glb, one Blender object per part (FRAME, SLIDE,
CLIP, HAMMER...), in model space, so FC4Hook's Workshop > Model import can put each piece on the
matching bone of a Far Cry 4 base weapon.

usage: python fc3xbg2glb.py model.xbg out.glb [--lod 0] [--extracted <folder>]

--extracted: a folder made by dunia_extract (files named <id>.<type> in folders by type). The model's
materials (m54414d00\<id>.*) and their colour (_d) and normal (_n) textures (xbt\<id>.xbt, joined with
their _mip0 half) are looked up there and packed into the .glb as PNGs (needs Pillow, numpy). The
shine/occlusion/gloss mask (_m) goes in as the metal/roughness texture marked extras.dunia_mask, which
FC4Hook 1.13.2+ copies as the game's own mask; normal maps are turned from Dunia's (X in alpha) into RGB.

The layout is the one FC4Hook's importer reads from Far Cry 4 models (src/glbwrite.cpp), which
Far Cry 3 shares: RMTL materials, NODE/SKID bones, SKND > CLUS parts, LODS geometry (vertex formats
0xCA/20 B and 0xBCA/32 B), PCMP/UCMP compression. Vertices are stored relative to their part's bone.
Game axes (x right, y forward, z up) -> glTF (x, z, -y). Triangles are clockwise in the game.
"""
import struct, json, sys, math

def rd32(d, o): return struct.unpack_from('<I', d, o)[0]
def rdf(d, o): return struct.unpack_from('<f', d, o)[0]

def chunks(d, off, end):
    out = {}
    while off + 20 <= end:
        tag = d[off:off + 4][::-1].decode('latin1'); size, dsz = rd32(d, off + 8), rd32(d, off + 12)
        if size < 20 or off + size > end or 20 + dsz > size: break
        out[tag] = (off, size, dsz); off += size
    return out

def qmul(a, b):
    return (a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1], a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
            a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3], a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2])
def qrot(q, v):
    u = q[:3]; w = q[3]
    c1 = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
    c2 = (u[1]*c1[2]-u[2]*c1[1], u[2]*c1[0]-u[0]*c1[2], u[0]*c1[1]-u[1]*c1[0])
    return (v[0] + 2*(w*c1[0] + c2[0]), v[1] + 2*(w*c1[1] + c2[1]), v[2] + 2*(w*c1[2] + c2[2]))

# where the normal sits in each vertex layout (position int16 x4 at 0, uv int16 x2 at 8 in all of them)
#   0xCA  20 B: pos uv normal colour            0xBCA 32 B: pos uv uv1 normal colour tangent binormal
#   0xDA  28 B: pos uv weights bones normal colour   0xBDA 40 B: pos uv uv1 weights bones normal colour tangent binormal
NORMAL_AT = {(0xCA, 20): 12, (0xBCA, 32): 16, (0xDA, 28): 20, (0xBDA, 40): 24}
WEIGHTS_AT = {(0xDA, 28): 12, (0xBDA, 40): 16}       # skinned: 4 weights (u8), then 4 palette slots (u8)

def read(path, lod=0):
    d = open(path, 'rb').read()
    assert d[:4] == b'HSEM', 'not an .xbg'
    C = chunks(d, 32, len(d))
    # materials
    o = C['RMTL'][0] + 20; n = rd32(d, o); o += 8; mats = []
    for _ in range(n):
        l = rd32(d, o); mp = d[o + 4:o + 4 + l].decode('latin1'); o += 4 + l + 1
        l = rd32(d, o); mn = d[o + 4:o + 4 + l].decode('latin1'); o += 4 + l + 1
        mats.append((mp, mn))
    # parts
    sk = C['SKND'][0]; clus = sk + 20
    assert d[clus:clus + 4] == b'SULC'
    c0 = clus + 20; cq = c0; cend = c0 + rd32(d, clus + 12); ent = []
    while cq < cend:
        k = rd32(d, cq); cq += 4; ent.append([cq + 174 * i for i in range(k)]); cq += 174 * k
    names = []; cur = ''
    for i in range(clus + rd32(d, clus + 8), sk + C['SKND'][1]):
        ch = chr(d[i])
        if ch.isalnum() or ch == '_': cur += ch; continue
        if d[i] == 0 and len(cur) >= 4 and '_LOD' in cur and len(names) < len(ent): names.append(cur)
        cur = ''
    # bones
    a = C['NODE'][0] + 20; e = a + C['NODE'][2]; nn = rd32(d, a); a += 4
    par, loc, nname = [], [], []
    for _ in range(nn):
        par.append(struct.unpack_from('<i', d, a + 12)[0])
        loc.append((struct.unpack_from('<4f', d, a + 16), struct.unpack_from('<3f', d, a + 32)))
        l = rd32(d, a + 68); nname.append(d[a + 72:a + 72 + l].decode('latin1')); a += 72 + l + 1
    world = []
    for k, (q, t) in enumerate(loc):
        p = par[k]
        if p < 0 or p >= k: world.append((q, t)); continue
        Pq, Pt = world[p]; rt = qrot(Pq, t)
        world.append((qmul(Pq, q), (Pt[0] + rt[0], Pt[1] + rt[1], Pt[2] + rt[2])))
    s = C['SKID'][0] + 20; nb = rd32(d, s)
    boneNode = [struct.unpack_from('<H', d, s + 4 + 12 * k + 6)[0] for k in range(nb)]
    partXf = []
    for pi in range(len(ent)):
        nd = boneNode[pi] if len(boneNode) == len(ent) else next((k for k, x in enumerate(nname) if x.upper() == names[pi].rsplit('_LOD', 1)[0]), -1)
        partXf.append(world[nd] if 0 <= nd < len(world) else ((0, 0, 0, 1), (0, 0, 0)))
    pOff, pScale = rdf(d, C['PCMP'][0] + 20), rdf(d, C['PCMP'][0] + 24)
    uOff, uScale = rdf(d, C['UCMP'][0] + 20), rdf(d, C['UCMP'][0] + 24)
    # LODS
    L = C['LODS'][0] + 20; nl = rd32(d, L); q = L + 4
    def pad(at): return (16 - at % 16) % 16
    lods = []
    for _ in range(nl):
        dist = rdf(d, q); nf = rd32(d, q + 4); q += 8
        fmts = [struct.unpack_from('<4I', d, q + 16 * i) for i in range(nf)]; q += 16 * nf
        ns = rd32(d, q); q += 4
        recs = [struct.unpack_from('<7I', d, q + 28 * i) for i in range(ns)]; q += 28 * ns
        vbytes = rd32(d, q); vstart = q + 4 + pad(q + 4); ie = vstart + vbytes
        ni = rd32(d, ie); istart = ie + 4 + pad(ie + 4)
        idx = struct.unpack_from('<%dH' % ni, d, istart); q = istart + 2 * ni
        lods.append(dict(dist=dist, fmts=fmts, recs=recs, vstart=vstart, idx=idx))
    lo = lods[min(lod, len(lods) - 1)]
    parts = {}
    for k, (fmt, part, entry, first, last, vbo, _) in enumerate(lo['recs']):
        cnt = (lo['recs'][k + 1][3] if k + 1 < len(lo['recs']) else len(lo['idx'])) - first
        if cnt < 3: continue
        flags, stride, count, boff = lo['fmts'][fmt]
        if (flags, stride) not in NORMAL_AT: raise ValueError('unknown vertex format %x/%d' % (flags, stride))
        base = lo['vstart'] + boff
        tri = lo['idx'][first:first + cnt]
        if len(set(tri)) < 3: continue
        used = sorted(set(tri)); remap = {v: i for i, v in enumerate(used)}
        Q, T = partXf[part]
        P, N, UV, BN = [], [], [], []
        pal = struct.unpack_from('<48H', d, ent[part][entry] + 14) if part < len(ent) and entry < len(ent[part]) else (0xffff,) * 48
        partNodeIx = boneNode[part] if len(boneNode) == len(ent) else -1
        for v in used:
            vo = base + v * stride
            x, y, z, _w = struct.unpack_from('<4h', d, vo)
            u, vv = struct.unpack_from('<2h', d, vo + 8)
            no = vo + NORMAL_AT[(flags, stride)]
            nz, ny, nx = [(b - 128) / 127.0 for b in d[no:no + 3]]
            lp = (x * pScale + pOff, y * pScale + pOff, z * pScale + pOff)
            bone = partNodeIx
            if flags & 0x10:                                         # skinned to several bones: already in model space
                gp = lp; gn = (nx, ny, nz)
                wo = vo + WEIGHTS_AT[(flags, stride)]; w = d[wo:wo + 4]; bi = d[wo + 4:wo + 8]
                sl = bi[max(range(4), key=lambda i: w[i])]
                if sl < 48 and pal[sl] < len(nname): bone = pal[sl]
            else:                                                    # rigid: stored relative to the part's bone
                gp = qrot(Q, lp); gp = (gp[0] + T[0], gp[1] + T[1], gp[2] + T[2]); gn = qrot(Q, (nx, ny, nz))
            ln = math.sqrt(sum(c * c for c in gn)) or 1
            P.append((gp[0], gp[2], -gp[1])); N.append((gn[0] / ln, gn[2] / ln, -gn[1] / ln))
            UV.append((u * uScale + uOff, vv * uScale + uOff)); BN.append(nname[bone] if 0 <= bone < len(nname) else '')
        I = []
        for t in range(0, len(tri) - 2, 3):                      # clockwise -> counter-clockwise
            a_, b_, c_ = tri[t], tri[t + 1], tri[t + 2]
            if len({a_, b_, c_}) == 3: I += [remap[a_], remap[c_], remap[b_]]
        pname = names[part].rsplit('_LOD', 1)[0] if part < len(names) else 'PART%d' % part
        mat = struct.unpack_from('<H', d, ent[part][entry])[0] if part < len(ent) and entry < len(ent[part]) else entry
        parts.setdefault(pname, []).append(dict(P=P, N=N, UV=UV, I=I, mat=mat, B=BN))
    # points the base weapon has too (left-hand IK grip, muzzle flash, shell ejection): exported as empties
    # so FC4Hook moves the base weapon's nodes onto this gun
    points = {nname[k]: world[k] for k in range(len(nname)) if nname[k].upper() in ('IK_LEFTHAND', 'FX_FIRE', 'FX_CASING')}
    return dict(parts=parts, mats=mats, names=names, nodes=nname, lods=len(lods), scale=pScale, points=points,
                world=dict(zip(nname, world)))

POLY = 0xD800000000000000
_T = []
for _i in range(256):
    _c = _i
    for _ in range(8): _c = (_c >> 1) ^ POLY if _c & 1 else _c >> 1
    _T.append(_c)
def file_id(p):
    h = 0
    for ch in p.replace('/', '\\').lower().encode(): h = _T[(h ^ ch) & 0xff] ^ (h >> 8)
    return h

def textures(model, folder):
    """material index -> {'d': png bytes, 'n': png bytes} from a dunia_extract folder"""
    import os, re, io, glob
    from PIL import Image
    def find(kind, fid):
        hits = glob.glob(os.path.join(folder, '*', '%016x.*' % fid)) if kind is None else [os.path.join(folder, kind, '%016x.%s' % (fid, kind))]
        return next((h for h in hits if os.path.exists(h)), None)
    def png(path):
        x = open(path, 'rb').read(); dds = x[rd32(x, 8):]
        m0 = find('xbt', file_id(path_of[path][:-4] + '_mip0.xbt'))
        if m0:                                                  # the high-res half holds the top level
            y = open(m0, 'rb').read(); dds = y[rd32(y, 8):]
        hdr = bytearray(dds[:128]); struct.pack_into('<I', hdr, 28, 0); struct.pack_into('<I', hdr, 8, rd32(hdr, 8) & ~0x20000)
        im = Image.open(io.BytesIO(bytes(hdr) + dds[128:])); im.load()
        if path_of[path][-6:-4].lower() == '_n':                # Dunia normal: X in alpha, Y in green -> plain RGB normal
            import numpy as np
            a = np.asarray(im.convert('RGBA')).astype(np.float32) / 255 * 2 - 1
            x, y = a[..., 3], a[..., 1]; z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
            im = Image.fromarray((np.stack([x, y, z], -1) * 127.5 + 127.5).clip(0, 255).astype(np.uint8), 'RGB')
        b = io.BytesIO(); im.convert('RGB').save(b, 'PNG'); return b.getvalue()
    path_of = {}; out = {}
    for i, (mp, mn) in enumerate(model['mats']):
        mf = find(None, file_id(mp))
        if not mf: continue
        md = open(mf, 'rb').read(); got = {}
        for t in re.findall(rb'graphics\\[\x20-\x7e]+?\.xbt', md):
            t = t.decode(); kind = t[-6:-4].lower()
            if kind not in ('_d', '_n', '_m'): kind = '_d'          # e.g. an Unlit (glowing) LED: led_07.xbt is its colour
            if kind[1] in got: continue
            xf = find('xbt', file_id(t))
            if xf: path_of[xf] = t; got[kind[1]] = png(xf)
        if b'Unlit' in md and 'd' in got:                          # glowing LED: greyscale mask x the material's colour
            col = None
            for o in range(8, len(md) - 12):
                if md[o - 1] in (12, 16):
                    c = struct.unpack_from('<3f', md, o)
                    if all(0 <= x <= 20 for x in c) and max(c) > 0.05: col = c; break
            if col:
                import numpy as np
                a = np.asarray(Image.open(io.BytesIO(got['d'])).convert('L')).astype(np.float32)
                a = a / max(1.0, a.max())                             # full brightness: Far Cry 4 has no glow here
                rgb = np.stack([a * min(1, c) for c in col], -1) * 255
                b = io.BytesIO(); Image.fromarray(rgb.clip(0, 255).astype(np.uint8), 'RGB').save(b, 'PNG'); got['d'] = b.getvalue()
        if got: out[i] = got
    return out

def write_glb(model, out, tex=None):
    bin_ = bytearray(); J = {'asset': {'version': '2.0', 'generator': 'fc3xbg2glb'}, 'scene': 0, 'scenes': [{'nodes': []}],
                             'nodes': [], 'meshes': [], 'accessors': [], 'bufferViews': [], 'buffers': [], 'materials': []}
    def add(data, fmt, comps, typ, target, mm=False):
        while len(bin_) % 4: bin_.append(0)
        off = len(bin_)
        for row in data: bin_.extend(struct.pack('<' + fmt * comps, *row) if comps > 1 else struct.pack('<' + fmt, row))
        J['bufferViews'].append({'buffer': 0, 'byteOffset': off, 'byteLength': len(bin_) - off, 'target': target})
        a = {'bufferView': len(J['bufferViews']) - 1, 'componentType': 5126 if fmt == 'f' else 5125, 'count': len(data), 'type': typ}
        if mm: a['min'] = [min(r[i] for r in data) for i in range(3)]; a['max'] = [max(r[i] for r in data) for i in range(3)]
        J['accessors'].append(a); return len(J['accessors']) - 1
    matIx = {}; J['images'] = []; J['textures'] = []; imgIx = {}
    def image(data):
        if id(data) in imgIx: return imgIx[id(data)]
        while len(bin_) % 4: bin_.append(0)
        off = len(bin_); bin_.extend(data)
        J['bufferViews'].append({'buffer': 0, 'byteOffset': off, 'byteLength': len(data)})
        J['images'].append({'bufferView': len(J['bufferViews']) - 1, 'mimeType': 'image/png'})
        J['textures'].append({'source': len(J['images']) - 1}); imgIx[id(data)] = len(J['textures']) - 1
        return imgIx[id(data)]
    for pname, pieces in model['parts'].items():
        prims = []
        for pc in pieces:
            if not pc['I']: continue
            key = pc['mat']
            if key not in matIx:
                mname = model['mats'][key][1] if key < len(model['mats']) else 'material_%d' % key
                mat = {'name': mname, 'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.6, 0.6, 1], 'metallicFactor': 0.2, 'roughnessFactor': 0.6}}
                t = (tex or {}).get(key, {})
                if 'd' in t: mat['pbrMetallicRoughness'] = {'baseColorTexture': {'index': image(t['d'])}, 'metallicFactor': 0.2, 'roughnessFactor': 0.6}
                if 'n' in t: mat['normalTexture'] = {'index': image(t['n'])}
                if 'm' in t:                                     # the game's shine/occlusion/gloss mask, passed as it is
                    mat['pbrMetallicRoughness']['metallicRoughnessTexture'] = {'index': image(t['m'])}
                    mat['extras'] = {'dunia_mask': 1}
                matIx[key] = len(J['materials']); J['materials'].append(mat)
            prims.append({'attributes': {'POSITION': add(pc['P'], 'f', 3, 'VEC3', 34962, True), 'NORMAL': add(pc['N'], 'f', 3, 'VEC3', 34962),
                                         'TEXCOORD_0': add(pc['UV'], 'f', 2, 'VEC2', 34962)}, 'indices': add(pc['I'], 'I', 1, 'SCALAR', 34963), 'material': matIx[key]})
        if not prims: continue
        J['meshes'].append({'name': pname, 'primitives': prims})
        J['nodes'].append({'name': pname, 'mesh': len(J['meshes']) - 1}); J['scenes'][0]['nodes'].append(len(J['nodes']) - 1)
    for nm, (q, t) in model.get('points', {}).items():         # game (x, y, z) -> glTF (x, z, -y)
        J['nodes'].append({'name': nm, 'translation': [t[0], t[2], -t[1]], 'rotation': [q[0], q[2], -q[1], q[3]]})
        J['scenes'][0]['nodes'].append(len(J['nodes']) - 1)
    J['buffers'] = [{'byteLength': len(bin_)}]
    js = json.dumps(J, separators=(',', ':')).encode()
    while len(js) % 4: js += b' '
    while len(bin_) % 4: bin_.append(0)
    open(out, 'wb').write(struct.pack('<III', 0x46546C67, 2, 28 + len(js) + len(bin_)) + struct.pack('<II', len(js), 0x4E4F534A) + js
                          + struct.pack('<II', len(bin_), 0x004E4942) + bytes(bin_))

def split_by_bone(model, mapping):
    """pieces -> one object per base-weapon node: a triangle goes to mapping[its first vertex's bone] (e.g.
    BARREL_01 -> PKM_Tripod2), the rest stays in its part. FC4Hook puts an object named like one of the base
    model's skinned nodes on that node, so the game's animations move it."""
    out = {}
    for pname, pieces in model['parts'].items():
        for pc in pieces:
            groups = {}
            for t in range(0, len(pc['I']), 3):
                tri = pc['I'][t:t + 3]
                groups.setdefault(mapping.get(pc['B'][tri[0]], pname), []).append(tri)
            for gname, tris in groups.items():
                used = sorted({v for tri in tris for v in tri}); remap = {v: i for i, v in enumerate(used)}
                out.setdefault(gname, []).append(dict(P=[pc['P'][v] for v in used], N=[pc['N'][v] for v in used], UV=[pc['UV'][v] for v in used],
                                                      B=[pc['B'][v] for v in used], I=[remap[v] for tri in tris for v in tri], mat=pc['mat']))
    model['parts'] = out

if __name__ == '__main__':
    a = sys.argv[1:]; lod = int(a[a.index('--lod') + 1]) if '--lod' in a else 0
    m = read(a[0], lod)
    if '--map' in a:                                             # --map BARREL_01=PKM_Tripod2,FLAP_01=PKM_AmmoLid
        split_by_bone(m, dict(kv.split('=') for kv in a[a.index('--map') + 1].split(',')))
    for i, x in enumerate(a):                                    # --point NAME=x,y,z[,qx,qy,qz,qw] (game space): move that node
        if x == '--point':
            nm, v = a[i + 1].split('='); v = [float(f) for f in v.split(',')]
            m['points'][nm] = (tuple(v[3:7]) if len(v) >= 7 else (0, 0, 0, 1), tuple(v[:3]))
    tex = textures(m, a[a.index('--extracted') + 1]) if '--extracted' in a else None
    if tex is not None: print('textures for materials:', {m['mats'][k][1]: sorted(v) for k, v in tex.items()})
    write_glb(m, a[1], tex)
    print('points:', sorted(m['points']), '| parts:', {k: sum(len(p['I']) // 3 for p in v) for k, v in m['parts'].items()}, '| materials:', [x[1] for x in m['mats']], '| LODs:', m['lods'])
