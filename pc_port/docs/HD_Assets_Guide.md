# HD Assets: Textures, Models, Font, Paper Maps

Scripts in `pc_port/tools/hd_assets/` that rebuild the game's art at a higher
quality **without changing its style**. The scripts never invent detail: no AI
upscaler, no repainting. Everything they write is a loose override under
`gamedata/load/`, so the disc image stays untouched and deleting a file brings
the original back.

None of the generated files belong in the repository. They are derived from the
retail disc, and the fan paper maps also carry third-party art. Regenerate them
locally with the commands below.

## Prerequisites

- An extracted disc tree, e.g. `dumpsxiso` plus `tools/silentassets/extract.py`,
  which gives `BG/ CHARA/ ITEM/ TIM/ 1ST/ ANIM/ ...`. The commands below call
  it `DISC`.
- Python 3 with `numpy`, `scipy` and `Pillow`.
- `allow_loose_files = 1` in `config.cfg`.
- Set `texture_packs = 0` if the remastered textures should be the only
  replacement. A DuckStation pack would otherwise still fill the palette rows
  the remaster leaves native.
- The game resolves `gamedata/load/` against its **working directory**. The
  `-data` command-line option only names the disc image. A test run from
  `pc_port/build` therefore needs its own copy of the overrides in
  `pc_port/build/gamedata/load/`. To confirm a file is loaded, run with
  `SH_LOOSE_VERBOSE=1` and look for `[LOOSE] hit` or
  `[LOOSE/HIRES] ... override(s)` in the log.

## Textures: `tex_remaster.py`

```bash
python tex_remaster.py DISC "<game>/gamedata/load" [--scale 4] [--jobs N] [--only "BG/HP*"]
```

The script processes every paletted TIM on the disc: about 1000 sheets, which
become roughly 6000 PNGs and about 1.8 GB at 4x. With 28 processes this takes
about a minute.

What happens to each sheet:

1. **Palette steps are smoothed.** The textures have 16 colours per region
   (4bpp) or 256 (8bpp), which shows as banding at high resolution. A
   bilateral filter merges neighbouring steps (colour sigma about 10/255) and
   leaves real edges alone.
2. **The sheet is enlarged** with a cubic filter, then sharpened gently
   (`SHARPEN = 0.6`).
3. **Fine grain is added** (`GRAIN = 2.2`), so surfaces never read as plastic.

The tunables are constants at the top of the script.

### Regions and palettes

A 4bpp sheet is drawn through several CLUT rows, and the same index data
decodes to garbage under the wrong palette. The script therefore processes
**each palette row on its own**, restricted to the texels the game draws
through that row.

- The row map comes from `clut_tool.py`. It is a union over every `.ILM`,
  `.PLM` and `.IPD` that references the sheet, so weapons count towards
  `HERO.TIM`.
- The result is a per-row set `<NAME>.TIM.pNN.png`. Outside its texels a row is
  transparent; no prim of that row samples there.
- Row 0 is always written, because the loader uses `p00` as the gate for a
  per-row set.
- Sheets with a single palette, and sheets no model references (2D art drawn
  by code), are processed whole. A single-palette sheet becomes
  `<NAME>.TIM.png`. A multi-palette sheet becomes a full `pNN` set.

### Transparency

- Colour `0x0000` stays transparent exactly where it was. Its edge is traced
  on the enlarged grid, so cut-outs get smooth borders instead of stair steps.
- The colour under transparent texels is bled from the neighbours, so
  filtering never pulls in a dark fringe.

### Left out

- Fonts (`1ST/FONT*`).
- The paper maps and their markings (`TIM/MP_*`, `TIM/MR_*`), which have their
  own sources (see below).
- The dev sheets in `TEST/`.

### Undo

Delete the generated `*.png` under `gamedata/load/{BG,CHARA,ITEM,TIM,1ST}`.
Keep any files that came from the other tools on this page.

## Character models: `smooth_model.py`

The round trip goes through `pc_port/tools/ilm_obj.py`. See
`Model_Modding_Guide.md` for the format and the import modes.

