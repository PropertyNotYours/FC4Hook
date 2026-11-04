"""Blood Dragon Overkill on Far Cry 4's PKM animations: spinning barrels, the feed flap and the ammo chain.

  python overkill_clips.py <clip.glb> <clip.json> <out.glb>

<clip.glb>   a PKM clip exported by FC4Hook (Workshop > Animations > Export .glb), with the gun's parts
<clip.json>  the clip's nested gun animations (clipdump.exe <clip file> > clip.json): the second copy holds the
             gun's root bone (PKM) relative to the hand, which says where the gun is in every frame

The Overkill model (fc3xbg2glb.py --map BARREL_01=PKM_Tripod2,FLAP_01=PKM_AmmoLid,CHAIN=PKM_AmmoBox
--point PKM_AmmoLid=<rest>) rides three PKM nodes:
  PKM_Tripod2  the barrels. The PKM's clips never move it: fire clips get a new track that turns it about the
               barrels' axis (one barrel, 60 degrees, per shot, then a spin-down).
  PKM_AmmoLid  the feed flap. The PKM's lid swings about its own hinge under the gun; the flap gets the same
               opening angle about its hinge (mirrored: it opens the other way). Its bind pose is the lid's
               rest pose in the clips (28.6 degrees), so clips that don't move the lid leave the flap shut.
  PKM_AmmoBox  the chain. Moves with the box (out and back in on reload), turning about the chain itself.
Positions are the gun's own space (x right, y forward, z up, metres).
"""
import json, math, struct, sys

BARREL_AXIS = (0.0, 0.6949, -0.00966)      # BD BARREL_01: 6 barrels about y
BARRELS = 6
FLAP_HINGE = (0.052, 0.530, 0.061)          # BD FLAP_01
CHAIN_PIVOT = (0.160, 0.521, 0.072)         # BD CHAIN
TRIPOD2_BIND = (0.0, 0.528, 0.028)          # PKM node bind (local to the root, no rotation)
BOX_BIND = (0.0, 0.216, 0.046)
LID_REST = ((0.0, 0.2470, 0.0, 0.9690), (0.0938, 0.0003, -0.0002))   # in the box
ROOT = '881d5c41'                          # crc32("PKM")
H = dict(box='4dec748d', lid='0529a24e')    # crc32 PKM_AmmoBox / PKM_AmmoLid

def qmul(a, b):
    return (a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1], a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
            a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3], a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2])
def qinv(q): return (-q[0], -q[1], -q[2], q[3])
def qrot(q, v): return qmul(qmul(q, (v[0], v[1], v[2], 0)), qinv(q))[:3]
def add(a, b): return tuple(x + y for x, y in zip(a, b))
def sub(a, b): return tuple(x - y for x, y in zip(a, b))
def xmul(A, B): return (qmul(A[0], B[0]), add(A[1], qrot(A[0], B[1])))      # (q, t) transforms
def xinv(A): iq = qinv(A[0]); return (iq, qrot(iq, tuple(-x for x in A[1])))
def roty(deg): a = math.radians(deg) / 2; return (0.0, math.sin(a), 0.0, math.cos(a))
def about(q, p): return (q, sub(p, qrot(q, p)))                                # rotate by q about point p
def angle_y(q):                                                                # signed angle about +y
    if q[3] < 0: q = tuple(-x for x in q)
    return math.degrees(2 * math.atan2(q[1], q[3]))

class Glb:
    def __init__(s, path):
        d = open(path, 'rb').read(); n = struct.unpack_from('<I', d, 12)[0]
        s.j = json.loads(d[20:20 + n]); o = 20 + n + ((4 - n % 4) % 4); bl = struct.unpack_from('<I', d, o)[0]
        s.bin = bytearray(d[o + 8:o + 8 + bl])
    def acc(s, i):
        a = s.j['accessors'][i]; v = s.j['bufferViews'][a['bufferView']]; n = {'SCALAR': 1, 'VEC3': 3, 'VEC4': 4}[a['type']]
        off = v.get('byteOffset', 0) + a.get('byteOffset', 0)
        return [struct.unpack_from('<%df' % n, s.bin, off + 4 * n * k) for k in range(a['count'])]
    def put(s, i, rows):                                       # overwrite an accessor (same count)
        a = s.j['accessors'][i]; v = s.j['bufferViews'][a['bufferView']]; n = len(rows[0])
        off = v.get('byteOffset', 0) + a.get('byteOffset', 0)
        for k, r in enumerate(rows): struct.pack_into('<%df' % n, s.bin, off + 4 * n * k, *r)
        if 'min' in a: a['min'] = [min(r[c] for r in rows) for c in range(n)]; a['max'] = [max(r[c] for r in rows) for c in range(n)]
    def new(s, rows, typ):
        while len(s.bin) % 4: s.bin.append(0)
        off = len(s.bin); n = len(rows[0]) if isinstance(rows[0], tuple) else 1
        for r in rows: s.bin += struct.pack('<%df' % n, *(r if n > 1 else (r,)))
        s.j['bufferViews'].append({'buffer': 0, 'byteOffset': off, 'byteLength': len(s.bin) - off})
        a = {'bufferView': len(s.j['bufferViews']) - 1, 'componentType': 5126, 'count': len(rows), 'type': typ}
        if typ == 'SCALAR': a['min'] = [min(rows)]; a['max'] = [max(rows)]
        s.j['accessors'].append(a); return len(s.j['accessors']) - 1
    def channel(s, node, path):
        an = s.j['animations'][0]; ni = next(i for i, n in enumerate(s.j['nodes']) if n.get('name') == node)
        for c in an['channels']:
            if c['target']['node'] == ni and c['target']['path'] == path: return an['samplers'][c['sampler']]
        return None
    def save(s, path):
        s.j['buffers'] = [{'byteLength': len(s.bin)}]
        js = json.dumps(s.j, separators=(',', ':')).encode()
        while len(js) % 4: js += b' '
        while len(s.bin) % 4: s.bin.append(0)
        open(path, 'wb').write(struct.pack('<III', 0x46546C67, 2, 28 + len(js) + len(s.bin)) + struct.pack('<II', len(js), 0x4E4F534A) + js
                               + struct.pack('<II', len(s.bin), 0x004E4942) + bytes(s.bin))

