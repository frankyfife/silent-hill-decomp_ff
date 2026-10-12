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


def enhance(rgb, opaque, region, S, seed):
    """rgb: HxWx3 (0..255) native decode; opaque: texel not transparent;
    region: texels this image may use. Returns (hi rgb, hi alpha 0/255, hi region)."""
    H, W = region.shape
    ys, xs = np.nonzero(region)
    if len(ys) == 0:
        return None
    m = 4  # context margin (native texels)
    y0, y1 = max(ys.min() - m, 0), min(ys.max() + m + 1, H)
    x0, x1 = max(xs.min() - m, 0), min(xs.max() + m + 1, W)
    rgb, opaque, region = rgb[y0:y1, x0:x1], opaque[y0:y1, x0:x1], region[y0:y1, x0:x1]
    solid = region & opaque
    src, _ = bleed(rgb, solid, m)
    sm = bilateral(src, solid)
    sm, _ = bleed(sm, solid, m)
    up = np.stack([ndimage.zoom(sm[..., c], S, order=3, mode='nearest', grid_mode=True)
                   for c in range(3)], -1)
    blur = np.stack([ndimage.gaussian_filter(up[..., c], S * 0.6) for c in range(3)], -1)
    up = up + SHARPEN * (up - blur)
    rng = np.random.default_rng(seed)
    g = ndimage.gaussian_filter(rng.normal(0, GRAIN, up.shape[:2]), 0.6) * 1.6
    up = np.clip(up + g[..., None], 0, 255)
    # alpha: binary as on the PSX, traced on the fine grid (smooth cut-out edges)
    a = ndimage.zoom(opaque.astype(float), S, order=1, mode='nearest', grid_mode=True) > 0.5
    reg = np.kron(ndimage.binary_dilation(region, iterations=1), np.ones((S, S), bool))
    return (y0, x0), up, a & reg, reg


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
    out = {}
    for r in rows:
        plane = bytearray(W * H)
        clut_tool._rasterise(plane, [p for p in prims if p['row'] == r], W, H, value=1)
        m = np.frombuffer(bytes(plane), np.uint8).reshape(H, W) > 0
        out[r] = ndimage.binary_dilation(m, iterations=1)  # hairline cracks between prims
    return out


def save(path, hi_rgb, hi_a, origin, S, W, H):
    canvas = np.zeros((H * S, W * S, 4), np.uint8)
    oy, ox = origin[0] * S, origin[1] * S
    h, w = hi_rgb.shape[:2]
    canvas[oy:oy + h, ox:ox + w, :3] = hi_rgb.astype(np.uint8)
    canvas[oy:oy + h, ox:ox + w, 3] = np.where(hi_a, 255, 0)
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
    full = np.ones((H, W), bool)
    if nrows == 1:
        rgb, opq = row_rgb(idx, pals[0])
        res = enhance(rgb, opq, full, S, seed)
        save(os.path.join(od, name + '.png'), res[1], res[2], res[0], S, W, H)
        return rel, 'whole', 1, time.time() - t0
    rows = sorted({p['row'] for p in prims if 0 <= p['row'] < min(nrows, MAX_ROWS)})
    masks = row_masks(prims, W, H, rows) if rows else {}
    if not rows:
        # drawn by code (2D art, HUD): every palette over the whole sheet
        rows = list(range(min(nrows, MAX_ROWS)))
        masks = {r: full for r in rows}
    if 0 not in rows:
        rows = [0] + rows  # p00 is the runtime's per-row sentinel; keep it native-equivalent
        masks[0] = np.zeros((H, W), bool)
    for r in rows:
        rgb, opq = row_rgb(idx, pals[r])
        p = os.path.join(od, '%s.p%02d.png' % (name, r))
        if not masks[r].any():
            # sentinel row nobody draws: the plain decode, enlarged
            a = np.kron(opq, np.ones((S, S), bool))
            save(p, np.kron(rgb, np.ones((S, S, 1))), a, (0, 0), S, W, H)
        else:
            res = enhance(rgb, opq, masks[r], S, seed + r)
            save(p, res[1], res[2], res[0], S, W, H)
        written += 1
    return rel, 'rows %s' % ','.join(map(str, rows)), written, time.time() - t0


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
