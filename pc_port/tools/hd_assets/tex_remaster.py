"""Gentle remaster of every paletted texture on the disc -> loose overrides.

    python tex_remaster.py DISC_DIR OUT_LOAD_DIR [--scale 4] [--jobs N]
                           [--only GLOB] [--index clut_index.json]

DISC_DIR is the extracted disc tree (BG/, CHARA/, ITEM/, TIM/, 1ST/, TEST/);
OUT_LOAD_DIR is gamedata/load (files land in OUT_LOAD_DIR/<FOLDER>/).

Nothing is invented - no AI upscaler, no new detail. Per texture:

  1. Smooth the palette steps. The textures carry 16 (4bpp) or 256 (8bpp)
     colours per region, which shows as banding on faces, skin and cloth at
     high resolution. An edge-preserving (bilateral) filter merges neighbouring
     steps into gradients and leaves real edges (eyes, seams, strands) alone.
  2. Enlarge (default 4x) with a cubic filter and sharpen gently.
  3. Lay a fine grain over it so it never looks like plastic.

Regions are kept apart. A 4bpp sheet is drawn through several CLUT rows; the
same index data decodes to garbage under the wrong palette, so each row is
treated on its own, restricted to the texels the game draws through it
(clut_tool's corpus-wide row map over every .ILM/.PLM/.IPD), and written as a
per-row override "<NAME>.TIM.pNN.png" (outside its texels it is transparent,
which no prim of that row samples). Sheets with one palette - and sheets no
model references (2D art, drawn by code) - are treated whole.

Repeating tiles (streets, floors, walls: one rectangle of the sheet mapped
whole by many quads) are processed on their own and padded with their own
opposite edge where the retail art tiles seamlessly, so they repeat without
seams; their neighbours in the sheet never bleed in.

Transparency (colour 0x0000) stays exactly where it was; its edge is traced on
the enlarged grid so cut-outs get smooth instead of stair-stepped borders, and
the colour under transparent texels is bled from the neighbours so filtering
never pulls in a dark fringe.

Left alone: fonts (1ST/FONT*), the paper maps and their markings
(TIM/MP_*, TIM/MR_*; they have their own HD sources) and the dev sheets in TEST/.
"""
import argparse
import fnmatch
import json
import os
import sys
import time
import zlib
from multiprocessing import Pool

import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import clut_tool  # noqa: E402

EXCLUDE = ['1ST/FONT*', 'TIM/MP_*', 'TIM/MR_*', 'TEST/*']
MAX_ROWS = clut_tool.HIRES_POOL_MAX_ROWS

SIG_S = 1.0       # bilateral: spatial sigma (native texels)
SIG_R = 10.0      # bilateral: colour sigma (0..255) - about one 5-bit step pair
SHARPEN = 0.6
GRAIN = 2.2


# ----------------------------------------------------------------- filters

def bilateral(img, valid, rad=2):
    H, W, _ = img.shape
    acc = np.zeros_like(img)
    wsum = np.zeros((H, W, 1))
    pad = np.pad(img, ((rad, rad), (rad, rad), (0, 0)), mode='edge')
    vp = np.pad(valid.astype(float), rad)
    for dy in range(-rad, rad + 1):
        for dx in range(-rad, rad + 1):
            sh = pad[rad + dy:rad + dy + H, rad + dx:rad + dx + W]
            w = np.exp(-(dx * dx + dy * dy) / (2 * SIG_S ** 2)) * \
                np.exp(-((sh - img) ** 2).sum(-1, keepdims=True) / (2 * SIG_R ** 2)) * \
                vp[rad + dy:rad + dy + H, rad + dx:rad + dx + W][..., None]
            acc += w * sh
            wsum += w
    out = acc / np.maximum(wsum, 1e-9)
    return np.where(valid[..., None], out, img)