def spin_angles(times, length):
    """degrees per key: one barrel over the first 0.1 s (the game restarts the clip on every shot), then a
    spin-down that stops on a whole barrel at the clip's end"""
    step = 360.0 / BARRELS; t1 = min(0.1, length / 2); w = step / t1; td = max(1e-3, length - t1)
    extra = max(step, round(w * td / 2 / step) * step)
    out = []
    for t in times:
        if t <= t1: out.append(w * t)
        else: u = min(1.0, (t - t1) / td); out.append(step + extra * (1 - (1 - u) ** 2))
    return out

def run(glb_path, dump_path, out_path, spin=None):
    g = Glb(glb_path); E = json.load(open(dump_path))
    full = next((e for e in E if ROOT in e['tracks']), None)
    if full is None: raise SystemExit('the clip has no copy with the gun root (PKM) track')
    def key(tr, k, kind):
        a = tr[kind]; return tuple(a[min(k, len(a) - 1)]) if a else None
    rootT = full['tracks'][ROOT]
    sb_r, sb_t = g.channel('PKM_AmmoBox', 'rotation'), g.channel('PKM_AmmoBox', 'translation')
    sl_r, sl_t = g.channel('PKM_AmmoLid', 'rotation'), g.channel('PKM_AmmoLid', 'translation')
    times = [t[0] for t in g.acc(sb_r['input'])]
    n = len(times); length = times[-1]
    Gk = [(key(rootT, k, 'r'), key(rootT, k, 'p')) for k in range(n)]                 # gun root in the hand
    B = list(zip([tuple(x) for x in g.acc(sb_r['output'])], [tuple(x) for x in g.acc(sb_t['output'])]))
    L = list(zip([tuple(x) for x in g.acc(sl_r['output'])], [tuple(x) for x in g.acc(sl_t['output'])]))
    Wbox = ((0, 0, 0, 1), BOX_BIND); Wlid = xmul(Wbox, LID_REST)
    nb, nl, moved, opened = [], [], 0.0, 0.0
    for k in range(n):
        A_box = xmul(xinv(Gk[k]), B[k])                                              # box in the gun
        D = xmul(A_box, xinv(Wbox))                                                  # how the box moved
        R = D[0]; delta = sub(A_box[1], Wbox[1])
        A_box2 = (R, add(add(qrot(R, sub(Wbox[1], CHAIN_PIVOT)), CHAIN_PIVOT), delta))   # same move, turning about the chain
        ang = angle_y(qmul(L[k][0], qinv(LID_REST[0])))                              # the lid's opening
        ang = max(0.0, ang)                                                          # (the flap can't close further)
        A_flap = xmul(about(roty(-ang), FLAP_HINGE), Wlid)                           # mirrored: it opens the other way
        nb.append(xmul(Gk[k], A_box2)); nl.append(xmul(xinv(A_box2), A_flap))
        moved = max(moved, math.dist(A_box[1], Wbox[1])); opened = max(opened, ang)
    g.put(sb_r['output'], [x[0] for x in nb]); g.put(sb_t['output'], [x[1] for x in nb])
    g.put(sl_r['output'], [x[0] for x in nl]); g.put(sl_t['output'], [x[1] for x in nl])
    rep = 'chain moves up to %.0f cm, flap opens up to %.0f deg' % (moved * 100, opened)
    if spin:
        ang = spin_angles(times, length); Wt = ((0, 0, 0, 1), TRIPOD2_BIND); rows_r, rows_t = [], []
        for k in range(n):
            X = xmul(Gk[k], xmul(about(roty(ang[k]), BARREL_AXIS), Wt)); rows_r.append(X[0]); rows_t.append(X[1])
        hand = next(i for i, nd in enumerate(g.j['nodes']) if g.j['nodes'][i].get('name') == 'RightHand')
        g.j['nodes'].append({'name': 'PKM_Tripod2', 'rotation': list(rows_r[0]), 'translation': list(rows_t[0]),
                             'extras': {'fc4gun': 1, 'fc4_new_track': 1, 'fc4gun_root': 'PKM'}})
        ni = len(g.j['nodes']) - 1; g.j['nodes'][hand].setdefault('children', []).append(ni)
        an = g.j['animations'][0]; ti = g.new(times, 'SCALAR')
        for path, rows, typ in (('rotation', rows_r, 'VEC4'), ('translation', rows_t, 'VEC3')):
            an['samplers'].append({'input': ti, 'output': g.new(rows, typ), 'interpolation': 'LINEAR'})
            an['channels'].append({'sampler': len(an['samplers']) - 1, 'target': {'node': ni, 'path': path}})
        rep += ', barrels turn %.0f deg' % ang[-1]
    g.save(out_path)
    return rep

if __name__ == '__main__':
    a = sys.argv[1:]
    print(run(a[0], a[1], a[2], spin='--spin' in a))
