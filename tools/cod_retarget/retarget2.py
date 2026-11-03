"""CoD viewmodel animation -> Far Cry 4 first-person clip, gun-driven.

CoD animates the gun on its own (tag_weapon hangs from the torso) and the hands follow it, so this does
the same: the gun follows CoD's path from where FC4 holds it, the right hand stays on the grip, the left
hand goes where CoD's is relative to the gun (easing from/to FC4's two-handed grip at the ends), and both
arms are solved back to fixed shoulders with 2-bone IK.

usage: retarget2.py <anim.seanim> <arms.semodel> <gun.semodel> <fc4clip.glb> <out.glb> [gunmove=0.6]
"""
import sys, json, struct, numpy as np
from retarget import (cod_worlds, load_glb, fc4_pose0, fc4_rest, swing, T, inv, q2m, m2q, slerp,
                      ARM, GUNPARTS)
import semodel

R = np.array([[0., -1., 0.], [0., 0., 1.], [-1., 0., 0.]])     # CoD x fwd, y left, z up -> glTF
def mapR(M): return R @ M[:3, :3]
def smooth(x): x = min(max(x, 0.0), 1.0); return x * x * (3 - 2 * x)

def ik2(S, target, lu, lf, pole):
    d = target - S; dist = np.linalg.norm(d); u = d / max(dist, 1e-9)
    dist = min(dist, (lu + lf) * 0.999); dist = max(dist, abs(lu - lf) * 1.001)
    a = (lu * lu - lf * lf + dist * dist) / (2 * dist); h = np.sqrt(max(lu * lu - a * a, 0))
    pv = (pole - S) - np.dot(pole - S, u) * u
    pv = pv / np.linalg.norm(pv) if np.linalg.norm(pv) > 1e-9 else np.array([0., -1., 0.])
    return S + a * u + h * pv, S + dist * u