def bleed(img, valid, n):
    """Extend the colours of `valid` texels outward n steps (UV edge padding)."""
    img = img.copy()
    valid = valid.copy()
    k = np.ones((3, 3))
    for _ in range(n):
        if valid.all():
            break
        cnt = ndimage.convolve(valid.astype(float), k, mode='constant')
        new = (~valid) & (cnt > 0)
        if not new.any():
            break
        for c in range(3):
            s = ndimage.convolve(img[..., c] * valid, k, mode='constant')
            img[..., c][new] = s[new] / cnt[new]
        valid = valid | new
    return img, valid


def upscale(sm, S):
    up = np.stack([ndimage.zoom(sm[..., c], S, order=3, mode='nearest', grid_mode=True)
                   for c in range(3)], -1)
    blur = np.stack([ndimage.gaussian_filter(up[..., c], S * 0.6) for c in range(3)], -1)
    return up + SHARPEN * (up - blur)


def grain(shape, seed, wrap=False):
    rng = np.random.default_rng(seed)
    return ndimage.gaussian_filter(rng.normal(0, GRAIN, shape), 0.6,
                                   mode='wrap' if wrap else 'reflect') * 1.6


def enhance(rgb, opaque, solid_region, out_region, S, seed):
    """rgb: HxWx3 (0..255) native decode; opaque: texel not transparent;
    solid_region: texels drawn through this palette (their colours feed the
    filter); out_region: texels written (solid_region plus a crack margin).
    Returns (origin, hi rgb, hi alpha, hi region)."""
    H, W = out_region.shape
    ys, xs = np.nonzero(out_region)
    if len(ys) == 0:
        return None
    m = 4  # context margin (native texels)
    y0, y1 = max(ys.min() - m, 0), min(ys.max() + m + 1, H)
    x0, x1 = max(xs.min() - m, 0), min(xs.max() + m + 1, W)
    rgb, opaque = rgb[y0:y1, x0:x1], opaque[y0:y1, x0:x1]
    solid = solid_region[y0:y1, x0:x1] & opaque
    src, _ = bleed(rgb, solid, m)
    sm = bilateral(src, solid)
    sm, _ = bleed(sm, solid, m)
    up = np.clip(upscale(sm, S) + grain(((y1 - y0) * S, (x1 - x0) * S), seed)[..., None], 0, 255)
    # alpha: binary as on the PSX, traced on the fine grid (smooth cut-out edges)
    a = ndimage.zoom(opaque.astype(float), S, order=1, mode='nearest', grid_mode=True) > 0.5
    reg = np.kron(out_region[y0:y1, x0:x1], np.ones((S, S), bool))
    return (y0, x0), up, a & reg, reg


# ----------------------------------------------------------------- repeating tiles
#
# Streets, pavements, walls and floors are one small rectangle of the sheet
# repeated over thousands of quads. Filtered like the rest, each tile's border
# took colour from whatever lies next to it in the sheet and was then clamped,
# so every repeat showed a seam: a grid of lines across the street. A rectangle
# that some quad maps whole is cut out and processed on its own, padded with
# its own opposite edge (wrap) on every axis where it tiles seamlessly in the
# retail art, so the remaster repeats as seamlessly as the original.

def tile_rects(prims, row, W, H):
    rects = {}
    for p in prims:
        if p['row'] != row or p['tri']:
            continue
        uv = p['uvs']
        us = sorted({u for u, _v in uv})
        vs = sorted({v for _u, v in uv})
        if len(us) != 2 or len(vs) != 2:
            continue
        if set(uv) != {(us[0], vs[0]), (us[1], vs[0]), (us[0], vs[1]), (us[1], vs[1])}:
            continue

        def span(a, b, lim):
            n = b - a
            if (n + 1) % 8 == 0:  # inclusive form: 0..31 is a 32-texel tile
                n += 1
            return a, min(a + n, lim)

        x0, x1 = span(us[0], us[1], W)
        y0, y1 = span(vs[0], vs[1], H)
        if x1 - x0 >= 8 and y1 - y0 >= 8:
            rects[(x0, y0, x1, y1)] = rects.get((x0, y0, x1, y1), 0) + 1
    return rects


