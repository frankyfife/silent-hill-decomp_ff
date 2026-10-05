"""Paper-map markings: export every marking sprite as a named PNG to repaint,
and build the game's marking atlases back from them.

The markings live in five 128x256 4-bit atlases (TIM/MR_*.TIM); each sprite
is a rectangle of one (tables D_800AE8A0 / D_800AEDBC in
src/bodyprog/bodyprog_mapscreen_80066D90.c, parsed by mr_cells.py into
mr_cells.json). In the atlas the ink is a cyan coverage mask that the map
screen SUBTRACTS from the paper, which is what makes it red.

  export:  python mr_markings.py export <extracted disc dir> <out dir>
           writes <out>/edit/<ATLAS>_<nn>_<name>.png (8x, smoothed, red ink on
           transparent: paint over these) and <out>/original/... (8x, the
           retail pixels, as a reference layer).
  build:   python mr_markings.py build <extracted disc dir> <out dir> <game gamedata dir>
           reads <out>/edit/*.png and writes <gamedata>/load/TIM/MR_*.TIM.png.

Only the ALPHA of an edited file counts (how much ink is there); its colour
is ignored. Keep the canvas size (8x the sprite).
"""
import json
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

import tim

S = 8
HERE = os.path.dirname(os.path.abspath(__file__))
INK = (190, 16, 16)

NAMES = {
    'MR_0TOWN': ['x_small', 'loop', 'check', 'exclaim', 'text_cafe', 'scribble_small', 'dot', 'arrow_both',
                 'arrow_right_long', 'arrow_right_short', 'scribble_tall', 'text_cheryl_arrow', 'line_vertical',
                 'text_k_gordon', 'x_big', 'circle_tall'],
    'MR_BSTLN': ['x_small', 'loop', 'check', 'arrow_left', 'x_big', 'x_wide', 'text_antique_shop'],
    'MR_SCL': ['scribble_horizontal', 'scribble_vertical', 'arrow_up_down', 'arrow_left_right', 'x_small', 'loop',
               'question_mark', 'text_piano', 'text_statue_of_hand', 'line_vertical', 'bar_horizontal',
               'line_horizontal_dotted', 'text_valve'],
    'MR_DRN': ['bracket_vertical', 'bracket_horizontal', 'line_vertical', 'line_horizontal', 'x_big'],
    'MR_HSP': ['scribble_horizontal', 'scribble_vertical', 'arrow_up_down', 'arrow_left_right', 'text_video',
               'line_vertical', 'line_horizontal'],
}


def coverage(disc, atlas):
    t = tim.read_tim(os.path.join(disc, 'TIM', atlas + '.TIM'))
    I = np.array(tim.indices(t), float)
    pal = t['clut'][:16]
    g = np.array([((v >> 5) & 31) for v in pal], float)
    lut = (g - g[1]) / max(g[15] - g[1], 1) * (14.0 / 15.0) + 1.0 / 15.0
    lut[0] = 0.0
    return lut[I.astype(int)]


def smooth(cov):
    up = ndimage.zoom(cov, S, order=3, mode='grid-constant', cval=0.0, grid_mode=True)
    up = ndimage.gaussian_filter(up, 0.32 * S)
    c = np.clip((up - 0.28) / (0.62 - 0.28), 0.0, 1.0)
    return c * c * (3.0 - 2.0 * c)


def ink_png(c):
    rgba = np.zeros(c.shape + (4,), np.uint8)
    rgba[..., 0], rgba[..., 1], rgba[..., 2] = INK
    rgba[..., 3] = np.clip(c * 255.0, 0, 255).astype(np.uint8)
    return Image.fromarray(rgba, 'RGBA')


def cells():
    return json.load(open(os.path.join(HERE, 'mr_cells.json')))


def export(disc, out):
    os.makedirs(os.path.join(out, 'edit'), exist_ok=True)
    os.makedirs(os.path.join(out, 'original'), exist_ok=True)
    for atlas, lst in cells().items():
        cov = coverage(disc, atlas)
        sm = smooth(cov)
        for i, c in enumerate(lst):
            name = '%s_%02d_%s.png' % (atlas, i, NAMES[atlas][i])
            u, v, w, h = c['u'], c['v'], c['w'], c['h']
            ink_png(sm[v * S:(v + h) * S, u * S:(u + w) * S]).save(os.path.join(out, 'edit', name))
            orig = np.kron(cov[v:v + h, u:u + w], np.ones((S, S)))
            ink_png(orig).save(os.path.join(out, 'original', name))
    print('exported to', out)


def build(disc, out, gamedata):
    dst = os.path.join(gamedata, 'load', 'TIM')
    os.makedirs(dst, exist_ok=True)
    for atlas, lst in cells().items():
        c_all = smooth(coverage(disc, atlas))
        for i, c in enumerate(lst):
            p = os.path.join(out, 'edit', '%s_%02d_%s.png' % (atlas, i, NAMES[atlas][i]))
            if not os.path.exists(p):
                continue
            u, v, w, h = c['u'], c['v'], c['w'], c['h']
            im = Image.open(p).convert('RGBA')
            if im.size != (w * S, h * S):
                im = im.resize((w * S, h * S), Image.LANCZOS)
                print('note: %s resized to %dx%d' % (os.path.basename(p), w * S, h * S))
            c_all[v * S:(v + h) * S, u * S:(u + w) * S] = np.asarray(im, float)[..., 3] / 255.0
        rgba = np.zeros(c_all.shape + (4,), np.uint8)
        rgba[..., 1] = rgba[..., 2] = np.clip(c_all * 255.0, 0, 255).astype(np.uint8)  # cyan x coverage
        rgba[..., 3] = np.where(c_all > 0.02, 255, 0)
        Image.fromarray(rgba, 'RGBA').save(os.path.join(dst, atlas + '.TIM.png'))
        print('built', os.path.join(dst, atlas + '.TIM.png'))


if __name__ == '__main__':
    if sys.argv[1] == 'export':
        export(sys.argv[2], sys.argv[3])
    else:
        build(sys.argv[2], sys.argv[3], sys.argv[4])
