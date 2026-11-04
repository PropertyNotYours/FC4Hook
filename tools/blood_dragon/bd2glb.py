"""Far Cry 3 Blood Dragon first-person clip ("AnD") -> .glb for FC4Hook (Workshop > Animations > Use my .glb).

  python bd2glb.py <bd clip> <rex_colt_fp.xbg> <bd weapon.xbg> <pkm.xbg> <out.glb> [--spin]

Blood Dragon clips (resource type 0x62):
  0x00  u32 type, u32 ?, 12 bytes ?, then four 256-bit bone sets: constant rotations (0x14), animated
        rotations (0x34), constant positions (0x54), animated positions (0x74). A set bit k = bone k of the
        clip's skeleton; tracks are stored in bone order.
  0xC0  "AnD" version, f32 length, 10 u32 offsets from header+0x10: root rotation, root position, constant
        rotations, animated rotations, constant positions, animated positions, nested (gun) clips, events,
        next header (a nested clip: another 0xC0 header + "AnD").
  constant rotations: u32 count, u32, 6-byte quaternions (the same 48-bit "smallest three" as Far Cry 4).
  animated rotations: u16 tracks, u16 frames, f32 fps, u32 offsets of each 8-frame block (+ end). A block:
        every track's first key, a byte per track (bit 7-j = frame j has a key), padded to 2, then the other
        keys, track by track. Frames without a key are slerped between the keys around them.
  positions: int16 x3 / 16384 (metres); animated ones every frame, track-interleaved.
The arms' skeleton is the first-person model's NODE list without the root and without *End, *Knee, finger
*4, *Twist* and *Elbow bones; bone names match Far Cry 4's arms. A gun clip's skeleton is the weapon model's
NODE list as it is.

The Overkill's parts go on PKM nodes (see fc3xbg2glb.py --map); each node gets the Blood Dragon part's motion
relative to its own bind pose, so the PKM nodes' bind poses don't matter.
"""
import json, math, re, struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fc3xbg2glb import chunks, rd32

SQ, H = 4.315969e-05, 0.70710677
def cquat(v0, v1, v2):
    a = (v0 & 0x7fff) * SQ - H; b = (v1 & 0x7fff) * SQ - H; c = struct.unpack('<h', struct.pack('<H', v2))[0] * SQ - H
    r = math.sqrt(max(0, 1 - a * a - b * b - c * c)); t0 = v0 & 0x8000; t1 = v1 & 0x8000
    if t0 and t1: return (a, b, c, r)
    if not t0 and t1: return (a, b, r, c)
    if t0 and not t1: return (a, r, b, c)
    return (r, a, b, c)
def q48(B, o): return cquat(*struct.unpack_from('<3H', B, o))
def qm(a, b): return (a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1], a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0], a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3], a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2])
def qi(q): return (-q[0], -q[1], -q[2], q[3])
def qr(q, v): return qm(qm(q, (v[0], v[1], v[2], 0)), qi(q))[:3]
def xm(A, B): r = qr(A[0], B[1]); return (qm(A[0], B[0]), tuple(A[1][i] + r[i] for i in range(3)))
def xi(A): iq = qi(A[0]); return (iq, qr(iq, tuple(-x for x in A[1])))
def slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0: b = tuple(-x for x in b); d = -d
    if d > 0.9995: r = tuple(x + (y - x) * t for x, y in zip(a, b))
    else:
        th = math.acos(min(1, d)); s = math.sin(th); r = tuple((math.sin((1 - t) * th) * x + math.sin(t * th) * y) / s for x, y in zip(a, b))
    n = math.sqrt(sum(x * x for x in r)); return tuple(x / n for x in r)

def skeleton(path):
    d = open(path, 'rb').read(); C = chunks(d, 32, len(d)); a = C['NODE'][0] + 20; n = rd32(d, a); a += 4; out = []
    for _ in range(n):
        par = struct.unpack_from('<i', d, a + 12)[0]; q = struct.unpack_from('<4f', d, a + 16); t = struct.unpack_from('<3f', d, a + 32)
        l = rd32(d, a + 68); out.append((d[a + 72:a + 72 + l].decode('latin1'), par, q, t)); a += 72 + l + 1
    return out
def arm_skeleton(nodes):
    return [n for n, *_ in nodes[1:] if not (n.endswith('End') or n.endswith('Knee') or n.endswith('4') or 'Twist' in n or n.endswith('Elbow'))]

def _bits(b): return [i for i in range(len(b) * 8) if b[i >> 3] >> (i & 7) & 1]
def _rot_group(B, g):
    A, F = struct.unpack_from('<HH', B, g); nb = (F >> 3) + 1
    offs = [struct.unpack_from('<I', B, g + 8 + 4 * i)[0] for i in range(nb + 1)]
    keys = [dict() for _ in range(A)]
    for b in range(nb):
        p = g + offs[b]; masks = B[p + 6 * A:p + 7 * A]; x = p + 6 * A + ((A + 1) & ~1)
        for t in range(A):
            keys[t][8 * b] = q48(B, p + 6 * t)
            for j in range(1, 8):
                if masks[t] >> (7 - j) & 1: keys[t][8 * b + j] = q48(B, x); x += 6
    out = []
    for t in range(A):
        ks = sorted(k for k in keys[t] if k <= F); fr = []
        for f in range(F + 1):
            if f in keys[t]: fr.append(keys[t][f]); continue
            a = max(k for k in ks if k < f); nxt = [k for k in ks if k > f]
            fr.append(keys[t][a] if not nxt else slerp(keys[t][a], keys[t][nxt[0]], (f - a) / (nxt[0] - a)))
        out.append(fr)
    return F, out
