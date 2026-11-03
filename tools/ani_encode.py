# glTF animation (.glb) -> Far Cry 4 "aNi" clip, using an existing game clip as the template.
# The template gives the header, the track list (crc32 of bone names), root-motion tracks, events and
# names; rotations, positions, flags, frame count and length are rebuilt from the .glb.
import struct, json, math, sys, zlib, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------- glb reading
def read_glb(path):
    d = open(path, 'rb').read()
    L = struct.unpack_from('<I', d, 12)[0]; j = json.loads(d[20:20 + L])
    o = 20 + L; bl = struct.unpack_from('<I', d, o)[0]; b = d[o + 8:o + 8 + bl]
    def accessor(i):
        a = j['accessors'][i]; bv = j['bufferViews'][a['bufferView']]
        n = {'SCALAR': 1, 'VEC3': 3, 'VEC4': 4}[a['type']]; off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        st = bv.get('byteStride', 4 * n); out = []
        for k in range(a['count']):
            v = struct.unpack_from('<%df' % n, b, off + k * st); out.append(v[0] if n == 1 else v)
        return out
    anim = j['animations'][0]
    tracks = {}                                           # node name -> {'rotation': (times, vals), ...}
    for ch in anim['channels']:
        s = anim['samplers'][ch['sampler']]; node = j['nodes'][ch['target']['node']].get('name', '')
        tracks.setdefault(node, {})[ch['target']['path']] = (accessor(s['input']), accessor(s['output']))
    return tracks

def sample(times, vals, t, is_q):
    if t <= times[0]: return vals[0]
    if t >= times[-1]: return vals[-1]
    lo, hi = 0, len(times) - 1
    while hi - lo > 1:
        m = (lo + hi) // 2
        if times[m] <= t: lo = m
        else: hi = m
    f = (t - times[lo]) / (times[hi] - times[lo]); a, b = vals[lo], vals[hi]
    if not is_q: return tuple(a[i] + (b[i] - a[i]) * f for i in range(3))
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0: b = tuple(-x for x in b); dot = -dot
    q = tuple(a[i] + (b[i] - a[i]) * f for i in range(4)); n = math.sqrt(sum(x * x for x in q))
    return tuple(x / n for x in q)

