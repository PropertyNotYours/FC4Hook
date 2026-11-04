# Retarget a CoD viewmodel .seanim (+ .semodel bind poses) onto an FC4Hook-exported clip .glb.
# Output: a .glb with the same skeleton and the FC4_<id> animation name, ready for Use my .glb.
import json, struct, sys, numpy as np
sys.path.insert(0, __file__.rsplit('/', 1)[0] if '/' in __file__ else '.')
import seanim, semodel

def qnorm(q): q = np.asarray(q, float); return q / np.linalg.norm(q)
def q2m(q):
    x, y, z, w = qnorm(q)
    return np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                     [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                     [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
def m2q(m):
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1) * 2; return qnorm([(m[2,1]-m[1,2])/s, (m[0,2]-m[2,0])/s, (m[1,0]-m[0,1])/s, s/4])
    i = int(np.argmax([m[0,0], m[1,1], m[2,2]])); j, k = (i+1) % 3, (i+2) % 3
    s = np.sqrt(1 + m[i,i] - m[j,j] - m[k,k]) * 2
    q = [0, 0, 0, 0]; q[i] = s/4; q[j] = (m[j,i]+m[i,j])/s; q[k] = (m[k,i]+m[i,k])/s; q[3] = (m[k,j]-m[j,k])/s
    return qnorm(q)
def slerp(a, b, f):
    a = qnorm(a); b = qnorm(b); d = np.dot(a, b)
    if d < 0: b = -b; d = -d
    if d > 0.9995: return qnorm(a + f*(b-a))
    th = np.arccos(d); return (np.sin((1-f)*th)*a + np.sin(f*th)*b) / np.sin(th)
def T(R=None, t=None): M = np.eye(4); M[:3,:3] = np.eye(3) if R is None else R; M[:3,3] = 0 if t is None else t; return M
def inv(M): R = M[:3,:3]; t = M[:3,3]; return T(R.T, -R.T @ t)

# ---------------------------------------------------------------- CoD side
def cod_worlds(anim_path, arms_path, gun_path):
    A = seanim.read(anim_path)
    bones = semodel.read(arms_path) + []
    na = len(bones)
    for b in semodel.read(gun_path):                       # gun hangs from tag_weapon
        b = dict(b); b['parent'] = (b['parent'] + na) if b['parent'] >= 0 else [x['name'] for x in bones].index('tag_weapon')
        bones.append(b)
    idx = {b['name']: i for i, b in enumerate(bones)}
    def sample(keys, f, slerpq):
        if not keys: return None
        if f <= keys[0][0]: return np.array(keys[0][1])
        for (f0, v0), (f1, v1) in zip(keys, keys[1:]):
            if f0 <= f <= f1:
                u = 0 if f1 == f0 else (f - f0) / (f1 - f0)
                return slerp(v0, v1, u) if slerpq else (1-u)*np.array(v0) + u*np.array(v1)
        return np.array(keys[-1][1])
    frames = A['frames']
    relpos = set()
    for b in bones:
        k = A['tracks'].get(b['name'], {}).get('pos', [])
        if k and np.linalg.norm(k[0][1]) < 0.01 and np.linalg.norm(b['lpos']) > 1.0: relpos.add(b['name'])
    if relpos: print('position offsets from bind:', sorted(relpos))
    out = []
    for f in range(frames + 1):
        W = [None]*len(bones)
        for i, b in enumerate(bones):
            tr = A['tracks'].get(b['name'], {})
            p = sample(tr.get('pos', []), f, False); q = sample(tr.get('rot', []), f, True)
            # some tracks (the Ray Gun's batteries) store positions as offsets from the bind pose: their first
            # key is ~0 while the bone sits well away from its parent. Read those as bind + offset.
            if p is not None and b['name'] in relpos: p = np.array(b['lpos']) + p
            L = T(q2m(q if q is not None else b['lrot']), p if p is not None else np.array(b['lpos']))
            W[i] = L if b['parent'] < 0 else W[b['parent']] @ L
        out.append({b['name']: W[i] for i, b in enumerate(bones)})
    return out, A

# ---------------------------------------------------------------- FC4 side (the exported glb)
def load_glb(path):
    d = open(path, 'rb').read(); jl = struct.unpack_from('<I', d, 12)[0]
    j = json.loads(d[20:20+jl]); bl = struct.unpack_from('<I', d, 20+jl)[0]; b = d[28+jl:28+jl+bl]
    return j, b
def acc_data(j, b, i):
    a = j['accessors'][i]; v = j['bufferViews'][a['bufferView']]; n = {'SCALAR': 1, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
    return np.frombuffer(b, '<f4', a['count']*n, v.get('byteOffset', 0) + a.get('byteOffset', 0)).reshape(a['count'], n)
def fc4_pose0(j, b):
    N = j['nodes']; par = {}
    for i, n in enumerate(N):
        for c in n.get('children', []): par[c] = i
    loc = [[np.array(n.get('rotation', [0, 0, 0, 1]), float), np.array(n.get('translation', [0, 0, 0]), float)] for n in N]
    an = j['animations'][0]
    for c in an['channels']:
        s = an['samplers'][c['sampler']]; v = acc_data(j, b, s['output'])[0]
        if c['target']['path'] == 'rotation': loc[c['target']['node']][0] = np.array(v, float)
        elif c['target']['path'] == 'translation': loc[c['target']['node']][1] = np.array(v, float)
    W = [None]*len(N)
    def w(i):
        if W[i] is None:
            L = T(q2m(loc[i][0]), loc[i][1]); W[i] = L if i not in par else w(par[i]) @ L
        return W[i]
    for i in range(len(N)): w(i)
    return N, par, loc, W

ARM = {'j_shoulder_le': 'LeftArm', 'j_elbow_le': 'LeftForeArm', 'j_wrist_le': 'LeftHand',
       'j_shoulder_ri': 'RightArm', 'j_elbow_ri': 'RightForeArm', 'j_wrist_ri': 'RightHand'}
FING = {'index': 'Index', 'mid': 'Middle', 'ring': 'Ring', 'pinky': 'Pinky'}
for side, S in (('le', 'Left'), ('ri', 'Right')):
    for c, f in FING.items():
        for k in (0, 1, 2): ARM['j_%s_%s_%d' % (c, side, k)] = '%sHand%s%d' % (S, f, k + 1)   # CoD counts from 0
    for k in (0, 1, 2): ARM['j_thumb_%s_%d' % (side, k)] = '%sHandThumb%d' % (S, k + 1)
GUNPARTS = {'tag_battery_1': 'CLIP', 'tag_battery_2': 'CLIP_BULLET', 'j_clip_release': 'SLIDE_LOCK'}

def similarity(P, Q):                        # Q ~ s R P + t
    pm, qm = P.mean(0), Q.mean(0); A = P - pm; B = Q - qm
    U, S, Vt = np.linalg.svd(B.T @ A); D = np.diag([1, 1, np.sign(np.linalg.det(U @ Vt))])
    R = U @ D @ Vt; s = np.trace(np.diag(S) @ D) / (A**2).sum(); t = qm - s * R @ pm
    return s, R, t


def swing(a, b):
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b); c = np.cross(a, b); d = float(np.dot(a, b))
    if d > 0.999999: return np.eye(3)
    if d < -0.999999:
        ax = np.cross(a, [1, 0, 0]);
        if np.linalg.norm(ax) < 1e-6: ax = np.cross(a, [0, 1, 0])
        ax /= np.linalg.norm(ax); return 2 * np.outer(ax, ax) - np.eye(3)
    K = np.array([[0, -c[2], c[1]], [c[2], 0, -c[0]], [-c[1], c[0], 0]])
    return np.eye(3) + K + K @ K * (1 / (1 + d))
def fc4_rest(j):
    N = j['nodes']; par = {}
    for i, n in enumerate(N):
        for c in n.get('children', []): par[c] = i
    W = {}
    def w(i):
        if i not in W:
            L = T(q2m(N[i].get('rotation', [0, 0, 0, 1])), np.array(N[i].get('translation', [0, 0, 0]), float))
            W[i] = L if i not in par else w(par[i]) @ L
        return W[i]
    return {N[i]['name']: w(i) for i in range(len(N))}

def run(anim, arms, gun, glb_in, glb_out, idle=None, gun_root='gun_88dcc141'):
    C, A = cod_worlds(anim, arms, gun)
    I, _ = cod_worlds(idle, arms, gun) if idle else (C, A)      # reference: CoD holding the gun normally
    for k in [k for k in ARM if k not in C[0]]: del ARM[k]
    j, b = load_glb(glb_in)
    N, par, loc, W0 = fc4_pose0(j, b)                             # reference: FC4 holding the gun (clip's first frame)
    ni = {n['name']: i for i, n in enumerate(N)}
    # axes: CoD x forward, y left, z up  ->  glTF: forward -z, left -x, up +y
    R = np.array([[0., -1., 0.], [0., 0., 1.], [-1., 0., 0.]])
    lens = []
    for sd, S in (('le', 'Left'), ('ri', 'Right')):
        for a_, b_, fa, fb in (('j_shoulder_', 'j_elbow_', 'Arm', 'ForeArm'), ('j_elbow_', 'j_wrist_', 'ForeArm', 'Hand')):
            lc = np.linalg.norm(I[0][b_ + sd][:3, 3] - I[0][a_ + sd][:3, 3]); lf = np.linalg.norm(W0[ni[S + fb]][:3, 3] - W0[ni[S + fa]][:3, 3])
            lens.append(lf / lc)
    s = float(np.mean(lens))
    print('scale %.5f (from arm lengths)' % s)
    def mapR(M): return R @ M[:3, :3]
    # per bone: the fixed turn that makes CoD's hold pose equal FC4's hold pose
    off = {k: mapR(I[0][k]).T @ W0[ni[v]][:3, :3] for k, v in ARM.items()}
    # the left arm: CoD's Ray Gun is held one-handed, so its hold pose says nothing about the left arm.
    # Left hand + fingers: offsets from the two rigs' bind/rest poses; arm: 2-bone IK to CoD's left
    # wrist position relative to the right hand, bent towards CoD's elbow.
    Rest = fc4_rest(j)
    Bind = {b['name']: T(q2m(b['grot']), np.array(b['gpos'])) for b in semodel.read(arms)}
    for k, v in ARM.items():
        if k.endswith(('_le', '_le_0', '_le_1', '_le_2', '_le_3')) and not k.startswith(('j_shoulder', 'j_elbow')) and k in Bind and v in Rest:
            off[k] = mapR(Bind[k]).T @ Rest[v][:3, :3]
    lenU = np.linalg.norm(W0[ni['LeftForeArm']][:3, 3] - W0[ni['LeftArm']][:3, 3])
    lenF = np.linalg.norm(W0[ni['LeftHand']][:3, 3] - W0[ni['LeftForeArm']][:3, 3])
    refU = W0[ni['LeftForeArm']][:3, 3] - W0[ni['LeftArm']][:3, 3]
    refF = W0[ni['LeftHand']][:3, 3] - W0[ni['LeftForeArm']][:3, 3]
    Orh = off['j_wrist_ri']
    F = A['frames']; nk = F + 1
    depth = {}
    def dep(i):
        if i not in depth: depth[i] = 0 if i not in par else dep(par[i]) + 1
        return depth[i]
    order = sorted(range(len(N)), key=dep)
    # the left arm reaches for a spot next to the right hand: do it after everything else
    def under(i, top):
        while i is not None:
            if i == top: return True
            i = par.get(i)
        return False
    la = ni['LeftArm']
    order = [i for i in order if not under(i, la)] + [i for i in order if under(i, la)]
    gi = ni[gun_root]
    rot = {i: [] for i in range(len(N))}; pos = {i: [] for i in range(len(N))}
    byFc4 = {v: k for k, v in ARM.items()}
    # the gun relative to the right wrist, and the parts relative to the gun, are taken from CoD (as changes
    # from its hold pose) and put on FC4's own hold pose
    def rel(a_, b_, Wd): return inv(Wd[a_]) @ Wd[b_]
    Igw = rel('j_wrist_ri', 'tag_weapon', I[0])
    Fgw = inv(W0[ni['RightHand']]) @ W0[gi]
    def mapRigid(E):                     # a CoD rigid change (in some CoD frame) -> same frame mapped to glTF axes, FC4 scale
        return T(R @ E[:3, :3] @ R.T, s * (R @ E[:3, 3]))
    def smooth(x): x = min(max(x, 0.0), 1.0); return x * x * (3 - 2 * x)
    EIN, EOUT = 10, 18                   # frames to ease the left hand off/onto FC4's grip
    for f in range(nk):
        Wt = {}; ik = None
        wL = smooth(f / EIN) * smooth((nk - 1 - f) / EOUT)
        grip = lambda i: Wt[ni['RightHand']] @ inv(W0[ni['RightHand']]) @ W0[i]
        for i in order:
            p = par.get(i); Wp = Wt[p] if p is not None else np.eye(4)
            L = T(q2m(loc[i][0]), loc[i][1])
            nm = N[i]['name']
            if nm in ('LeftArm', 'LeftForeArm') and 'j_wrist_le' in C[f]:
                if nm == 'LeftArm':
                    S = W0[i][:3, 3] + s * R @ (C[f]['j_shoulder_le'][:3, 3] - I[0]['j_shoulder_le'][:3, 3])
                    # target: CoD left wrist in CoD right-wrist space -> FC4 right-hand space
                    lw = inv(C[f]['j_wrist_ri']) @ C[f]['j_wrist_le']
                    Pt = (Wt[ni['RightHand']] @ T(None, s * Orh.T @ lw[:3, 3]))[:3, 3] if ni['RightHand'] in Wt else None
                    if Pt is None:
                        Pt = W0[ni['LeftHand']][:3, 3]
                    Pt = (1 - wL) * grip(ni['LeftHand'])[:3, 3] + wL * Pt
                    S = (1 - wL) * W0[i][:3, 3] + wL * S
                    Ep = S + s * R @ (C[f]['j_elbow_le'][:3, 3] - C[f]['j_shoulder_le'][:3, 3])
                    Ep = (1 - wL) * W0[ni['LeftForeArm']][:3, 3] + wL * Ep
                    dvec = Pt - S; d = min(np.linalg.norm(dvec), (lenU + lenF) * 0.999); u = dvec / np.linalg.norm(dvec)
                    a_ = (lenU**2 - lenF**2 + d**2) / (2 * d); h = np.sqrt(max(lenU**2 - a_**2, 0))
                    pv = (Ep - S) - np.dot(Ep - S, u) * u
                    pv = pv / np.linalg.norm(pv) if np.linalg.norm(pv) > 1e-9 else np.array([0., -1., 0.])
                    El = S + a_ * u + h * pv; Wr = S + d * u
                    ik = {'U': (S, El), 'F': (El, Wr)}
                    Wt[i] = T(swing(refU, El - S) @ W0[i][:3, :3], S)
                else:
                    El, Wr = ik['F']
                    Wt[i] = T(swing(refF, Wr - El) @ W0[i][:3, :3], El)
                continue
            if nm in byFc4:
                k = byFc4[nm]
                Rw = mapR(C[f][k]) @ off[k]
                if wL < 1 and nm != 'LeftArm' and under(i, la) and not k.startswith(('j_shoulder', 'j_elbow')):
                    Rg = grip(i)[:3, :3] if nm == 'LeftHand' else (Wp @ inv(W0[par[i]]) @ W0[i])[:3, :3]
                    Rw = q2m(slerp(m2q(Rg), m2q(Rw), wL))
                pw = (Wp @ L)[:3, 3]
                if nm == 'LeftHand': pw = ik['F'][1]
                if k.startswith('j_shoulder'):
                    pw = W0[i][:3, 3] + s * R @ (C[f][k][:3, 3] - I[0][k][:3, 3])
                Wt[i] = T(Rw, pw)
            else:
                Wt[i] = Wp @ L
        # gun in the right hand: FC4's hold offset, plus CoD's change of gun-vs-wrist since its hold
        Cgw = rel('j_wrist_ri', 'tag_weapon', C[f])
        Dg = Cgw @ inv(Igw)                                    # in CoD wrist frame
        # express the change in FC4's hand frame: CoD wrist axes -> FC4 hand axes via the bone offset
        O = off['j_wrist_ri']; Om = T(O.T, None)              # FC4 hand frame = CoD wrist frame * O  -> vectors: v_fc4 = O^T v_cod
        Dg_f = T(O.T @ Dg[:3, :3] @ O, s * O.T @ Dg[:3, 3])
        Ga = Wt[ni['RightHand']] @ Dg_f @ Fgw
        Wt[gi] = Ga
        # parts: CoD part-vs-gun change since hold, in gun frame -> FC4 gun frame
        Og = (R @ I[0]['tag_weapon'][:3, :3]).T @ W0[gi][:3, :3]
        for k, nm in GUNPARTS.items():
            if nm not in ni or k not in C[f]: continue
            E = rel('tag_weapon', k, C[f]) @ inv(rel('tag_weapon', k, I[0]))
            Ef = T(Og.T @ E[:3, :3] @ Og, s * Og.T @ E[:3, 3])
            Wt[ni[nm]] = Ga @ Ef @ inv(W0[gi]) @ W0[ni[nm]]
        # the gun's other parts ride with the gun
        for nm in ('FRAME', 'SLIDE', 'HAMMER'):
            if nm in ni: Wt[ni[nm]] = Ga @ inv(W0[gi]) @ W0[ni[nm]]
        for i in order:
            p = par.get(i); L = (inv(Wt[p]) if p is not None else np.eye(4)) @ Wt[i]
            q = m2q(L[:3, :3])
            if rot[i] and np.dot(q, rot[i][-1]) < 0: q = -q
            rot[i].append(q); pos[i].append(L[:3, 3])
    # write: same nodes/skin/IBMs, a new animation on every joint, Blender frame k = game frame k
    joints = j['skins'][0]['joints']
    bin_ = bytearray(b)
    def add(arr, typ):
        arr = np.asarray(arr, '<f4'); off = len(bin_); bin_.extend(arr.tobytes())
        while len(bin_) % 4: bin_.append(0)
        j['bufferViews'].append({'buffer': 0, 'byteOffset': off, 'byteLength': arr.nbytes})
        a = {'bufferView': len(j['bufferViews']) - 1, 'componentType': 5126, 'count': arr.shape[0], 'type': typ}
        if typ == 'SCALAR': a['min'] = [float(arr.min())]; a['max'] = [float(arr.max())]
        j['accessors'].append(a); return len(j['accessors']) - 1
    ta = add(np.arange(nk, dtype=float).reshape(-1, 1) / 24.0, 'SCALAR')
    samp, chan = [], []
    for i in joints:
        for path, data, typ in (('rotation', rot[i], 'VEC4'), ('translation', pos[i], 'VEC3')):
            samp.append({'input': ta, 'output': add(np.array(data), typ), 'interpolation': 'LINEAR'})
            chan.append({'sampler': len(samp) - 1, 'target': {'node': i, 'path': path}})
    name = j['animations'][0]['name']
    j['animations'] = [{'name': name, 'samplers': samp, 'channels': chan}]
    j['buffers'][0]['byteLength'] = len(bin_)
    js = json.dumps(j, separators=(',', ':')).encode()
    while len(js) % 4: js += b' '
    out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_)) + struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(bin_), 0x004E4942) + bytes(bin_)
    open(glb_out, 'wb').write(out)
    print('wrote', glb_out, nk, 'keys;', 'notes:', A['notes'])

if __name__ == '__main__':
    run(*sys.argv[1:7])