def seamless(t):
    """(wraps vertically, wraps horizontally) in the retail art: the jump across
    the tile's own edge is no bigger than an ordinary step inside it."""
    t = t.astype(float)
    ix = np.abs(np.diff(t, axis=1)).mean()
    iy = np.abs(np.diff(t, axis=0)).mean()
    ex = np.abs(t[:, 0] - t[:, -1]).mean()
    ey = np.abs(t[0] - t[-1]).mean()
    return ey <= 1.5 * iy + 2.0, ex <= 1.5 * ix + 2.0


def pad2(a, m, wy, wx):
    rest = [(0, 0)] * (a.ndim - 2)
    a = np.pad(a, [(m, m), (0, 0)] + rest, mode='wrap' if wy else 'edge')
    return np.pad(a, [(0, 0), (m, m)] + rest, mode='wrap' if wx else 'edge')


def enhance_tile(t, S, seed, wy, wx):
    m = 4
    h, w = t.shape[:2]
    tp = pad2(t, m, wy, wx)
    sm = bilateral(tp, np.ones(tp.shape[:2], bool))
    up = upscale(sm, S)[m * S:(m + h) * S, m * S:(m + w) * S]
    return np.clip(up + grain((h * S, w * S), seed, wrap=True)[..., None], 0, 255)


# ----------------------------------------------------------------- per sheet

def decode(tim):
    idx = np.array(tim['idx'], np.int32)
    pals = np.array(tim['palettes'], np.int32)
    return idx, pals


def row_rgb(idx, pal):
    raw = pal[np.clip(idx, 0, len(pal) - 1)]
    r5, g5, b5 = raw & 31, (raw >> 5) & 31, (raw >> 10) & 31
    rgb = np.stack([(r5 << 3) | (r5 >> 2), (g5 << 3) | (g5 >> 2), (b5 << 3) | (b5 >> 2)], -1).astype(float)
    return rgb, raw != 0


def row_masks(prims, W, H, rows):
    """-> {row: (drawn texels, drawn texels + 1-texel crack margin)}"""
    out = {}
    for r in rows:
        plane = bytearray(W * H)
        clut_tool._rasterise(plane, [p for p in prims if p['row'] == r], W, H, value=1)
        m = np.frombuffer(bytes(plane), np.uint8).reshape(H, W) > 0
        out[r] = (m, ndimage.binary_dilation(m, iterations=1))
    return out


def render_row(rgb, opq, solid, region, prims, row, S, seed):
    """Full-sheet hi-res canvas (rgb, alpha) of one palette row."""
    H, W = region.shape
    hi = np.zeros((H * S, W * S, 3))
    a = np.zeros((H * S, W * S), bool)
    res = enhance(rgb, opq, solid, region, S, seed)
    if res is not None:
        (oy, ox), up, al, reg = res
        h, w = up.shape[:2]
        hi[oy * S:oy * S + h, ox * S:ox * S + w][reg] = up[reg]
        a[oy * S:oy * S + h, ox * S:ox * S + w] |= al
    tiles = 0
    rects = tile_rects(prims, row, W, H)
    for (x0, y0, x1, y1), _n in sorted(rects.items(), key=lambda kv: kv[1]):
        if not opq[y0:y1, x0:x1].all():
            continue  # cut-outs (fences, foliage) do not repeat edge to edge
        wy, wx = seamless(rgb[y0:y1, x0:x1])
        if not (wy or wx):
            continue
        hi[y0 * S:y1 * S, x0 * S:x1 * S] = enhance_tile(rgb[y0:y1, x0:x1], S, seed + x0 * 7 + y0 * 131, wy, wx)
        a[y0 * S:y1 * S, x0 * S:x1 * S] = True
        tiles += 1
    return hi, a, tiles