def clip(B, h=0):
    """the clip whose 0xC0-byte header starts at h: dict(length, frames, rot={bone: [q..]}, pos={bone: [v..]})"""
    a = h + 0xc0
    if B[a:a + 3] != b'AnD': raise ValueError('not a Blood Dragon clip')
    length = struct.unpack_from('<f', B, a + 4)[0]; f = struct.unpack_from('<10I', B, a + 8); base = h + 0x10
    sets = [_bits(B[h + o:h + o + 32]) for o in (0x14, 0x34, 0x54, 0x74)]
    rot, pos, frames = {}, {}, 0
    for i, bone in enumerate(sets[0]): rot[bone] = [q48(B, base + f[2] + 8 + 6 * i)]
    if sets[1]:
        frames, tr = _rot_group(B, base + f[3])
        for bone, fr in zip(sets[1], tr): rot[bone] = fr
    for i, bone in enumerate(sets[2]): pos[bone] = [tuple(v / 16384 for v in struct.unpack_from('<3h', B, base + f[4] + 8 + 6 * i))]
    if sets[3]:
        g = base + f[5]; A, F = struct.unpack_from('<HH', B, g); frames = max(frames, F)
        for i, bone in enumerate(sets[3]):
            pos[bone] = [tuple(v / 16384 for v in struct.unpack_from('<3h', B, g + 8 + 6 * (k * A + i))) for k in range(F + 1)]
    return dict(length=length, frames=frames, rot=rot, pos=pos)
def nested(B):
    """[(package, bone, clip)] in file order"""
    out = []
    for m in re.finditer(b'AnD', B):
        h = m.start() - 0xc0
        if h > 0: out.append(clip(B, h))
    names = re.findall(rb'(bd_[a-z0-9_]+|pkm[a-z_]*)\x00', B[:])
    return out

def key(track, k): return track[min(k, len(track) - 1)]

def run(bd_path, rex_path, gun_path, pkm_path, out_path, gun_map, spin=None, lid_rest=None):
    B = open(bd_path, 'rb').read()
    arms = arm_skeleton(skeleton(rex_path))
    main = clip(B, 0)
    N = max(main['frames'], 1) + 1
    times = [main['length'] * k / (N - 1) for k in range(N)] if N > 1 else [0.0]
    chans = {}                                                   # node -> {'rotation': [...], 'translation': [...]}
    for bone, fr in main['rot'].items():
        if bone < len(arms): chans.setdefault(arms[bone], {})['rotation'] = [key(fr, round(k * (len(fr) - 1) / max(1, N - 1))) for k in range(N)]
    for bone, fr in main['pos'].items():
        if bone < len(arms): chans.setdefault(arms[bone], {})['translation'] = [key(fr, round(k * (len(fr) - 1) / max(1, N - 1))) for k in range(N)]
    extras = {}; parent_of = {}
    gun = None
    for c in nested(B):
        if len(c['rot']) > 3: gun = c; break
    rep = '%d frames (%.2f s), %d arm bones' % (N - 1, main['length'], len(chans))
    if gun is not None and gun_map:
        G = skeleton(gun_path); P = skeleton(pkm_path)
        gname = [n for n, *_ in G]; pname = [n for n, *_ in P]
        Pw = {}                                                  # PKM bind poses in the gun's space
        for n, par, q, t in P:
            L = (q, t)
            if n == 'PKM_AmmoLid' and lid_rest: L = lid_rest
            Pw[n] = L if par < 0 else xm(Pw[pname[par]], L)
        Gb = {n: (q, t) for n, par, q, t in G}                   # Blood Dragon parts' bind poses (all under the root)
        gN = max(1, gun['frames']) + 1
        def gk(k): return round(k * (gN - 1) / max(1, N - 1))
        def bd_anim(bone, k):
            i = gname.index(bone); q, t = Gb[bone]
            if i in gun['rot']: q = key(gun['rot'][i], gk(k))
            if i in gun['pos']: t = key(gun['pos'][i], gk(k))
            return (q, t)
        Rk = [bd_anim(gname[0], k) for k in range(N)]            # the gun's root in the hand
        chans['PKM'] = {'rotation': [r[0] for r in Rk], 'translation': [r[1] for r in Rk]}
        parent_of['PKM'] = 'RightHand'
        world = {}
        for bd_bone, node in gun_map.items():
            world[node] = [xm(xm(bd_anim(bd_bone, k), xi(Gb[bd_bone])), Pw[node]) for k in range(N)]
        if spin and 'PKM_Tripod2' in world:
            ang = spin(times)
            ax = spin.axis
            def about(deg, p):
                a = math.radians(deg) / 2; q = (0.0, math.sin(a), 0.0, math.cos(a)); return (q, tuple(p[i] - qr(q, p)[i] for i in range(3)))
            world['PKM_Tripod2'] = [xm(about(ang[k], ax), w) for k, w in enumerate(world['PKM_Tripod2'])]
            rep += ', barrels turn %.0f deg' % ang[-1]
        for node, ws in world.items():
            par = P[pname.index(node)][1]; pn = pname[par] if par >= 0 else None
            if pn is None or pn == 'PKM':                         # hangs off the hand in the clip
                tr = [xm(Rk[k], ws[k]) for k in range(N)]; parent_of[node] = 'RightHand'
                extras[node] = {'fc4gun': 1, 'fc4gun_root': 'PKM', 'fc4_new_track': 1}
            else:
                pw = world.get(pn) or [Pw[pn]] * N
                tr = [xm(xi(pw[k]), ws[k]) for k in range(N)]; parent_of[node] = pn
                extras[node] = {'fc4gun': 1, 'fc4_new_track': 1}
            chans[node] = {'rotation': [x[0] for x in tr], 'translation': [x[1] for x in tr]}
        rep += ', gun: %d parts' % len(world)
    write(out_path, times, chans, parent_of, extras)
    return rep

