"""Minimal PSX TIM decoder: TIM -> RGBA PIL image (per CLUT row)."""
import struct
from PIL import Image


def read_tim(path):
    b = open(path, 'rb').read()
    magic, flags = struct.unpack_from('<II', b, 0)
    assert magic == 0x10, hex(magic)
    bpp = flags & 3
    has_clut = (flags >> 3) & 1
    off = 8
    clut = None
    clut_info = None
    if has_clut:
        ln, cx, cy, cw, ch = struct.unpack_from('<IHHHH', b, off)
        data = b[off + 12: off + ln]
        clut = [struct.unpack_from('<H', data, i * 2)[0] for i in range(cw * ch)]
        clut_info = (cx, cy, cw, ch)
        off += ln
    ln, ix, iy, iw, ih = struct.unpack_from('<IHHHH', b, off)
    pix = b[off + 12: off + ln]
    return dict(bpp=bpp, clut=clut, clut_info=clut_info, x=ix, y=iy, w16=iw, h=ih, pix=pix)


def c15(v, transparent_zero=True):
    if v == 0 and transparent_zero:
        return (0, 0, 0, 0)
    r = (v & 31) * 255 // 31
    g = ((v >> 5) & 31) * 255 // 31
    bl = ((v >> 10) & 31) * 255 // 31
    return (r, g, bl, 255)


def to_image(t, row=0):
    bpp, iw, ih, pix = t['bpp'], t['w16'], t['h'], t['pix']
    if bpp == 0:
        w = iw * 4
        cw = t['clut_info'][2]
        pal = t['clut'][row * cw:(row + 1) * cw]
        im = Image.new('RGBA', (w, ih))
        px = im.load()
        for y in range(ih):
            for x in range(w):
                byte = pix[y * iw * 2 + x // 2]
                idx = (byte >> 4) if (x & 1) else (byte & 15)
                px[x, y] = c15(pal[idx])
        return im, [ (y*iw*2) for y in range(0)]
    if bpp == 1:
        w = iw * 2
        cw = t['clut_info'][2]
        pal = t['clut'][row * cw:(row + 1) * cw]
        im = Image.new('RGBA', (w, ih))
        px = im.load()
        for y in range(ih):
            for x in range(w):
                px[x, y] = c15(pal[pix[y * iw * 2 + x]])
        return im, None
    if bpp == 2:
        w = iw
        im = Image.new('RGBA', (w, ih))
        px = im.load()
        for y in range(ih):
            for x in range(w):
                px[x, y] = c15(struct.unpack_from('<H', pix, (y * w + x) * 2)[0])
        return im, None
    raise ValueError(bpp)


def indices(t):
    """4bpp index grid."""
    iw, ih, pix = t['w16'], t['h'], t['pix']
    w = iw * 4
    out = [[0] * w for _ in range(ih)]
    for y in range(ih):
        for x in range(w):
            byte = pix[y * iw * 2 + x // 2]
            out[y][x] = (byte >> 4) if (x & 1) else (byte & 15)
    return out
