"""Round off an exported SH1 character model (ilm_obj.py export) part by part.

    python smooth_model.py IN.obj OUT.obj [--levels 1] [--keep 0.5] [--skip NAME,...]

Every rigid part ('o' block) gets Catmull-Clark subdivision on its own, so
part names, materials and the one-bone-per-part rig stay exactly as they
were; ilm_obj.py import --v7 then builds a high-poly ILM from the result.

  * Positions: Catmull-Clark. The open ends of a part (the joints, where the
    neighbouring part continues) use the boundary rule, so both parts subdivide
    the shared ring identically and stay joined. --keep pulls the original
    corners back toward where they were (0 = pure Catmull-Clark, which shrinks
    a low-poly limb noticeably; 1 = corners pinned).
  * UVs: per face corner, interpolated inside each face (texture atlas islands
    never bleed into each other).
  * Normals: recomputed smooth per vertex from the new surface; a vertex shared
    across a hard UV/material seam still shares its normal, so the shading is
    round.
  * --soft PATTERN=AMOUNT,...: parts whose name contains PATTERN get only
    AMOUNT of the Catmull-Clark rounding (0 = the original shape, just finer
    and smooth shaded; 1 = full). Heads keep their features this way, e.g.
    --soft HEAD=0.15,HAIR=0.3. AMOUNT may also be one value per level,
    separated by ':' - HEAD=1:0 rounds the head like a single subdivision
    and only splits the result once more without moving anything. The open joint rings always take the full
    boundary rule, so a soft part still meets its neighbour exactly.
  * --skip: parts copied unchanged (geometry, UVs and normals), for hard-edged
    things a subdivision would melt, e.g. Cybil's gun hands and holster
    (06LGUN,10RGUN,11HOLDER).
"""
import argparse
import math
import sys
from collections import defaultdict


def parse(path):
    head, parts = [], []
    V, VT, VN = [], [], []
    cur = None
    mtl = None
    for line in open(path, encoding='utf-8'):
        s = line.split()
        if not s:
            continue
        k = s[0]
        if k == 'v':
            V.append(tuple(map(float, s[1:4])))
            cur['v'].append(len(V))
        elif k == 'vt':
            VT.append(tuple(map(float, s[1:3])))
        elif k == 'vn':
            VN.append(tuple(map(float, s[1:4])))
        elif k == 'o':
            cur = dict(name=s[1], v=[], faces=[])
            parts.append(cur)
            mtl = None
        elif k == 'usemtl':
            mtl = s[1]
        elif k == 'f':
            cs = []
            for c in s[1:]:
                a = c.split('/')
                cs.append((int(a[0]), int(a[1]) if len(a) > 1 and a[1] else 0,
                           int(a[2]) if len(a) > 2 and a[2] else 0))
            cur['faces'].append(dict(mtl=mtl, c=cs))
        elif cur is None:
            head.append(line.rstrip('\n'))
    return head, parts, V, VT, VN


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def norm(a):
    l = math.sqrt(dot(a, a))
    return (a[0] / l, a[1] / l, a[2] / l) if l > 1e-12 else (0.0, 0.0, 0.0)


def avg(pts):
    s = (0.0, 0.0, 0.0)
    for p in pts:
        s = add(s, p)
    return mul(s, 1.0 / len(pts))


