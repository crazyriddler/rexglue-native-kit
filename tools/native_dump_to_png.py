"""Convert native renderer frame dumps to PNG.

Each .raw file: u32 width, height, dxgi_format, row_pitch, then rows.
usage: native_dump_to_png.py <dir>
"""
import glob
import os
import struct
import sys

import numpy as np
from PIL import Image

d = sys.argv[1]
for f in sorted(glob.glob(os.path.join(d, '*.raw'))):
    b = open(f, 'rb').read()
    w, h, fmt, pitch = struct.unpack('<4I', b[:16])
    data = np.frombuffer(b[16:16 + pitch * h], dtype=np.uint8).reshape(h, pitch)
    img = None
    note = ''
    if fmt in (27, 28, 29):  # R8G8B8A8
        img = data[:, :w * 4].reshape(h, w, 4).astype(np.float32)
    elif fmt == 87:  # B8G8R8A8
        img = data[:, :w * 4].reshape(h, w, 4)[:, :, [2, 1, 0, 3]].astype(np.float32)
    elif fmt in (24, 25):  # R10G10B10A2
        v = data[:, :w * 4].copy().view(np.uint32).reshape(h, w)
        img = np.stack([(v >> 2) & 0xFF, (v >> 12) & 0xFF, (v >> 22) & 0xFF,
                        np.full_like(v, 255)], -1).astype(np.float32)
    elif fmt in (9, 10, 11):  # R16G16B16A16_FLOAT
        v = data[:, :w * 8].copy().view(np.float16).reshape(h, w, 4).astype(np.float32)
        note = f'range {np.nanmin(v[..., :3]):.3f}..{np.nanmax(v[..., :3]):.3f} nan={np.isnan(v).sum()}'
        v = np.nan_to_num(v)
        img = np.clip(v / (1.0 + np.abs(v)), 0, 1) * 255
        img[..., 3] = 255
    elif fmt == 41:  # R32_FLOAT
        v = data[:, :w * 4].copy().view(np.float32).reshape(h, w)
        lo, hi = np.nanmin(v), np.nanmax(v)
        note = f'R32F range {lo}..{hi}'
        n = (v - lo) / (hi - lo + 1e-9) * 255
        img = np.stack([n, n, n, np.full_like(n, 255)], -1)
    elif fmt in (44, 45, 46):  # R24G8
        v = data[:, :w * 4].copy().view(np.uint32).reshape(h, w) & 0xFFFFFF
        n = v.astype(np.float32) / 0xFFFFFF
        note = f'depth range {n.min():.4f}..{n.max():.4f}'
        n = (n - n.min()) / (n.max() - n.min() + 1e-9) * 255
        img = np.stack([n, n, n, np.full_like(n, 255)], -1)
    elif fmt in (34, 35, 37):  # R16G16 (typeless/unorm/snorm)
        v = data[:, :w * 4].copy().view(np.uint16).reshape(h, w, 2).astype(np.float32) / 65535 * 255
        img = np.stack([v[..., 0], v[..., 1], np.zeros_like(v[..., 0]),
                        np.full_like(v[..., 0], 255)], -1)
    if img is None:
        print('  unsupported format', fmt, os.path.basename(f))
        continue
    mean = img[..., :3].mean()
    print(f'{os.path.basename(f)} {w}x{h} fmt{fmt} mean={mean:.1f} {note}')
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGBA').convert('RGB').save(f[:-4] + '.png')
