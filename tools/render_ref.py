"""NumPy reference of MoonUp Neural Render v3 (reads ui/js/render_weights.js). Used to verify
the GPU implementation and by the training script's documentation of the layout."""
import json, os, re, sys
import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def load_weights():
    s = open(os.path.join(ROOT, 'ui/js/render_weights.js')).read()
    off = json.loads(re.search(r'OFFSETS = (\{.*?\});', s).group(1))
    flat = np.array([float(v) for v in re.search(r'export default \[(.*)\];', s, re.S).group(1).split(',')], np.float32)
    return flat, off


def conv(x, w, b, stride, relu, dil=1):
    """x (H,W,C); w (Cout,Cin,3,3); edge clamp; output centred on input i*stride; dilation dil."""
    H, W, C = x.shape
    oh, ow = (H - 1) // stride + 1, (W - 1) // stride + 1
    out = np.tile(b, (oh, ow, 1)).astype(np.float32)
    ys = np.arange(oh) * stride
    xs = np.arange(ow) * stride
    for ky in range(3):
        for kx in range(3):
            yy = np.clip(ys + (ky - 1) * dil, 0, H - 1)
            xx = np.clip(xs + (kx - 1) * dil, 0, W - 1)
            patch = x[yy][:, xx]                       # (oh,ow,Cin)
            out += patch @ w[:, :, ky, kx].T
    return np.maximum(out, 0) if relu else out


def luma(x):
    return x[..., 0] * 0.299 + x[..., 1] * 0.587 + x[..., 2] * 0.114


def bilinear_up(m, H, W):
    """Samples map m (h,w,C) at full resolution pixel centres, clamped (= GPU SampleLevel)."""
    h, w = m.shape[:2]
    u = np.clip((np.arange(W) + 0.5) / W * w - 0.5, 0, w - 1)
    v = np.clip((np.arange(H) + 0.5) / H * h - 0.5, 0, h - 1)
    u0 = np.floor(u).astype(int); u1 = np.minimum(u0 + 1, w - 1); fu = (u - u0)[None, :, None]
    v0 = np.floor(v).astype(int); v1 = np.minimum(v0 + 1, h - 1); fv = (v - v0)[:, None, None]
    return (m[v0][:, u0] * (1 - fu) + m[v0][:, u1] * fu) * (1 - fv) + (m[v1][:, u0] * (1 - fu) + m[v1][:, u1] * fu) * fv


