"""Contact sheet of per-pass native dumps (native_dump_after_pass=99).
usage: dump_sheet.py <dir> <out.png> <surface name suffix> [passes...]"""
import sys, struct, os
import numpy as np
from PIL import Image, ImageDraw

def load(path):
    b = open(path, 'rb').read()
    w, h, f, p = struct.unpack('<4I', b[:16])
    d = np.frombuffer(b[16:16 + p * h], np.uint8).reshape(h, p)
    if f in (9, 10, 11):
        v = d[:, :w * 8].copy().view(np.float16).reshape(h, w, 4)[..., :3].astype(np.float32)
        v = np.nan_to_num(v); v = np.clip(v / max(np.percentile(v, 99), 1e-6), 0, 1) ** (1 / 2.2) * 255
    elif f in (44, 45, 46):
        z = (d[:, :w * 4].copy().view(np.uint32).reshape(h, w) & 0xFFFFFF) / 0xFFFFFF
        z = np.clip((z - 0.7) / 0.3, 0, 1) * 255
        v = np.stack([z, z, z], -1)
    elif f in (27, 28, 29):
        v = d[:, :w * 4].reshape(h, w, 4)[..., :3].astype(np.float32)
    else:
        v = np.zeros((h, w, 3))
    return Image.fromarray(v.astype(np.uint8))

d, out, suffix = sys.argv[1], sys.argv[2], sys.argv[3]
passes = [int(x) for x in sys.argv[4:]] or list(range(4, 28))
tiles = []
for p in passes:
    fn = os.path.join(d, f'p{p:02}_{suffix}.raw')
    if os.path.exists(fn):
        im = load(fn).resize((256, 144))
        ImageDraw.Draw(im).text((4, 4), f'p{p}', fill=(255, 0, 0))
        tiles.append(im)
cols = 4
sheet = Image.new('RGB', (256 * cols, 144 * ((len(tiles) + cols - 1) // cols)))
for i, t in enumerate(tiles):
    sheet.paste(t, (256 * (i % cols), 144 * (i // cols)))
sheet.save(out)
