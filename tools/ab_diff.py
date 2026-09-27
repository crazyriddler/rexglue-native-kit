"""Compare a native final-frame dump with the Xenos screenshot of the same run.
usage: ab_diff.py <native png 1280x720> <xenos bmp> [diff_out.png]"""
import sys
import numpy as np
from PIL import Image

a = np.asarray(Image.open(sys.argv[1]).convert('RGB').resize((1280, 720))).astype(np.float32)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB').resize((1280, 720))).astype(np.float32)
d = np.abs(a - b)
mse = (d ** 2).mean()
psnr = 10 * np.log10(255 ** 2 / max(mse, 1e-9))
print(f'mean_abs={d.mean():.2f} psnr={psnr:.2f}dB pixels>32={(d.max(-1) > 32).mean() * 100:.2f}%')
if len(sys.argv) > 3:
    Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(sys.argv[3])
