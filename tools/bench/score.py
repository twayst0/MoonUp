"""Scores generated frames: mean PSNR and the PSNR of the worst 2% of 16x16 blocks (artifacts)."""
import sys, glob, os, numpy as np
from PIL import Image
gen, sc = sys.argv[1], sys.argv[2]
for s in (sys.argv[3].split(',') if len(sys.argv) > 3 else ['parallax', 'fps', 'orbit', 'fast']):
    ps, ws = [], []
    for g in sorted(glob.glob(f'{gen}/{s}/gen_*.png')):
        i = int(os.path.basename(g)[4:8])
        a = np.asarray(Image.open(g).convert('RGB'), np.float64)
        b = np.asarray(Image.open(f'{sc}/{s}/{i:04d}.png').convert('RGB'), np.float64)
        se = ((a - b) ** 2).mean(2)
        ps.append(10 * np.log10(255 ** 2 / max(se.mean(), 1e-9)))
        h, w = se.shape
        blk = se[:h // 16 * 16, :w // 16 * 16].reshape(h // 16, 16, w // 16, 16).mean((1, 3)).ravel()
        worst = np.sort(blk)[-max(1, len(blk) // 50):].mean()
        ws.append(10 * np.log10(255 ** 2 / max(worst, 1e-9)))
    print(f'{s:9s} psnr {np.mean(ps):6.2f}  worst2% {np.mean(ws):6.2f}')
