"""List the marking sprites (cells) each paper map uses, from the decomp's
D_800AE8A0 / D_800AEDBC tables (bodyprog_mapscreen_80066D90.c)."""
import re
import json

SRC = r'D:/Projekte/silent-hill-decomp_ff/src/bodyprog/bodyprog_mapscreen_80066D90.c'
t = open(SRC, encoding='utf-8').read()

a0 = t.index('D_800AE8A0[327] = {') + 20
a1 = t.index('};', a0)
vals = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]+)', t[a0:a1])]

b0 = t.index('s_800AEDBC D_800AEDBC[] = {')
b1 = t.index('};', b0)
rows = []
for line in t[b0:b1].splitlines():
    m = re.search(r'\{\s*&D_800AE8A0\[(\d+)\],\s*&D_800AE8A0\[(\d+)\],\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)\s*\}', line)
    if m:
        rows.append(tuple(int(m.group(i), 16 if i > 2 else 10) for i in range(1, 5)))
    elif '{}' in line:
        rows.append(None)

MAPS = ["OtherPlaces", "OldTown", "FogCentralTown", "AltCentralTown", "ResortTown", "FogSchoolBF",
        "FogSchool1F", "FogSchool2F", "FogSchoolRF", "AltSchoolBF", "AltSchool1F", "AltSchool2F",
        "AltSchoolRF", "FogSewer1F", "FogSewer2F", "AltSewer", "FogHospitalBF", "FogHospital1F",
        "FogHospital2F", "FogHospital3F", "AltHospitalBF", "AltHospital1F", "AltHospital2F", "AltHospital3F"]
MARK = [None, 0, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3]
ATLAS = ['MR_0TOWN', 'MR_BSTLN', 'MR_DRN', 'MR_HSP', 'MR_SCL']


def cell(v):
    return dict(u=v & 0xFF, v=(v >> 8) & 0xFF, w=(v >> 16) & 0xFF, h=(v >> 24) & 0xFF)


atlases = {}
for idx, r in enumerate(rows):
    if r is None or idx >= len(MARK) or MARK[idx] is None:
        continue
    base, pl, f8, fa = r
    n = (fa - f8) >> 1
    at = ATLAS[MARK[idx]]
    for i in range(n):
        p = vals[pl + i]
        x = p & 0xFF
        y = (p >> 8) & 0xFF
        x = x - 256 if x > 127 else x
        y = y - 256 if y > 127 else y
        for s in ((p >> 16) & 0xFF, (p >> 24) & 0xFF):
            if s == 0:
                continue
            c = cell(vals[base + s])
            key = (c['u'], c['v'], c['w'], c['h'])
            e = atlases.setdefault(at, {}).setdefault(key, dict(c, maps=set(), at=[]))
            e['maps'].add(MAPS[idx])
            e['at'].append((x * 2, y * 2))

out = {}
for at, cells in atlases.items():
    lst = []
    for key in sorted(cells, key=lambda k: (k[1], k[0])):
        e = cells[key]
        lst.append(dict(u=e['u'], v=e['v'], w=e['w'], h=e['h'], maps=sorted(e['maps']), at=e['at'][:3]))
    out[at] = lst
    print(at, len(lst))
json.dump(out, open('mr_cells.json', 'w'), indent=1)
