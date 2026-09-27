"""Pack the native renderer's DXIL shaders into one blob (embedded in conan.exe
as an RCDATA resource, see $PORT_DIR/CMakeLists.txt).
usage: pack_shaders.py <dxil_dir> <out.pak>

Format (little-endian):
  'CNSH' u32 version(1) u32 count
  count x { u64 hash, u32 stage (0 = vs, 1 = ps), u32 offset, u32 size }  sorted by (hash, stage)
  data"""
import os
import re
import struct
import sys

src, out = sys.argv[1], sys.argv[2]
entries = []
for name in os.listdir(src):
    m = re.fullmatch(r'([0-9A-F]{16})\.(vs|ps)\.dxil', name)
    if m:
        entries.append((int(m.group(1), 16), 0 if m.group(2) == 'vs' else 1, name))
entries.sort()
header = struct.pack('<4sII', b'CNSH', 1, len(entries))
table_size = len(entries) * 20
offset = len(header) + table_size
table, blobs = [], []
for h, stage, name in entries:
    data = open(os.path.join(src, name), 'rb').read()
    table.append(struct.pack('<QIII', h, stage, offset, len(data)))
    blobs.append(data)
    offset += len(data)
tmp = out + '.tmp'
with open(tmp, 'wb') as f:
    f.write(header)
    f.writelines(table)
    f.writelines(blobs)
os.replace(tmp, out)
print(f'packed {len(entries)} shaders, {offset} bytes -> {out}')