def save(path, hi, a):
    canvas = np.dstack([np.clip(hi, 0, 255), np.where(a, 255, 0)]).astype(np.uint8)
    # colour under alpha 0 stays (bled) so bilinear filtering has no dark fringe
    Image.fromarray(canvas, 'RGBA').save(path, compress_level=6)


def process(job):
    rel, prims, disc, out_load, S = job
    t0 = time.time()
    try:
        tim = clut_tool.parse_tim_indexed(open(os.path.join(disc, rel), 'rb').read())
    except ValueError as e:
        return rel, 'skip (%s)' % e, 0, 0.0
    W, H = tim['w'], tim['h']
    idx, pals = decode(tim)
    folder = os.path.dirname(rel)
    name = os.path.basename(rel)
    od = os.path.join(out_load, folder)
    os.makedirs(od, exist_ok=True)
    seed = zlib.crc32(rel.encode())
    nrows = len(pals)
    written = 0
    tiles = 0
    full = np.ones((H, W), bool)
    if nrows == 1:
        rgb, opq = row_rgb(idx, pals[0])
        hi, a, tiles = render_row(rgb, opq, full, full, prims, 0, S, seed)
        save(os.path.join(od, name + '.png'), hi, a)
        return rel, 'whole, %d tile(s)' % tiles, 1, time.time() - t0
    rows = sorted({p['row'] for p in prims if 0 <= p['row'] < min(nrows, MAX_ROWS)})
    masks = row_masks(prims, W, H, rows) if rows else {}
    if not rows:
        # drawn by code (2D art, HUD): every palette over the whole sheet
        rows = list(range(min(nrows, MAX_ROWS)))
        masks = {r: (full, full) for r in rows}
    if 0 not in rows:
        rows = [0] + rows  # p00 is the runtime's per-row sentinel; keep it native-equivalent
        masks[0] = (np.zeros((H, W), bool),) * 2
    for r in rows:
        rgb, opq = row_rgb(idx, pals[r])
        p = os.path.join(od, '%s.p%02d.png' % (name, r))
        solid, region = masks[r]
        if not region.any():
            # sentinel row nobody draws: the plain decode, enlarged
            save(p, np.kron(rgb, np.ones((S, S, 1))), np.kron(opq, np.ones((S, S), bool)))
        else:
            hi, a, n = render_row(rgb, opq, solid, region, prims, r, S, seed + r)
            tiles += n
            save(p, hi, a)
        written += 1
    return rel, 'rows %s, %d tile(s)' % (','.join(map(str, rows)), tiles), written, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('disc')
    ap.add_argument('out_load')
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument('--only', default='*')
    ap.add_argument('--index', default=None)
    a = ap.parse_args()

    ip = a.index or os.path.join(a.out_load, '..', 'clut_index.json')
    if os.path.exists(ip):
        idx = clut_tool.load_index(ip, a.disc)
    else:
        idx = clut_tool.build_index(a.disc)
        json.dump(idx, open(ip, 'w'))
    rels = []
    for rel in sorted(idx['tims']):
        if any(fnmatch.fnmatch(rel.upper(), e) for e in EXCLUDE):
            continue
        if not fnmatch.fnmatch(rel.upper(), a.only.upper()):
            continue
        rels.append(rel)
    print('collecting prims for %d sheets ...' % len(rels))
    prims = clut_tool.collect_prims(idx, rels)
    jobs = [(rel, prims[rel], a.disc, a.out_load, a.scale) for rel in rels]
    t0 = time.time()
    total = 0
    with Pool(a.jobs) as pool:
        for i, (rel, what, n, dt) in enumerate(pool.imap_unordered(process, jobs, chunksize=1)):
            total += n
            print('[%d/%d] %s: %s (%d file(s), %.1fs)' % (i + 1, len(jobs), rel, what, n, dt), flush=True)
    print('done: %d file(s) in %.0fs' % (total, time.time() - t0))


if __name__ == '__main__':
    main()
