"""Paper-map markings (MR_*.TIM) -> smooth 4x replacements.

The atlas is a 4-bit cyan coverage mask (palette 1..15 = stroke coverage);
the map screen draws it SUBTRACTIVELY, so cyan taken off the paper reads red.
Each atlas is rebuilt at 4x: coverage upsampled smoothly, the pixel stairs
blurred out, then pulled back to a crisp stroke edge. It stays cyan, scaled by
coverage (a soft edge subtracts less), opaque wherever there is ink.
"""
import os
import sys
import numpy as np
from PIL import Image
from scipy import ndimage

import tim

S = 4
OUT = sys.argv[1] if len(sys.argv) > 1 else 'mr_out'
os.makedirs(OUT, exist_ok=True)

for name in ['MR_0TOWN', 'MR_BSTLN', 'MR_DRN', 'MR_HSP', 'MR_SCL']:
    t = tim.read_tim(f'out/TIM/{name}.TIM')
    I = np.array(tim.indices(t), float)
    pal = t['clut'][:16]
    # coverage from the palette's brightness (green channel), 0 for index 0
    gch = np.array([((v >> 5) & 31) for v in pal], float)
    cov_lut = (gch - gch[1]) / max(gch[15] - gch[1], 1) * (14.0 / 15.0) + 1.0 / 15.0
    cov_lut[0] = 0.0
    cov = cov_lut[I.astype(int)]
    up = ndimage.zoom(cov, S, order=3, mode='grid-constant', cval=0.0, grid_mode=True)
    up = ndimage.gaussian_filter(up, 0.32 * S)
    c = np.clip((up - 0.28) / (0.62 - 0.28), 0.0, 1.0)
    c = c * c * (3.0 - 2.0 * c)
    full = np.array([0.0, 255.0, 255.0])
    rgb = full[None, None, :] * c[..., None]
    alpha = np.where(c > 0.02, 255, 0)
    img = np.dstack([rgb, alpha]).astype(np.uint8)
    Image.fromarray(img, 'RGBA').save(os.path.join(OUT, name + '.TIM.png'))
    print(name, img.shape)
