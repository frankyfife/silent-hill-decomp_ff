#!/usr/bin/env python3
"""Generate the launcher's level-select data (maps + spawn points).

Reads the same tables the game resolves `-spawn` against, so the indices in
`door:<n>` / `room:<n>` always line up with pc_level_select.c:
  - src/map_registry.c      MAP_NAMES / MAP_DESCRIPTIONS
  - include/pc_rando_data.h RANDO_ARRIVALS_<MAP>[] (door arrivals, "from <map>")
                            RANDO_ROOMS_<MAP>[]    (room-arrival points)

Usage: python tools/gen_level_select.py [out.json]
Default output: launcher_qt/resources/level_select.json
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(ROOT, "src", "map_registry.c")
RANDO = os.path.join(ROOT, "include", "pc_rando_data.h")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "launcher_qt", "resources", "level_select.json")


def table(src, name):
    m = re.search(r"\b" + name + r"\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        sys.exit("table not found: " + name)
    return dict(re.findall(r"\[MapIdx_(MAP\d_S\d\d)\]\s*=\s*\"([^\"]*)\"", m.group(1)))


def entries(src, array):
    m = re.search(r"static const \w+ " + array + r"\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    return m.group(1).strip().splitlines() if m else []


def coord(line, field):
    m = re.search(r"\." + field + r"\s*=\s*Q12\((-?[\d.]+)f?\)", line)
    return float(m.group(1)) if m else 0.0


registry = open(REGISTRY, encoding="utf-8").read()
rando = open(RANDO, encoding="utf-8").read()
names = table(registry, "MAP_NAMES")
descs = table(registry, "MAP_DESCRIPTIONS")

maps = []
for key in sorted(names, key=lambda k: (int(k[3]), int(k[6:]))):
    mid = names[key]
    desc = descs.get(key, "")
    spawns = [{"arg": "default", "label": "Map start"}]

    seen = {}
    for i, line in enumerate(entries(rando, "RANDO_ARRIVALS_" + key)):
        src = re.search(r"/\*\s*from (map\d_s\d\d)\s*\*/", line)
        src = src.group(1) if src else "?"
        seen[src] = seen.get(src, 0) + 1
        x, z = coord(line, "positionX"), coord(line, "positionZ")
        label = "Door from %s (%s)" % (src, descs.get(src.upper(), src))
        if seen[src] > 1:
            label += " #%d" % seen[src]
        spawns.append({"arg": "door:%d" % i, "label": label, "x": x, "z": z})

    for i, line in enumerate(entries(rando, "RANDO_ROOMS_" + key)):
        tail = re.search(r"\}\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", line)
        point = int(tail.group(2)) if tail else i
        x, z = coord(line, "positionX"), coord(line, "positionZ")
        spawns.append({"arg": "room:%d" % i,
                       "label": "Room arrival, point %d (x %.1f, z %.1f)" % (point, x, z),
                       "x": x, "z": z})

    maps.append({"id": mid, "name": desc, "usable": desc != "Unused", "spawns": spawns})

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    json.dump({"generated_by": "pc_port/tools/gen_level_select.py", "maps": maps}, f, indent=1)
    f.write("\n")
print("%s: %d maps, %d spawns" % (OUT, len(maps), sum(len(m["spawns"]) for m in maps)))