def write(path, times, chans, parent_of, extras):
    names = list(chans)
    J = {'asset': {'version': '2.0', 'generator': 'bd2glb'}, 'scene': 0, 'scenes': [{'nodes': []}], 'nodes': [],
         'animations': [{'name': 'blood_dragon', 'channels': [], 'samplers': []}], 'accessors': [], 'bufferViews': [], 'buffers': []}
    if 'RightHand' not in names: names.append('RightHand')
    for n in names:
        nd = {'name': n}
        if n in extras: nd['extras'] = extras[n]
        J['nodes'].append(nd)
    idx = {n: i for i, n in enumerate(names)}
    for n, p in parent_of.items():
        J['nodes'][idx[p]].setdefault('children', []).append(idx[n])
    kids = {c for n in J['nodes'] for c in n.get('children', [])}
    J['scenes'][0]['nodes'] = [i for i in range(len(names)) if i not in kids]
    bin_ = bytearray()
    def acc(rows, typ):
        while len(bin_) % 4: bin_.append(0)
        off = len(bin_); n = {'SCALAR': 1, 'VEC3': 3, 'VEC4': 4}[typ]
        for r in rows: bin_.extend(struct.pack('<%df' % n, *((r,) if n == 1 else r)))
        J['bufferViews'].append({'buffer': 0, 'byteOffset': off, 'byteLength': len(bin_) - off})
        a = {'bufferView': len(J['bufferViews']) - 1, 'componentType': 5126, 'count': len(rows), 'type': typ}
        if typ == 'SCALAR': a['min'] = [min(rows)]; a['max'] = [max(rows)]
        J['accessors'].append(a); return len(J['accessors']) - 1
    ti = acc(times, 'SCALAR'); an = J['animations'][0]
    for n, ch in chans.items():
        for kind, rows in ch.items():
            an['samplers'].append({'input': ti, 'output': acc(rows, 'VEC4' if kind == 'rotation' else 'VEC3'), 'interpolation': 'LINEAR'})
            an['channels'].append({'sampler': len(an['samplers']) - 1, 'target': {'node': idx[n], 'path': kind}})
    J['buffers'] = [{'byteLength': len(bin_)}]
    js = json.dumps(J, separators=(',', ':')).encode()
    while len(js) % 4: js += b' '
    open(path, 'wb').write(struct.pack('<III', 0x46546C67, 2, 28 + len(js) + len(bin_)) + struct.pack('<II', len(js), 0x4E4F534A) + js
                           + struct.pack('<II', len(bin_), 0x004E4942) + bytes(bin_))

# PKM nodes the PKM's own clips never move (except the box and its lid), so clips left as they are keep these still
OVERKILL = {'BARREL_01': 'PKM_Tripod2', 'FLAP_01': 'PKM_AmmoLid', 'CHAIN': 'PKM_AmmoBox', 'RING_01': 'PKM_Slide', 'RING_02': 'PKM_Trigger',
            'COVER_01': 'PKM_Top', 'SPIN_01': 'FRONTSIGHT_FLIP'}
LID_REST = ((0.0, 0.2470, 0.0, 0.9690), (0.0938, 0.0003, -0.0002))   # the Overkill model's PKM_AmmoLid bind (in the box)

if __name__ == '__main__':
    a = sys.argv[1:]
    spin = None
    if '--spin' in a:
        from overkill_clips import spin_angles, BARREL_AXIS
        class Spin:
            axis = BARREL_AXIS
            def __call__(s, times): return spin_angles(times, times[-1])
        spin = Spin()
    print(run(a[0], a[1], a[2], a[3], a[4], OVERKILL, spin, LID_REST))