def subdivide(P, faces, keep, amount=1.0):
    """P: list of positions (welded ids); faces: list of dict(v=[ids], uv=[(u,v)], mtl, nref)
    returns new P, new faces (quads)."""
    nP = len(P)
    fpts = [avg([P[i] for i in f['v']]) for f in faces]
    edge_faces = defaultdict(list)
    for fi, f in enumerate(faces):
        n = len(f['v'])
        for i in range(n):
            a, b = f['v'][i], f['v'][(i + 1) % n]
            if a != b:
                edge_faces[(min(a, b), max(a, b))].append(fi)
    epts = {}
    for e, fl in edge_faces.items():
        a, b = e
        if len(fl) == 2:
            cc = mul(add(add(P[a], P[b]), add(fpts[fl[0]], fpts[fl[1]])), 0.25)
            lin = mul(add(P[a], P[b]), 0.5)
            epts[e] = add(mul(cc, amount), mul(lin, 1.0 - amount))
        else:
            epts[e] = mul(add(P[a], P[b]), 0.5)
    vfaces = defaultdict(list)
    vedges = defaultdict(list)
    for fi, f in enumerate(faces):
        for v in set(f['v']):
            vfaces[v].append(fi)
    for e in edge_faces:
        vedges[e[0]].append(e)
        vedges[e[1]].append(e)
    newP = list(P)
    for v in range(nP):
        es = vedges.get(v, [])
        if not es:
            continue
        bnd = [e for e in es if len(edge_faces[e]) != 2]
        if bnd:
            if len(bnd) == 2:
                nb = [e[0] if e[1] == v else e[1] for e in bnd]
                q = add(mul(P[v], 0.75), mul(add(P[nb[0]], P[nb[1]]), 0.125))
            else:
                q = P[v]
        else:
            n = len(es)
            Q = avg([fpts[fi] for fi in vfaces[v]])
            R = avg([mul(add(P[e[0]], P[e[1]]), 0.5) for e in es])
            q = mul(add(add(Q, mul(R, 2.0)), mul(P[v], n - 3.0)), 1.0 / n)
            q = add(mul(q, amount), mul(P[v], 1.0 - amount))
        newP[v] = add(mul(q, 1.0 - keep), mul(P[v], keep))
    eid = {}
    for e in edge_faces:
        eid[e] = len(newP)
        newP.append(epts[e])
    fid = []
    for fp in fpts:
        fid.append(len(newP))
        newP.append(fp)
    out = []
    for fi, f in enumerate(faces):
        vs, uvs = f['v'], f['uv']
        n = len(vs)
        fuv = (sum(u[0] for u in uvs) / n, sum(u[1] for u in uvs) / n)
        for i in range(n):
            a, b, c = vs[(i - 1) % n], vs[i], vs[(i + 1) % n]
            ua, ub, uc = uvs[(i - 1) % n], uvs[i], uvs[(i + 1) % n]
            e_prev = eid[(min(a, b), max(a, b))]
            e_next = eid[(min(b, c), max(b, c))]
            out.append(dict(
                v=[b, e_next, fid[fi], e_prev],
                uv=[ub, ((ub[0] + uc[0]) / 2, (ub[1] + uc[1]) / 2), fuv, ((ua[0] + ub[0]) / 2, (ua[1] + ub[1]) / 2)],
                mtl=f['mtl'], nref=f['nref']))
    return newP, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp')
    ap.add_argument('out')
    ap.add_argument('--levels', type=int, default=1)
    ap.add_argument('--keep', type=float, default=0.5)
    ap.add_argument('--skip', default='', help='comma-separated part names left unchanged')
    ap.add_argument('--soft', default='', help='PATTERN=AMOUNT,...: parts whose name contains PATTERN '
                    'get only AMOUNT (0..1) of the rounding')
    a = ap.parse_args()

    head, parts, V, VT, VN = parse(a.inp)
    out = list(head)
    gv = gvt = gvn = 0
    body = []
    total_v = total_f = 0
    skip = set(s for s in a.skip.split(',') if s)
    soft = []
    for s in a.soft.split(','):
        if s:
            k, v = s.split('=')
            soft.append((k, [float(x) for x in v.split(':')]))
    for part in parts:
        if part['name'] in skip:
            # verbatim: own positions, UVs and the original normals
            loc = {vl: i for i, vl in enumerate(part['v'])}
            body.append('o %s' % part['name'])
            for vl in part['v']:
                body.append('v %.6f %.6f %.6f' % V[vl - 1])
            nmap, nl = {}, []
            uvl, fl = [], []
            cur_m = None
            for f in part['faces']:
                if f['mtl'] != cur_m:
                    fl.append('usemtl %s' % f['mtl'])
                    cur_m = f['mtl']
                cs = []
                for vi, ti, ni in f['c']:
                    uvl.append(VT[ti - 1] if ti else (0.0, 0.0))
                    if ni not in nmap:
                        nmap[ni] = len(nl)
                        nl.append(VN[ni - 1] if ni else (0.0, 1.0, 0.0))
                    cs.append('%d/%d/%d' % (gv + loc[vi] + 1, gvt + len(uvl), gvn + nmap[ni] + 1))
                fl.append('f ' + ' '.join(cs))
            for n in nl:
                body.append('vn %.6f %.6f %.6f' % n)
            for uv in uvl:
                body.append('vt %.6f %.6f' % uv)
            body += fl
            gv += len(part['v'])
            gvn += len(nl)
            gvt += len(uvl)
            total_v += len(part['v'])
            total_f += len(part['faces'])
            continue
        # weld identical positions inside the part (seam duplicates of one point)
        ids, P = {}, []
        remap = {}
        for vl in part['v']:
            key = tuple(round(x, 4) for x in V[vl - 1])
            if key not in ids:
                ids[key] = len(P)
                P.append(V[vl - 1])
            remap[vl] = ids[key]
        faces = []
        for f in part['faces']:
            vs = [remap[c[0]] for c in f['c']]
            uvs = [VT[c[1] - 1] if c[1] else (0.0, 0.0) for c in f['c']]
            nref = avg([VN[c[2] - 1] for c in f['c'] if c[2]]) if any(c[2] for c in f['c']) else None
            faces.append(dict(v=vs, uv=uvs, mtl=f['mtl'], nref=nref))
        amounts = None
        for k, v in soft:
            if k in part['name'] and amounts is None:
                amounts = v
        for lv in range(a.levels):
            amount = 1.0 if amounts is None else amounts[min(lv, len(amounts) - 1)]
            P, faces = subdivide(P, faces, a.keep, amount)
        # orientation: match each face to the original normals' side
        vn_acc = [(0.0, 0.0, 0.0)] * len(P)
        fnorm = []
        for f in faces:
            p = [P[i] for i in f['v']]
            fn = cross(sub(p[2], p[0]), sub(p[3] if len(p) > 3 else p[2], p[1]))
            if len(p) == 4:
                fn = cross(sub(p[2], p[0]), sub(p[3], p[1]))
            if f['nref'] is not None and dot(fn, f['nref']) < 0:
                fn = mul(fn, -1.0)
            fnorm.append(fn)
            for i in f['v']:
                vn_acc[i] = add(vn_acc[i], fn)
        vns = [norm(n) for n in vn_acc]
        body.append('o %s' % part['name'])
        for p in P:
            body.append('v %.6f %.6f %.6f' % p)
        used = sorted(set(i for f in faces for i in f['v']))
        for i in range(len(P)):
            n = vns[i] if dot(vns[i], vns[i]) > 0.5 else (0.0, 1.0, 0.0)
            body.append('vn %.6f %.6f %.6f' % n)
        cur_m = None
        uvlines = []
        flines = []
        for f in faces:
            if f['mtl'] != cur_m:
                flines.append(('m', f['mtl']))
                cur_m = f['mtl']
            corners = []
            for i, uv in zip(f['v'], f['uv']):
                uvlines.append(uv)
                corners.append('%d/%d/%d' % (gv + i + 1, gvt + len(uvlines), gvn + i + 1))
            flines.append(('f', ' '.join(corners)))
        for uv in uvlines:
            body.append('vt %.6f %.6f' % uv)
        for k, s in flines:
            body.append(('usemtl %s' % s) if k == 'm' else ('f ' + s))
        gv += len(P)
        gvn += len(P)
        gvt += len(uvlines)
        total_v += len(P)
        total_f += len(faces)
    out += body
    open(a.out, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
    print('smoothed %d parts: %d vertices, %d quads' % (len(parts), total_v, total_f))


if __name__ == '__main__':
    main()
