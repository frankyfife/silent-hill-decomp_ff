"""HD paper maps -> gamedata/load/TIM/MP_*.TIM.png (4x the retail 320x480).

The fan maps are 1024x763 (the 4:3 screen); the retail TIM is 320x480 drawn
at half height (interlace), so the override is the map stretched to 1280x1920.
The watermark at the bottom right (an orange "SH RESORT" logo and a white URL
under it) is found by colour and replaced by the retail map's pixels at the
same place, upscaled and matched to the surrounding colour.
"""
import os
import sys
import numpy as np
from PIL import Image, ImageFilter
from scipy import ndimage

import tim

SRC = r'D:/Downloads/silent hill'
OUT = sys.argv[1] if len(sys.argv) > 1 else 'maps_out'
os.makedirs(OUT, exist_ok=True)

NAMES = {
    'CENTRALSH': 'MP_BSTLN', 'HOSPITAL1F': 'MP_HSP1F', 'HOSPITAL2F': 'MP_HSP2F', 'HOSPITALB': 'MP_HSPBF',
    'OLDSILENTHILL': 'MP_0TOWN', 'RESORTAREA': 'MP_RSORT', 'SCHOOL1F': 'MP_SCL1F', 'SCHOOL2F': 'MP_SCL2F',
    'SCHOOLB': 'MP_SCLBF', 'SCHOOLR': 'MP_SCLRF', 'SEWERS1': 'MP_DRN1F', 'SEWERS2': 'MP_DRN2F',
    'SEWERS3': 'MP_DRNBK', 'SHOSPITAL3F': 'MP_HSP3F',
}


def watermark_mask(rgb):
    h, w, _ = rgb.shape
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx = rgb.max(-1)
    mn = rgb.min(-1)
    sat = (mx - mn) / np.maximum(mx, 1)
    # the logo's glowing orange/yellow letters (room walls are a dull dark red)
    logo = (sat > 0.6) & (r > 180) & (b < 90) & (g > 40)
    region = np.zeros((h, w), bool)
    region[int(h * 0.72):, int(w * 0.55):] = True
    logo &= region
    lab, n = ndimage.label(ndimage.binary_dilation(logo, iterations=8))
    if n == 0:
        return None
    sizes = ndimage.sum(logo, lab, range(1, n + 1))
    keep = lab == (1 + int(np.argmax(sizes)))
    ys, xs = np.nonzero(keep)
    if ys.size == 0:
        return None
    x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
    lw, lh = x1 - x0, y1 - y0
    m = np.zeros((h, w), bool)
    # logo (with its cross, which rises above the letters) and the URL under it,
    # which starts well left of the logo
    # the dark red cross after the letters is not orange: take everything
    # reddish right of the letters on their rows too
    dark_red = (r > 90) & (r > g * 1.6) & (r > b * 1.6)
    band = np.zeros((h, w), bool)
    band[max(y0 - 16, 0):min(y1 + 4, h), max(x0 - 40, 0):min(x1 + 40, w)] = True
    cols = (dark_red & band).sum(0) >= 2
    xr = np.nonzero(cols)[0]
    if xr.size:
        x0 = min(x0, xr.min())
        x1 = max(x1, xr.max())
    lw = x1 - x0
    m[max(y0 - 16, 0):min(y1 + 3, h), max(x0 - 6, 0):min(x1 + 6, w)] = True
    m[y1:min(y1 + int(lh * 0.6) + 7, h), max(x0 - int(lw * 0.7) - 14, 0):min(x1 + 6, w)] = True
    return m


def period(profile, lo=6, hi=60):
    p = profile - profile.mean()
    ac = np.correlate(p, p, 'full')[len(p) - 1:]
    if hi >= len(ac):
        hi = len(ac) - 1
    k = lo + int(np.argmax(ac[lo:hi]))
    return k


def fill_by_shift(a, m):
    """Fill the masked box with a copy of the same-sized block from above or
    from the left, offset by whole periods of the floor grid so its lines
    continue; the candidate that best matches the border wins."""
    ys, xs = np.nonzero(m)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    bh, bw = y1 - y0, x1 - x0
    lum = a.mean(-1)
    stripy = lum[max(y0 - 160, 0):y0, max(x0 - 40, 0):x0].mean(1) if y0 > 20 else None
    py = period(stripy) if stripy is not None and stripy.size > 20 else 16
    stripx = lum[max(y0 - 30, 0):y0, max(x0 - 240, 0):x1].mean(0)
    px = period(stripx) if stripx.size > 20 else 16
    H_, W_ = a.shape[:2]
    ny = int(np.ceil((bh + 4) / py)) * py
    nx = int(np.ceil((bw + 4) / px)) * px
    dys = [0] + [ny + j * py for j in range(0, 8)]
    dxs = [0] + [s * (nx + j * px) for j in range(0, 10) for s in (1, -1)]
    cands = []
    for dy in dys:
        for dx in dxs:
            if dy == 0 and dx == 0:
                continue
            if y0 - dy < 0 or y1 - dy > H_ or x0 - dx < 0 or x1 - dx > W_:
                continue
            cands.append((dy, dx))
    ring = ndimage.binary_dilation(m, iterations=4) & ~m
    gy, gx = np.gradient(lum)
    edge = np.hypot(gx, gy)
    best, best_e = None, 1e18
    for dy, dx in cands:
        # the copied block: ring continuity plus how busy it is (text, symbols)
        src = np.roll(np.roll(a, dy, axis=0), dx, axis=1)
        busy = np.roll(np.roll(edge, dy, axis=0), dx, axis=1)[m].mean()
        e = np.abs(src[ring] - a[ring]).mean() + 3.0 * busy
        if e < best_e:
            best, best_e = src, e
    return best


for f, name in NAMES.items():
    hd = Image.open(os.path.join(SRC, f + '.png')).convert('RGB')
    W, H = hd.size
    a = np.asarray(hd, float)
    t = tim.read_tim(f'out/TIM/{name}.TIM')
    orig, _ = tim.to_image(t, 0)
    o = np.asarray(orig.convert('RGB').resize((W, H), Image.BICUBIC), float)
    m = watermark_mask(a)
    if m is not None:
        src = fill_by_shift(a, m)
        if src is not None:
            soft = np.maximum(ndimage.gaussian_filter(m.astype(float), 2.0), m)
            a = a * (1 - soft[..., None]) + src * soft[..., None]
    out = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), 'RGB').resize((1280, 1920), Image.LANCZOS)
    out.save(os.path.join(OUT, name + '.TIM.png'))
    # preview of the repaired corner (display aspect)
    Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).crop((W - 330, H - 190, W, H)).save(os.path.join(OUT, 'corner_' + name + '.png'))
    print(f, '->', name, 'watermark', 'found' if m is not None else 'NOT FOUND')