```bash
python ilm_obj.py export DISC/CHARA/SIBYL.ILM -o SIBYL.obj
python smooth_model.py SIBYL.obj SIBYL_s.obj --levels 2 --keep 0.35 \
       --skip 06LGUN,10RGUN,11HOLDER
copy SIBYL.ilmmeta.json SIBYL_s.ilmmeta.json
python ilm_obj.py import SIBYL_s.obj DISC/CHARA/SIBYL.ILM -o SIBYL.ILM --v7
```

The result goes to `gamedata/load/CHARA/SIBYL.ILM`.

### Smoothing

- Each rigid part gets its own Catmull-Clark subdivision; the part list, which
  is the rig, never changes.
- Open joint rings use the boundary rule, so neighbouring parts subdivide the
  shared ring identically. `--v7` then welds them across parts.
- `--keep` pulls the original corners back. Pure Catmull-Clark shrinks a
  low-poly limb too much.

### Options

- **`--soft PATTERN=a1:a2`** sets the amount of rounding per subdivision level
  for parts whose name contains `PATTERN`. `HEAD=1:0` rounds the head like a
  single subdivision and only splits it once more without moving anything,
  which keeps more of the head's own geometry.
- **`--skip`** copies hard-edged parts unchanged. The skip lists used so far:

| Model | `--skip` |
|---|---|
| `SIBYL`, `MSB` | `06LGUN,10RGUN,11HOLDER` |
| `KAU` | `06LBAG,10RGUN` |
| `DARIA` | `10RKEY,10FLAURO` |

With the texture on, a head smoothed as fully as the body keeps its face:
the features live mostly in the texture, and the UVs are subdivided with the
surface, so eyes, nose and mouth stay where they were. A head kept angular
next to a rounded body looked out of place. `--soft` stays available for a
model whose head geometry does carry the features.

### Characters done with these settings

`HERO`, `SIBYL`, `SRL`, `DARIA`, `LISA`, `BLISA`, `AR`, `TAR`, `MAR`, `BAR`,
`KAU`, `DOC` and `MSB`.

`BAR`'s own `BAR.ANM` keyframe 0 is a collapsed pose. Export it with
`--anm ANIM/MAR.ANM`; the rig is identical. The pose only affects the export,
because the bind matrices are frozen into the meta file.

To check a model in game, set `player_character = <name>` in the config. In
`map3_s01` the player is in a close shot after about 4 s.

### What not to subdivide

Level geometry (`.IPD`) is not handled, and should not be:

- Rounding melts architecture: walls, frames and stairs are hard 90° edges.
- Collision is separate data and would no longer match the drawn geometry.
- There is no high-poly (v7) path for map meshes.

## Font: `font_ttf.py`

```bash
python font_ttf.py <serif.ttf> 8 FONT16.TIM.png
```

The output goes to `gamedata/load/1ST/FONT16.TIM.png`. Each 12x16 cell is
matched against a TrueType font and redrawn at 8x into the retail glyph's ink
box, so advances, baselines and the grey ramp stay retail.

## Paper maps: `maps_hd.py`, `mr_markings.py`

### Maps

```bash
python maps_hd.py OUT_DIR SRC_DIR [--clean]
```

- The source is the 1024x763 fan maps.
- The output is `MP_*.TIM.png` at 1280x1920, which goes to
  `gamedata/load/TIM/`.
- Without `--clean`, the script finds the watermark by colour and fills it
  from the retail map.
- With `--clean`, it takes hand-retouched maps as they are. File names may
  carry a download prefix such as `imgi_1_`.

### Markings

The markings are the red pen sprites in `TIM/MR_*.TIM`.

```bash
python mr_markings.py export DISC OUT      # OUT/edit/<ATLAS>_<nn>_<name>.png to repaint
python mr_markings.py build  DISC OUT GAMEDATA
```

- Only the alpha channel of an edited sprite counts: it is the ink coverage.
  The script rebuilds the cyan subtractive atlas at 8x.
- Keep each canvas at 8x the sprite size.