# ---------------------------------------------------------------- bit writer
class BitW:
    def __init__(s): s.bits = []
    def put(s, v, n):
        for i in range(n): s.bits.append((v >> i) & 1)
    def bytes(s):
        out = bytearray((len(s.bits) + 7) // 8)
        for i, b in enumerate(s.bits):
            if b: out[i >> 3] |= 1 << (i & 7)
        return bytes(out) + b'\0\0\0\0'                    # the game reads up to 4 bytes past a value

# ---------------------------------------------------------------- component encoding (shared by rot/pos)
def enc_component(bw, vals, bits):
    """vals in [-1, 1] for one block; returns True if written as constant (header bit)"""
    mn, mx = min(vals), max(vals)
    if mx - mn < 2e-5:
        bw.put(max(0, min(65535, round((vals[0] + 1) * 32767.5))), 16); return True
    return False
def enc_component_data(bw, vals, bits):
    mn, mx = min(vals), max(vals)
    qmin = max(0, min(255, math.floor((mn + 1) * 127)))
    m = qmin / 127 - 1
    qr = max(1, min(255, math.ceil((mx - m) * 127.5)))
    rg = qr / 127.5
    bw.put(qmin, 8); bw.put(qr, 8)
    top = (1 << bits) - 1
    for v in vals: bw.put(max(0, min(top, round((v - m) / rg * top))), bits)

def drop_index(q):
    return max(range(4), key=lambda i: abs(q[i]))

def enc_rot_block(bw, keys, bits):
    # one dropped component for the whole block: the largest on average
    idx = max(range(4), key=lambda i: sum(abs(q[i]) for q in keys))
    ks = [q if q[idx] >= 0 else tuple(-x for x in q) for q in keys]       # dropped one >= 0: no sign mask
    comps = [[q[i] for q in ks] for i in range(4) if i != idx]
    hdr = idx << 4
    tmp = []
    for c in range(3):
        if max(comps[c]) - min(comps[c]) < 2e-5: hdr |= 1 << c
    bw.put(hdr, 6)
    for c in range(3):
        if (hdr >> c) & 1: bw.put(max(0, min(65535, round((comps[c][0] + 1) * 32767.5))), 16)
        else: enc_component_data(bw, comps[c], bits)

def enc_pos_block(bw, keys, bits, scale):
    comps = [[p[i] / scale for p in keys] for i in range(3)]
    hdr = 0
    for c in range(3):
        if max(comps[c]) - min(comps[c]) < 2e-5: hdr |= 1 << c
    bw.put(hdr, 3)
    for c in range(3):
        if (hdr >> c) & 1: bw.put(max(0, min(65535, round((comps[c][0] + 1) * 32767.5))), 16)
        else: enc_component_data(bw, comps[c], bits)

def enc_const_quat(q):
    idx = drop_index(q)
    if q[idx] < 0: q = tuple(-x for x in q)
    o = [q[i] for i in range(4) if i != idx]
    v = [max(0, min(32767, round((x + 0.70710677) / 4.315969e-05))) for x in o]
    t0, t1 = {3: (1, 1), 2: (0, 1), 1: (1, 0), 0: (0, 0)}[idx]
    return struct.pack('<3H', v[0] | (t0 << 15), v[1] | (t1 << 15), v[2])

def anim_region(nkeys, per_track_blocks):
    """per_track_blocks(block_index, nk) -> BitW for that block (all tracks). Returns region bytes."""
    nb = (nkeys + 7) // 8
    blocks = [per_track_blocks(b, min(8, nkeys - 8 * b)).bytes() for b in range(nb)]
    head = 4 * (nb + 1); offs = []; o = head
    for bl in blocks: offs.append(o); o += len(bl)
    offs.append(o)
    return struct.pack('<%dI' % len(offs), *offs) + b''.join(blocks)

# ---------------------------------------------------------------- the clip
# section offsets: u16 low part at +lo, high byte at +hi.  0x38 = nested aNi clips for attached objects (the weapon's own
# animation, per hand), 0x3a = event block (EntityEvent...): copied as they are, but MOVED with the clip.
U16_FIELDS = [(0x28, 0x3e), (0x2a, 0x3f), (0x2c, 0x40), (0x2e, 0x41), (0x30, 0x42), (0x32, 0x43), (0x34, 0x44), (0x36, 0x45),
              (0x38, 0x46), (0x3a, 0x47)]

def encode(template, glb, out, bone_names, fps=None, bits=16):
    d = bytearray(open(template, 'rb').read()); C = 0x18
    g16 = lambda o: struct.unpack_from('<H', d, C + o)[0]; g32 = lambda o: struct.unpack_from('<I', d, C + o)[0]
    if g16(4) & 0xc000: raise ValueError('template uses a header layout not supported yet')
    ntr = g16(4) & 0xfff; frames0 = g16(6); len0 = struct.unpack_from('<f', d, C + 8)[0]
    fps = fps or (frames0 / len0 if len0 > 0 else 30.0)
    toff = g32(0x14); foff = g32(0x10)
    hashes = [struct.unpack_from('<I', d, C + toff + 4 * i)[0] for i in range(ntr)]
    name_of = {zlib.crc32(n.encode()): n for n in bone_names}
    tr = read_glb(glb)
    dur = max(max(v[0]) for t in tr.values() for v in t.values())
    frames = max(1, round(dur * fps)); keys = frames + 1; length = frames / fps
    times = [k / fps for k in range(keys)]
    old_flags = list(d[C + foff:C + foff + ntr])
    rot, pos = [], []
    for i in range(ntr):
        nm = name_of.get(hashes[i]); t = tr.get(nm, {})
        r = [sample(*t['rotation'], x, True) for x in times] if 'rotation' in t else None
        p = [sample(*t['translation'], x, False) for x in times] if 'translation' in t else None
        if r is None and old_flags[i] & 0x30: raise ValueError('bone %s has no rotation in the .glb' % nm)
        rot.append(r); pos.append(p)
    # classify
    flags = []; crot = []; arot = []; cpos = []; apos = []
    for i in range(ntr):
        f = 0
        r = rot[i]
        if r is not None:
            ref = r[0]
            if all(abs(abs(sum(a * b for a, b in zip(q, ref))) - 1) < 1e-7 for q in r): f |= 0x30; crot.append(i)
            else: f |= 0x10 | (bits - 1); arot.append(i)
        p = pos[i]
        if p is not None and old_flags[i] & 0xc0:
            if all(max(abs(a - b) for a, b in zip(q, p[0])) < 1e-6 for q in p): f |= 0xc0; cpos.append(i)
            else: f |= 0x40; apos.append(i)
        flags.append(f)
    # regions we rebuild
    reg = {}
    reg[0x28] = b''.join(enc_const_quat(rot[i][0]) for i in crot)
    compact = d[C + 3] & 0x20
    reg[0x2a] = b''.join((struct.pack('<3h', *[max(-32768, min(32767, round(v * 16384))) for v in pos[i][0]]) if compact
                          else struct.pack('<3f', *pos[i][0])) for i in cpos)
    extra = (d[C + 0x45] << 16) | g16(0x36)
    old_sc = (d[C + 0x41] << 16) | g16(0x2e)
    scales = []
    for i in apos:
        m = max(abs(v) for p in pos[i] for v in p) or 1e-3
        q24 = min(0xffffff, math.ceil(m / 0.00029802325) + 1); scales.append((i, q24 * 0.00029802325, q24))
    sc_bytes = (d[C + old_sc:C + old_sc + 4] if extra and old_sc else b'') + b''.join(struct.pack('<I', bits | (q24 << 8)) for _, _, q24 in scales)
    reg[0x2e] = sc_bytes
    def rot_blocks(b, nk):
        bw = BitW()
        for i in arot: enc_rot_block(bw, rot[i][8 * b:8 * b + nk], bits)
        return bw
    def pos_blocks(b, nk):
        bw = BitW()
        for i, sc, _ in scales: enc_pos_block(bw, pos[i][8 * b:8 * b + nk], bits, sc)
        return bw
    reg[0x30] = anim_region(keys, rot_blocks) if arot else b''
    reg[0x32] = anim_region(keys, pos_blocks) if apos else b''
    # every section of the template, in file order: (start, field)
    secs = []
    for lo, hi in U16_FIELDS:
        o = (d[C + hi] << 16) | g16(lo)
        if o: secs.append((o, ('u16', lo, hi)))
    secs.append((toff, ('u32', 0x14))); secs.append((foff, ('u32', 0x10)))
    secs.sort()
    clip_end = len(d) - C
    first = secs[0][0]
    body = bytearray(d[C:C + first])                       # header (+ anything before the first section)
    new_off = {}
    produced = set()
    for k, (o, fld) in enumerate(secs):
        end = secs[k + 1][0] if k + 1 < len(secs) else clip_end
        key = fld[1]
        rebuilt = fld[0] == 'u16' and key in reg
        while len(body) % 4 or (not rebuilt and (len(body) - o) % 16): body.append(0)    # copied sections keep their alignment
        start = len(body)
        if rebuilt: chunk = reg[key]; produced.add(key)
        elif fld == ('u32', 0x10): chunk = bytes(flags) + bytes(d[C + o + ntr:C + end])        # new flags + the rest (names/events)
        else: chunk = bytes(d[C + o:C + end])
        body += chunk; new_off[fld] = start if chunk or fld[0] == 'u32' else 0
    # sections the template didn't have but we need (e.g. animated positions)
    for key in (0x28, 0x2a, 0x2e, 0x30, 0x32):
        if key not in produced and reg.get(key):
            while len(body) % 4: body.append(0)
            new_off[('u16', key, dict(U16_FIELDS)[key])] = len(body); body += reg[key]
    # write offsets + counts
    for fld, o in new_off.items():
        if fld[0] == 'u32': struct.pack_into('<I', body, fld[1], o)
        else: struct.pack_into('<H', body, fld[1], o & 0xffff); body[fld[2]] = (o >> 16) & 0xff
    struct.pack_into('<H', body, 6, frames); struct.pack_into('<f', body, 8, length)
    struct.pack_into('<H', body, 0x18, len(crot)); struct.pack_into('<H', body, 0x1a, len(cpos))
    struct.pack_into('<H', body, 0x1c, len(arot)); struct.pack_into('<H', body, 0x1e, len(apos))
    struct.pack_into('<H', body, 0x20, len(scales) + (1 if extra and old_sc else 0))
    res = bytearray(d[:C]) + body
    open(out, 'wb').write(res)
    return dict(frames=frames, length=length, const_rot=len(crot), anim_rot=len(arot), const_pos=len(cpos), anim_pos=len(apos), size=len(res))

if __name__ == '__main__':
    import ani2glb
    tmpl, glb, skel, out = sys.argv[1:5]
    names = [n['name'] for n in ani2glb.skeleton(skel)]
    print(encode(tmpl, glb, out, names))