def run(anim, arms, gun, glb_in, glb_out, gunmove=0.6, gun_root='gun_88dcc141'):
    C, A = cod_worlds(anim, arms, gun)
    j, b = load_glb(glb_in)
    N, par, loc, W0 = fc4_pose0(j, b)
    ni = {n['name']: i for i, n in enumerate(N)}
    def P(nm): return W0[ni[nm]][:3, 3]
    # scale from arm lengths
    lens = []
    for sd, S_ in (('le', 'Left'), ('ri', 'Right')):
        for a_, b_, fa, fb in (('j_shoulder_', 'j_elbow_', 'Arm', 'ForeArm'), ('j_elbow_', 'j_wrist_', 'ForeArm', 'Hand')):
            lc = np.linalg.norm(C[0][b_ + sd][:3, 3] - C[0][a_ + sd][:3, 3]); lf = np.linalg.norm(P(S_ + fb) - P(S_ + fa))
            lens.append(lf / lc)
    s = float(np.mean(lens)); print('scale %.5f' % s)
    gi = ni[gun_root]; G0 = W0[gi]; Cg0 = C[0]['tag_weapon']
    Og = (R @ Cg0[:3, :3]).T @ G0[:3, :3]                       # CoD gun axes -> FC4 gun axes
    # left hand/finger axis offsets from the two rigs' bind/rest poses
    Rest = fc4_rest(j); Bind = {x['name']: T(q2m(x['grot']), np.array(x['gpos'])) for x in semodel.read(arms)}
    off = {k: mapR(Bind[k]).T @ Rest[v][:3, :3] for k, v in ARM.items() if k in Bind and v in Rest}
    arm = {}
    for side in ('Left', 'Right'):
        U, F, H = ni[side + 'Arm'], ni[side + 'ForeArm'], ni[side + 'Hand']
        arm[side] = dict(U=U, F=F, H=H, lu=np.linalg.norm(P(side + 'ForeArm') - P(side + 'Arm')),
                         lf=np.linalg.norm(P(side + 'Hand') - P(side + 'ForeArm')),
                         refU=P(side + 'ForeArm') - P(side + 'Arm'), refF=P(side + 'Hand') - P(side + 'ForeArm'))
    def under(i, top):
        while i is not None:
            if i == top: return True
            i = par.get(i)
        return False
    lfing = {v: k for k, v in ARM.items() if v.startswith('LeftHand') and v != 'LeftHand'}
    depth = {}
    def dep(i):
        if i not in depth: depth[i] = 0 if i not in par else dep(par[i]) + 1
        return depth[i]
    order = sorted(range(len(N)), key=dep)
    nk = A['frames'] + 1
    EIN, EOUT = 10, 18
    rot = {i: [] for i in range(len(N))}; pos = {i: [] for i in range(len(N))}
    for f in range(nk):
        Cf = C[f]; wL = smooth(f / EIN) * smooth((nk - 1 - f) / EOUT)
        # 1. gun: CoD's turn since its first frame, about the gun; CoD's move, damped
        Rd = R @ Cf['tag_weapon'][:3, :3] @ Cg0[:3, :3].T @ R.T
        G = T(Rd @ G0[:3, :3], G0[:3, 3] + gunmove * s * R @ (Cf['tag_weapon'][:3, 3] - Cg0[:3, 3]))
        gripOf = lambda nm: G @ inv(G0) @ W0[ni[nm]]
        # 2. hand targets
        tgt = {'Right': gripOf('RightHand')}
        gl = gripOf('LeftHand')
        if 'j_wrist_le' in Cf:
            pc = G[:3, 3] + s * R @ (Cf['j_wrist_le'][:3, 3] - Cf['tag_weapon'][:3, 3])
            rc = mapR(Cf['j_wrist_le']) @ off['j_wrist_le']
            p = (1 - wL) * gl[:3, 3] + wL * pc
            r = q2m(slerp(m2q(gl[:3, :3]), m2q(rc), wL))
            tgt['Left'] = T(r, p)
        else:
            tgt['Left'] = gl
        # 3. arms: IK from the fixed shoulders, bending like FC4's hold (left: towards CoD's elbow too)
        Wt = {}
        over = {}
        for side, a in arm.items():
            S_ = W0[a['U']][:3, 3]; poleFC = W0[a['F']][:3, 3]
            pole = poleFC
            if side == 'Left' and 'j_elbow_le' in Cf:
                pc = S_ + s * R @ (Cf['j_elbow_le'][:3, 3] - Cf['j_shoulder_le'][:3, 3])
                pole = (1 - wL) * poleFC + wL * pc
            El, Wr = ik2(S_, tgt[side][:3, 3], a['lu'], a['lf'], pole)
            over[a['U']] = T(swing(a['refU'], El - S_) @ W0[a['U']][:3, :3], S_)
            over[a['F']] = T(swing(a['refF'], Wr - El) @ W0[a['F']][:3, :3], El)
            over[a['H']] = T(tgt[side][:3, :3], Wr)
        for i in order:
            p_ = par.get(i); Wp = Wt[p_] if p_ is not None else np.eye(4)
            nm = N[i]['name']
            if i in over: Wt[i] = over[i]; continue
            Lfc = T(q2m(loc[i][0]), loc[i][1])                     # FC4 hold (clip frame 0) local
            if nm in lfing and wL > 0 and lfing[nm] in Cf:
                k = lfing[nm]; pk = [x for x, v in ARM.items() if v == N[par[i]]['name']]
                pk = pk[0] if pk else 'j_wrist_le'
                if pk in Cf and pk in off and k in off:
                    Lc = (mapR(Cf[pk]) @ off[pk]).T @ (mapR(Cf[k]) @ off[k])
                    Lr = q2m(slerp(m2q(Lfc[:3, :3]), m2q(Lc), wL))
                    Wt[i] = Wp @ T(Lr, Lfc[:3, 3]); continue
            Wt[i] = Wp @ Lfc
        # 4. the gun and its parts (gun node may hang from the hand: overwrite world, locals follow)
        Wt[gi] = G
        for k, nm in GUNPARTS.items():
            if nm not in ni or k not in Cf: continue
            E = inv(Cf['tag_weapon']) @ Cf[k] @ inv(inv(Cg0) @ C[0][k])
            Ef = T(Og.T @ E[:3, :3] @ Og, s * Og.T @ E[:3, 3])
            Wt[ni[nm]] = G @ Ef @ inv(G0) @ W0[ni[nm]]
        for nm in ('FRAME', 'SLIDE', 'HAMMER'):
            if nm in ni: Wt[ni[nm]] = G @ inv(G0) @ W0[ni[nm]]
        for i in order:
            if under(i, gi) and i != gi and N[i]['name'] not in ('FRAME', 'SLIDE', 'HAMMER') and N[i]['name'] not in GUNPARTS.values():
                Wt[i] = Wt[par[i]] @ T(q2m(loc[i][0]), loc[i][1])
        for i in order:
            p_ = par.get(i); L = (inv(Wt[p_]) if p_ is not None else np.eye(4)) @ Wt[i]
            q = m2q(L[:3, :3])
            if rot[i] and np.dot(q, rot[i][-1]) < 0: q = -q
            rot[i].append(q); pos[i].append(L[:3, 3])
    joints = j['skins'][0]['joints']; bin_ = bytearray(b)
    def add(arr, typ):
        arr = np.asarray(arr, '<f4'); o = len(bin_); bin_.extend(arr.tobytes())
        while len(bin_) % 4: bin_.append(0)
        j['bufferViews'].append({'buffer': 0, 'byteOffset': o, 'byteLength': arr.nbytes})
        a = {'bufferView': len(j['bufferViews']) - 1, 'componentType': 5126, 'count': arr.shape[0], 'type': typ}
        if typ == 'SCALAR': a['min'] = [float(arr.min())]; a['max'] = [float(arr.max())]
        j['accessors'].append(a); return len(j['accessors']) - 1
    ta = add(np.arange(nk, dtype=float).reshape(-1, 1) / 24.0, 'SCALAR')
    samp, chan = [], []
    for i in joints:
        for path, data, typ in (('rotation', rot[i], 'VEC4'), ('translation', pos[i], 'VEC3')):
            samp.append({'input': ta, 'output': add(np.array(data), typ), 'interpolation': 'LINEAR'})
            chan.append({'sampler': len(samp) - 1, 'target': {'node': i, 'path': path}})
    j['animations'] = [{'name': j['animations'][0]['name'], 'samplers': samp, 'channels': chan}]
    j['buffers'][0]['byteLength'] = len(bin_)
    js = json.dumps(j, separators=(',', ':')).encode()
    while len(js) % 4: js += b' '
    open(glb_out, 'wb').write(struct.pack('<III', 0x46546C67, 2, 28 + len(js) + len(bin_)) + struct.pack('<II', len(js), 0x4E4F534A)
                              + js + struct.pack('<II', len(bin_), 0x004E4942) + bytes(bin_))
    print('wrote', glb_out, nk, 'keys')

if __name__ == '__main__':
    a = sys.argv[1:]
    run(a[0], a[1], a[2], a[3], a[4], float(a[5]) if len(a) > 5 else 0.6)
