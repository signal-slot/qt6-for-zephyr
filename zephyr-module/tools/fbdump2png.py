#!/usr/bin/env python3
"""fbdump2png.py <console.log> <out prefix>: the 128x75 frame thumbnails the AM62P
display glue prints ("fbdump <present> <row>:<hex rgb x128>") as PNG files."""
import re, sys
from PIL import Image
log, prefix = sys.argv[1], sys.argv[2]
frames = {}
for line in open(log, 'rb').read().decode('latin-1').splitlines():
    m = re.match(r'fbdump (\d+) (\d+):([0-9a-f]{768})\s*$', line.strip())
    if m:
        frames.setdefault(int(m.group(1)), {})[int(m.group(2))] = m.group(3)
for n, rows in sorted(frames.items()):
    img = Image.new('RGB', (128, 75))
    for y, hexrow in rows.items():
        for x in range(128):
            v = int(hexrow[x*6:x*6+6], 16)
            img.putpixel((x, y), (v >> 16, (v >> 8) & 255, v & 255))
    out = '%s-%d.png' % (prefix, n)
    img.resize((1024, 600), Image.NEAREST).save(out)
    print(out, len(rows), 'rows')
