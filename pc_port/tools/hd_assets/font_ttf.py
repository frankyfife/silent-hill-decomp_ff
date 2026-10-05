"""FONT16.TIM -> sharp 8x replacement set from a real serif font.

Every 12x16 cell of the retail atlas is matched against the candidate
characters of a TrueType font (best overlap with the retail glyph's ink), and
the winner is drawn at 8x into exactly the retail glyph's ink box, so advance
widths, baselines and the grid stay retail. The vertical grey ramp of the body
and the drop shadow (body moved by one native pixel) are rebuilt from the
retail atlas. Cells with no ink stay empty.

usage: python font_ttf.py <font.ttf> [scale] [out.png]
"""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

import tim

FONT = sys.argv[1]
S = int(sys.argv[2]) if len(sys.argv) > 2 else 8
OUT = sys.argv[3] if len(sys.argv) > 3 else 'FONT16.TIM.png'

t = tim.read_tim('out/1ST/FONT16.TIM')
I = np.array(tim.indices(t))
H, W = I.shape
pal = t['clut'][:16]
CW, CH, COLS, ROWS = 12, 16, 21, 6


def rgb(v):
    return np.array([(v & 31), (v >> 5) & 31, (v >> 10) & 31], float) * 255.0 / 31.0


body = (I >= 8)

ramp = np.zeros(16)
for r in range(16):
    rows = I[r::16]
    v = rows[rows >= 8]
    ramp[r] = np.median(v) if v.size else 15
ramp_rgb = np.array([rgb(pal[int(round(k))]) for k in ramp])

MAP = (["’", "(", ")", "=", "+", ",", "-", ".", "/"] + list("0123456789") + [":", ";"]
       + ["<", "—", ">", "?", "★"] + list("ABCDEFGHIJKLMNOP")
       + list("QRSTUVWXYZ") + ["“", "!", "”", "&", None, "’"] + list("abcde")
       + list("fghijklmnopqrstuvwxyz")
       + ["ß", "à", "á", "â", None, "ä", None, None, "ç", "è", "é",
          "ê", "ë", "ì", "í", "î", "ï", None, "ñ", "ò", "ó"]
       + ["ô", None, "ö", "×", None, "ù", "ú", "û", "ü", "¨", "¿",
          "¡", "Ç", "œ", "´", None, None, None, None, None, None])
assert len(MAP) == 126, len(MAP)
CANDS = "".join(c for c in MAP if c)
BIG = 256
font = ImageFont.truetype(FONT, BIG)


def glyph_mask(ch):
    """Ink of one character, cropped to its bounding box (float 0..1)."""
    im = Image.new('L', (BIG * 2, BIG * 2), 0)
    ImageDraw.Draw(im).text((BIG // 2, BIG // 4), ch, font=font, fill=255)
    a = np.array(im, float) / 255.0
    ys, xs = np.nonzero(a > 0.5)
    if ys.size == 0:
        return None
    return a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


sym = ImageFont.truetype("C:/Windows/Fonts/seguisym.ttf", BIG)
masks = {}
for ch in CANDS:
    if ch == "★":
        im = Image.new('L', (BIG * 2, BIG * 2), 0)
        ImageDraw.Draw(im).text((BIG // 2, BIG // 4), ch, font=sym, fill=255)
        a = np.array(im, float) / 255.0
        ys, xs = np.nonzero(a > 0.5)
        masks[ch] = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
        continue
    m = glyph_mask(ch)
    if m is not None:
        masks[ch] = m


def fit(m, w, h):
    return np.array(Image.fromarray((m * 255).astype(np.uint8)).resize((w, h), Image.LANCZOS), float) / 255.0


out_a = np.zeros((H * S, W * S))
chosen = []
for row in range(ROWS):
    for col in range(COLS):
        x0, y0 = col * CW, row * CH
        cell = body[y0:y0 + CH, x0:x0 + CW]
        ys, xs = np.nonzero(cell)
        if ys.size < 2:
            chosen.append(' ')
            continue
        bx0, bx1, by0, by1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
        ref = cell[by0:by1, bx0:bx1].astype(float)
        best = MAP[row * COLS + col]
        if best is None or best not in masks:
            chosen.append('?')
            continue
        chosen.append(best)
        # draw the winner at S x into the retail ink box
        w, h = (bx1 - bx0) * S, (by1 - by0) * S
        g = fit(masks[best], w, h)
        oy, ox = (y0 + by0) * S, (x0 + bx0) * S
        out_a[oy:oy + h, ox:ox + w] = np.maximum(out_a[oy:oy + h, ox:ox + w], g)

A_body = np.clip(out_a, 0, 1)
A_shadow = np.zeros_like(A_body)
A_shadow[S:, S:] = A_body[:-S, :-S]

yy = np.arange(H * S)[:, None] * np.ones((1, W * S))
yn = (yy + 0.5) / S - 0.5
r0 = np.floor(yn).astype(int)
f = (yn - r0)[..., None]
body_rgb = ramp_rgb[np.clip(r0, 0, None) % 16] * (1 - f) + ramp_rgb[np.clip(r0 + 1, 0, None) % 16] * f
shadow_rgb = rgb(pal[1])

a = A_body + A_shadow * (1.0 - A_body)
col = (body_rgb * A_body[..., None] + shadow_rgb * (A_shadow * (1.0 - A_body))[..., None]) / np.maximum(a, 1e-6)[..., None]
img = np.dstack([np.clip(col, 0, 255), np.clip(a * 255.0, 0, 255)]).astype(np.uint8)
Image.fromarray(img, 'RGBA').save(OUT)
for r in range(ROWS):
    print(r, ''.join(chosen[r * COLS:(r + 1) * COLS]))
