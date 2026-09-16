#!/usr/bin/env python3
"""harvest-shaders.py -- collect shaders YakoGL's offline table lacked from a board console log.

When Qt's GLES2 RHI fails to compile a shader it prints "Failed to compile
shader: ..." followed by "Source was:" and the complete GLSL text; YakoGL's
glCompileShader logs the SHA-256 the table was searched for.  Qt Quick 3D
generates material shaders at run time for variants its build-time shadergen
did not produce (a material whose texture map is set dynamically, for one),
and this is how those reach tools/shaders/ for glsl2usc: byte-exact, the SHA
re-checked against the driver's.

usage: harvest-shaders.py <console.log> <out dir>
"""
import hashlib, os, re, sys

log, out = sys.argv[1], sys.argv[2]
data = open(log, 'rb').read().replace(b'\r\n', b'\n')
lines = data.split(b'\n')
os.makedirs(out, exist_ok=True)
sha_re = re.compile(rb'offline shaderc: (vertex|fragment) shader not in offline table \(sha256=([0-9a-f]{64})\)')
want = {}   # sha -> stage, in order of appearance
for l in lines:
    m = sha_re.search(l)
    if m:
        want.setdefault(m.group(2).decode(), m.group(1).decode())
found = 0
i = 0
while i < len(lines):
    if lines[i].startswith(b'Source was:'):
        j = i + 1
        body = []
        while j < len(lines) and not lines[j].startswith(b'Failed to build graphics pipeline') \
                and not lines[j].startswith(b'Failed to compile shader'):
            body.append(lines[j]); j += 1
        text = b'\n'.join(body).rstrip(b'\n')
        for cand in (text + b'\n\n', text + b'\n', text):
            sha = hashlib.sha256(cand).hexdigest()
            if sha in want:
                name = '%s.%s' % (sha[:16], 'vert' if want[sha] == 'vertex' else 'frag')
                path = os.path.join(out, name)
                if not os.path.exists(path):
                    open(path, 'wb').write(cand)
                    print('harvested', name, len(cand), 'bytes')
                    found += 1
                break
        i = j
    else:
        i += 1
missing = [s for s in want if not any(f.startswith(s[:16]) for f in os.listdir(out))]
print('%d shader(s) harvested; %d still missing: %s' % (found, len(missing), ' '.join(m[:16] for m in missing)))