def run(img, tone=1.0, color=1.0, structure=1.0):
    flat, o = load_weights()
    g = lambda k, n: flat[o[k]:o[k] + n]
    C3 = 64
    H, W, _ = img.shape
    low = np.asarray(Image.fromarray((img * 255 + 0.5).astype(np.uint8)).resize((256, 144), Image.BOX), np.float32) / 255
    f = conv(low, g('c1w', 432).reshape(16, 3, 3, 3), g('c1b', 16), 2, True)
    f = conv(f, g('c2w', 4608).reshape(32, 16, 3, 3), g('c2b', 32), 2, True)
    f = conv(f, g('c3w', 32 * C3 * 9).reshape(C3, 32, 3, 3), g('c3b', C3), 2, True)
    f4 = conv(f, g('c4w', C3 * C3 * 9).reshape(C3, C3, 3, 3), g('c4b', C3), 2, True)
    loc = conv(f4, g('c5w', C3 * C3 * 9).reshape(C3, C3, 3, 3), g('c5b', C3), 1, True)
    loc = conv(loc, g('c6w', C3 * C3 * 9).reshape(C3, C3, 3, 3), g('c6b', C3), 1, False)
    m = f4.mean((0, 1))
    hdn = np.maximum(g('f1w', C3 * C3).reshape(C3, C3) @ m + g('f1b', C3), 0)
    gl = g('f2w', C3 * C3).reshape(C3, C3) @ hdn + g('f2b', C3)
    fused = np.maximum(loc + gl, 0)
    grid = (fused @ g('ow', C3 * 96).reshape(96, C3).T + g('ob', 96)).reshape(9, 16, 8, 12)
    ccm = g('ccm', 9).reshape(3, 3); ccmb = g('ccmb', 3); slopes = g('slopes', 48).reshape(3, 16); mix = g('mix', 3); mixb = flat[o['mixb']]
    y = img @ ccm.T + ccmb
    c = (np.maximum(y[..., None] - np.arange(16) / 16, 0) * slopes).sum(-1)
    gd = np.clip((c * mix).sum(-1) + mixb, 0, 1)
    u = np.clip((np.arange(W) + 0.5) / W * 16 - 0.5, 0, 15)
    v = np.clip((np.arange(H) + 0.5) / H * 9 - 0.5, 0, 8)
    z = np.clip(gd * 8 - 0.5, 0, 7)
    u0 = np.floor(u).astype(int); u1 = np.minimum(u0 + 1, 15); fu = (u - u0)[None, :, None, None]
    v0 = np.floor(v).astype(int); v1 = np.minimum(v0 + 1, 8); fv = (v - v0)[:, None, None, None]
    gs = (grid[v0][:, u0] * (1 - fu) + grid[v0][:, u1] * fu) * (1 - fv) + (grid[v1][:, u0] * (1 - fu) + grid[v1][:, u1] * fu) * fv
    z0 = np.floor(z).astype(int); z1 = np.minimum(z0 + 1, 7); fz = (z - z0)[..., None]
    a0 = np.take_along_axis(gs, z0[..., None, None], 2)[:, :, 0]
    a1 = np.take_along_axis(gs, z1[..., None, None], 2)[:, :, 0]
    A = (a0 * (1 - fz) + a1 * fz).reshape(H, W, 3, 4)
    out = np.clip((A[..., :3] * img[..., None, :]).sum(-1) + A[..., 3], 0, 1)
    d = out - img
    dl = (d * np.array([0.299, 0.587, 0.114])).sum(-1, keepdims=True)
    toned = img + tone * dl + color * (d - dl)
    # structure branch
    L = luma(img)
    hh, hw = (H + 1) // 2, (W + 1) // 2
    Lp = np.pad(L, ((0, hh * 2 - H), (0, hw * 2 - W)), mode='edge')
    half = Lp.reshape(hh, 2, hw, 2).mean((1, 3))[..., None]
    f = half
    cin = 1
    SC = 12
    for i, dil in enumerate([1, 2, 4, 8, 1]):
        f = conv(f, g('s%dw' % (i + 1), SC * cin * 9).reshape(SC, cin, 3, 3), g('s%db' % (i + 1), SC), 1, True, dil)
        cin = SC
    sm = conv(f, g('s6w', 2 * SC * 9).reshape(2, SC, 3, 3), g('s6b', 2), 1, False)
    mu = bilinear_up(sm, H, W)
    a = np.clip(mu[..., 0], -1.5, 1.0)
    k = np.clip(mu[..., 1], -1.0, 3.0)
    Lq = np.pad(L, 1, mode='edge')
    hb = (Lq[:, :-2] + 2 * Lq[:, 1:-1] + Lq[:, 2:]) / 4
    bl = (hb[:-2] + 2 * hb[1:-1] + hb[2:]) / 4
    hp = L - bl
    res = toned * np.exp(structure * a)[..., None] + (structure * k * hp)[..., None]
    return np.clip(res, 0, 1)


if __name__ == '__main__':
    img = np.asarray(Image.open(sys.argv[1]).convert('RGB'), np.float32) / 255
    out = run(img)
    Image.fromarray((out * 255 + 0.5).astype(np.uint8)).save(sys.argv[2])
    if len(sys.argv) > 3:
        gpu = np.asarray(Image.open(sys.argv[3]).convert('RGB'), np.float32) / 255
        d = np.abs(gpu - out)
        print('gpu vs reference: mean abs %.4f  max %.4f  psnr %.2f' % (d.mean(), d.max(), 10 * np.log10(1 / np.mean(d ** 2))))
        print('change applied: mean abs %.4f' % np.abs(out - img).mean())
