"""Per-swap native vs Xenos comparison for bench/ab_multi.sh.
usage: ab_multi.py <native_dump_dir> <xenos_screenshot_dir> <swap,swap,...> <sheet.png>"""
import os
import sys

import numpy as np
from PIL import Image

ndir, xdir, swaps, sheet_path = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
rows = []
for s in [int(x) for x in swaps.split(',') if x]:
    npath = os.path.join(ndir, f's{s:06d}_output_1280x720.png')
    xpath = os.path.join(xdir, f'swap_{s:06d}.bmp')
    if not os.path.exists(npath) or not os.path.exists(xpath):
        print(f'swap {s}: missing {"native" if not os.path.exists(npath) else "xenos"}')
        continue
    a = np.asarray(Image.open(npath).convert('RGB')).astype(np.float64)
    b = np.asarray(Image.open(xpath).convert('RGB').resize((a.shape[1], a.shape[0]))).astype(np.float64)
    d = np.abs(a - b)
    mse = (d ** 2).mean()
    psnr = 10 * np.log10(255 ** 2 / mse) if mse > 0 else 99.0
    print(f'swap {s}: psnr={psnr:.2f}dB mean_abs={d.mean():.2f} pixels>32={100 * (d.max(-1) > 32).mean():.2f}% '
          f'native_mean={a.mean():.1f} xenos_mean={b.mean():.1f}')
    w, h = 320, 180
    row = Image.new('RGB', (w * 3, h))
    row.paste(Image.fromarray(a.astype(np.uint8)).resize((w, h)), (0, 0))
    row.paste(Image.fromarray(b.astype(np.uint8)).resize((w, h)), (w, 0))
    row.paste(Image.fromarray(np.clip(d * 8, 0, 255).astype(np.uint8)).resize((w, h)), (2 * w, 0))
    rows.append(row)
if rows:
    sheet = Image.new('RGB', (rows[0].width, rows[0].height * len(rows)))
    for i, r in enumerate(rows):
        sheet.paste(r, (0, i * r.height))
    sheet.save(sheet_path)
