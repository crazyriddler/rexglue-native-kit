"""Pairwise native vs Xenos resolve comparison for an A/B dump directory.
usage: ab_resolves.py <dir> <out.png>"""
import glob, os, re, struct, sys
import numpy as np
from PIL import Image, ImageDraw

def load(path):
    b = open(path, 'rb').read()
    w, h, f, p = struct.unpack('<4I', b[:16])
    d = np.frombuffer(b[16:16 + p * h], np.uint8).reshape(h, p)
    if f in (9, 10, 11):
        return d[:, :w * 8].copy().view(np.float16).reshape(h, w, 4).astype(np.float32)
    if f in (27, 28, 29):
        return d[:, :w * 4].reshape(h, w, 4).astype(np.float32) / 255
    if f == 87:
        return d[:, :w * 4].reshape(h, w, 4)[..., [2, 1, 0, 3]].astype(np.float32) / 255
    if f == 41:
        v = d[:, :w * 4].copy().view(np.float32).reshape(h, w)
        return np.stack([v, v, v, np.ones_like(v)], -1)
    return None

def show(v, scale):
    v = np.nan_to_num(v[..., :3]) / max(scale, 1e-6)
    return Image.fromarray((np.clip(v, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8))

d = sys.argv[1]
rows = []
for xf in sorted(glob.glob(os.path.join(d, 'xenos_*.raw'))):
    m = re.match(r'xenos_([0-9A-F]{8})_gf\d+_(\d+)x(\d+)', os.path.basename(xf))
    addr, w, h = m.group(1), m.group(2), m.group(3)
    nat = glob.glob(os.path.join(d, f'resolve_{addr}_f*_{w}x{h}.raw'))
    if not nat:
        continue
    a, b = load(nat[0]), load(xf)
    if a is None or b is None or a.shape != b.shape:
        continue
    diff = np.abs(np.nan_to_num(a[..., :3]) - np.nan_to_num(b[..., :3]))
    scale = max(np.nanpercentile(b[..., :3], 99), 1e-6)
    print(f'{addr} {w}x{h}: native mean {np.nanmean(a[..., :3]):.4f} xenos mean {np.nanmean(b[..., :3]):.4f} '
          f'nan {np.isnan(a).sum()}/{np.isnan(b).sum()} rel_diff {diff.mean() / scale:.4f}')
    ia, ib = show(a, scale).resize((256, 144)), show(b, scale).resize((256, 144))
    row = Image.new('RGB', (512, 144))
    row.paste(ia, (0, 0)); row.paste(ib, (256, 0))
    ImageDraw.Draw(row).text((4, 4), f'{addr} native | xenos', fill=(255, 0, 0))
    rows.append(row)
sheet = Image.new('RGB', (512, 144 * len(rows)))
for i, r in enumerate(rows):
    sheet.paste(r, (0, 144 * i))
sheet.save(sys.argv[2])
