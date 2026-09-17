#!/usr/bin/env python3
"""fbdump2png.py <console.log> <out prefix>: the 128x75 frame thumbnails the AM62P
display glue prints ("fbdump <present> <row>:<hex rgb x128>") as PNG files, and its
full-resolution crops ("fbcrop <present> <row>:<hex rgb>") as <prefix>-crop-<present>.png."""
import re, sys
from PIL import Image
log, prefix = sys.argv[1], sys.argv[2]
PIX = re.compile(r'([0-9a-f]{6})(?:\*(\d+)\.)?')

def expand(row):
    """hex RGB row, a run written as rrggbb*N. -> flat hex"""
    out = []
    for m in PIX.finditer(row):
        out.append(m.group(1) * (int(m.group(2)) if m.group(2) else 1))
    return ''.join(out)

frames = {}
for line in open(log, 'rb').read().decode('latin-1').splitlines():
    m = re.match(r'fbdump (\d+) (\d+):([0-9a-f*.0-9]+)\s*$', line.strip())
    if m:
        row = expand(m.group(3))
        if len(row) == 768:
            frames.setdefault(int(m.group(1)), {})[int(m.group(2))] = row
for n, rows in sorted(frames.items()):
    img = Image.new('RGB', (128, 75))
    for y, hexrow in rows.items():
        for x in range(128):
            v = int(hexrow[x*6:x*6+6], 16)
            img.putpixel((x, y), (v >> 16, (v >> 8) & 255, v & 255))
    out = '%s-%d.png' % (prefix, n)
    img.resize((1024, 600), Image.NEAREST).save(out)
    print(out, len(rows), 'rows')

crops = {}
for line in open(log, 'rb').read().decode('latin-1').splitlines():
    m = re.match(r'fbcrop (\d+) (\d+):([0-9a-f*.0-9]+)\s*$', line.strip())
    if m:
        row = expand(m.group(3))
        if row and len(row) % 6 == 0:
            crops.setdefault(int(m.group(1)), {})[int(m.group(2))] = row
for n, rows in sorted(crops.items()):
    w = max(len(r) for r in rows.values()) // 6
    h = max(rows) + 1
    img = Image.new('RGB', (w, h))
    for y, hexrow in rows.items():
        for x in range(len(hexrow) // 6):
            v = int(hexrow[x*6:x*6+6], 16)
            img.putpixel((x, y), (v >> 16, (v >> 8) & 255, v & 255))
    out = '%s-crop-%d.png' % (prefix, n)
    img.resize((w * 2, h * 2), Image.NEAREST).save(out)
    print(out, len(rows), 'rows', w, 'wide')
