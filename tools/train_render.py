"""Trains MoonUp Neural Render v3: a one-step, deterministic, all-GPU neural rendering stage.

Design follows what NVIDIA has published about DLSS 5 (SIGGRAPH 2026), adapted to what a
capture-based tool can see (only the final colour frame, no G-buffer):
  * one frame in, one frame out, deterministic, conditioned on the rendered frame itself;
  * two separately controllable parts, like DLSS 5's "Tone intensity" (low frequency lighting
    and colour) and "Structure intensity" (high frequency shading and detail);
  * temporal stability from carried state (grid and structure maps blended by local change).

Tone branch (HDRNet style bilateral grid, Gharbi et al. 2017):
    low-res input 256x144 RGB (area resize)
    c1 conv3x3 s2  3->16 ReLU   128x72
    c2 conv3x3 s2 16->32 ReLU    64x36
    c3 conv3x3 s2 32->64 ReLU    32x18
    c4 conv3x3 s2 64->64 ReLU    16x9
    local:  c5 conv3x3 64->64 ReLU, c6 conv3x3 64->64
    global: mean(c4) -> FC 64->64 ReLU -> FC 64->64       (scene lighting context)
    fuse:   ReLU(local + global) -> 1x1 64->96 = grid 16x9 x 8 bins x 12 coefficients
    guide:  g = clamp(sum_c mix_c * curve_c(ccm . rgb + ccmb)_c + mixb, 0, 1)  (16-knot curves)
    tone  = A(x,y,g) [r g b 1]^T   (3x4 affine, trilinear grid lookup)

Structure branch (half resolution, luma, dilated context aggregation network, Yu & Koltun 2016;
used for fast image processing by Chen et al., ICCV 2017):
    input  half-res luma (2x2 box)
    s1 1->12 d1, s2 12->12 d2, s3 12->12 d4, s4 12->12 d8, s5 12->12 d1 (ReLU), s6 12->2 d1
    receptive field 71 half-res pixels: enough for contact shadows / ambient occlusion
    outputs a (log shading) and k (detail gain), bilinearly upsampled
    out = tone * exp(clamp(a, -1.5, 1)) + clamp(k, -1, 3) * (L - blur3x3(L))

Training pairs: photographs (Kodak, Urban100, BSDS500, General100/T91, scikit-image samples) are the targets;
inputs are the same photographs passed through a randomised "game look" model (lighting and
local contrast flattened in the log domain, contact shading / ambient occlusion removed,
shadows lifted, highlights compressed, saturation and white balance muted, TAA-like softness).
The network learns the inverse. It cannot invent light that is not in the frame (DLSS 5 has the
game's data for that); it restores and deepens what is there.

Conv conventions: 3x3, edge clamped, stride-2 output i centred on input 2i, dilation d taps at
-d, 0, +d. Weights exported as [cout][cin][ky][kx], FC as [out][in].
Outputs: src/engine/render_weights.h, ui/js/render_weights.js
"""
import glob, math, os, random, sys, time

import jax
import jax.numpy as jnp
import numpy as np
import optax
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
rng = np.random.default_rng(7)
random.seed(7)

LW, LH = 256, 144          # low-res input
FW, FH = 320, 180          # full-res training size
GW, GH, GD, GC = 16, 9, 8, 12
C1, C2, C3 = 16, 32, 64
SC = 12                    # structure branch channels
DIL = [1, 2, 4, 8, 1, 1]
KNOTS = 16


# ----------------------------------------------------------------------------- data
def load_photos():
    files = sorted(glob.glob('/tmp/sr/kodak/*.png')) + sorted(glob.glob('/tmp/sr/urban/*.png'))
    files += sorted(glob.glob('/tmp/ds/BSDS500/BSDS500/data/images/*/*.jpg'))     # BSDS500 (includes BSD100)
    files += sorted(glob.glob('/tmp/ds/FSRCNN-TensorFlow/Train/*.png'))           # General100 + T91
    ims = [Image.open(f).convert('RGB') for f in files]
    ims = [im for im in ims if im.size[0] >= 300 and im.size[1] >= 170]
    try:
        import skimage.data as d
        for n in ['astronaut', 'chelsea', 'coffee', 'rocket', 'brick', 'grass', 'gravel', 'cat']:
            a = getattr(d, n)()
            if a.ndim == 2:
                a = np.stack([a] * 3, -1)
            ims.append(Image.fromarray(a.astype(np.uint8)).convert('RGB'))
    except Exception as e:
        print('skimage', e)
    return ims


def gauss_blur(x, sigma):
    from scipy.ndimage import gaussian_filter
    return gaussian_filter(x, sigma=(sigma, sigma) + (0,) * (x.ndim - 2), mode='reflect')


def luma(x):
    return x[..., 0] * 0.299 + x[..., 1] * 0.587 + x[..., 2] * 0.114


def game_look(y):
    """Randomised degradation from a photograph to a flatter, game-like rendering."""
    eps = 0.02
    L = luma(y)
    logL = np.log(L + eps)
    s1 = rng.uniform(6, 22)        # lighting scale
    s2 = rng.uniform(1.2, 3.0)     # contact shading / ambient occlusion scale
    b1 = gauss_blur(logL[..., None], s1)[..., 0]
    b2 = gauss_blur(logL[..., None], s2)[..., 0]
    g_light = rng.uniform(0.60, 0.92)   # global lighting contrast kept
    g_mid = rng.uniform(0.55, 0.92)     # contact shading kept
    g_fine = rng.uniform(0.88, 1.0)     # texture detail kept
    m = logL.mean()
    mid = b2 - b1
    # Ambient occlusion / contact shadows are dark, local and small: remove more of the dark side.
    mid = np.where(mid < 0, mid * rng.uniform(0.45, 1.0), mid)
    newlog = m + (b1 - m) * g_light + mid * g_mid + (logL - b2) * g_fine
    L2 = np.exp(newlog) - eps
    L2 = np.clip(L2, 0, None)
    lift = rng.uniform(0.0, 0.08)
    L2 = (L2 + lift) / (1 + lift)
    h = rng.uniform(0.0, 0.4)
    L2 = L2 * (1 + h) / (1 + h * L2)
    ratio = (L2 + 1e-3) / (L + 1e-3)
    x = y * ratio[..., None]
    sat = rng.uniform(0.68, 0.97)
    Lx = luma(x)[..., None]
    x = Lx + (x - Lx) * sat
    wb = rng.uniform(0.97, 1.03, 3).astype(np.float32)
    x = x * wb
    if rng.random() < 0.7:          # temporal anti-aliasing / upscaler softness
        x = gauss_blur(x, rng.uniform(0.3, 0.85))
    return np.clip(x, 0, 1).astype(np.float32)


def area_resize(a, w, h):
    return np.asarray(Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)).resize((w, h), Image.BOX), np.float32) / 255.0


def make_pairs(ims, n):
    X, Xl, Y = [], [], []
    for k in range(n):
        im = ims[k % len(ims)]
        W, H = im.size
        # random 16:9 crop; mostly near the photograph's own pixel scale (the structure branch
        # works in pixels, game frames have photo-like pixel scale), sometimes the whole view
        maxw = int(min(W, H * 16 / 9))
        cw = int(rng.uniform(min(maxw, max(FW, 0.35 * maxw)), maxw))
        ch = int(cw * 9 / 16)
        if ch > H:
            ch = H
            cw = int(ch * 16 / 9)
        x0 = rng.integers(0, W - cw + 1)
        y0 = rng.integers(0, H - ch + 1)
        crop = im.crop((x0, y0, x0 + cw, y0 + ch)).resize((FW, FH), Image.LANCZOS)
        y = np.asarray(crop, np.float32) / 255.0
        if rng.random() < 0.5:
            y = y[:, ::-1]
        x = game_look(y)
        X.append((x * 255 + 0.5).astype(np.uint8))
        Y.append((y * 255 + 0.5).astype(np.uint8))
        Xl.append((area_resize(x, LW, LH) * 255 + 0.5).astype(np.uint8))
    return np.stack(X), np.stack(Xl), np.stack(Y)


# ----------------------------------------------------------------------------- model
def conv(x, w, b, stride, dil=1):
    """x: (N,H,W,C) edge clamped 3x3 conv. w: (3,3,Cin,Cout)."""
    xp = jnp.pad(x, ((0, 0), (dil, dil), (dil, dil), (0, 0)), mode='edge')
    y = jax.lax.conv_general_dilated(xp, w, (stride, stride), 'VALID', rhs_dilation=(dil, dil),
                                     dimension_numbers=('NHWC', 'HWIO', 'NHWC'))
    return y + b


def init_params(key):
    ks = jax.random.split(key, 24)

    def cw(k, cin, cout, s=1.0):
        return jax.random.normal(k, (3, 3, cin, cout)) * math.sqrt(2.0 / (9 * cin)) * s

    p = {
        'c1w': cw(ks[0], 3, C1), 'c1b': jnp.zeros(C1),
        'c2w': cw(ks[1], C1, C2), 'c2b': jnp.zeros(C2),
        'c3w': cw(ks[2], C2, C3), 'c3b': jnp.zeros(C3),
        'c4w': cw(ks[3], C3, C3), 'c4b': jnp.zeros(C3),
        'c5w': cw(ks[4], C3, C3), 'c5b': jnp.zeros(C3),
        'c6w': cw(ks[5], C3, C3, 0.5), 'c6b': jnp.zeros(C3),
        'f1w': jax.random.normal(ks[6], (C3, C3)) * math.sqrt(2 / C3), 'f1b': jnp.zeros(C3),
        'f2w': jax.random.normal(ks[7], (C3, C3)) * math.sqrt(1 / C3) * 0.5, 'f2b': jnp.zeros(C3),
        'ow': jax.random.normal(ks[8], (C3, GD * GC)) * 0.01,
        # identity affine bias for every bin: A = [I | 0]
        'ob': jnp.tile(jnp.array([1., 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]), GD),
        # guide
        'ccm': jnp.eye(3), 'ccmb': jnp.zeros(3),
        'slopes': jnp.zeros((3, KNOTS)).at[:, 0].set(1.0),
        'mix': jnp.ones(3) / 3, 'mixb': jnp.zeros(()),
        # structure branch
        's1w': cw(ks[10], 1, SC), 's1b': jnp.zeros(SC),
        's2w': cw(ks[11], SC, SC), 's2b': jnp.zeros(SC),
        's3w': cw(ks[12], SC, SC), 's3b': jnp.zeros(SC),
        's4w': cw(ks[13], SC, SC), 's4b': jnp.zeros(SC),
        's5w': cw(ks[14], SC, SC), 's5b': jnp.zeros(SC),
        's6w': cw(ks[15], SC, 2, 0.02), 's6b': jnp.zeros(2),
    }
    return p


def lowres_grid(p, xl):
    r = jax.nn.relu
    f = r(conv(xl, p['c1w'], p['c1b'], 2))
    f = r(conv(f, p['c2w'], p['c2b'], 2))
    f = r(conv(f, p['c3w'], p['c3b'], 2))
    f = r(conv(f, p['c4w'], p['c4b'], 2))          # (N,9,16,48)
    loc = r(conv(f, p['c5w'], p['c5b'], 1))
    loc = conv(loc, p['c6w'], p['c6b'], 1)
    g = f.mean(axis=(1, 2))
    g = r(g @ p['f1w'] + p['f1b'])
    g = g @ p['f2w'] + p['f2b']
    fused = r(loc + g[:, None, None, :])
    out = fused @ p['ow'] + p['ob']                 # (N,9,16,96)
    return out.reshape(out.shape[0], GH, GW, GD, GC)


KNOT_T = jnp.arange(KNOTS) / KNOTS


def guide(p, x):
    y = x @ p['ccm'].T + p['ccmb']                  # (N,H,W,3)
    c = (jax.nn.relu(y[..., None] - KNOT_T) * p['slopes']).sum(-1)
    g = (c * p['mix']).sum(-1) + p['mixb']
    return jnp.clip(g, 0.0, 1.0)


def slice_apply(grid, x, g):
    """Trilinear lookup of grid (N,GH,GW,GD,GC) at every pixel, then affine colour transform."""
    N, H, W, _ = x.shape
    u = (jnp.arange(W) + 0.5) / W * GW - 0.5
    v = (jnp.arange(H) + 0.5) / H * GH - 0.5
    z = g * GD - 0.5                                 # (N,H,W)
    u = jnp.clip(u, 0, GW - 1)
    v = jnp.clip(v, 0, GH - 1)
    z = jnp.clip(z, 0, GD - 1)
    u0 = jnp.floor(u).astype(jnp.int32); u1 = jnp.minimum(u0 + 1, GW - 1); fu = u - u0
    v0 = jnp.floor(v).astype(jnp.int32); v1 = jnp.minimum(v0 + 1, GH - 1); fv = v - v0
    z0 = jnp.floor(z).astype(jnp.int32); z1 = jnp.minimum(z0 + 1, GD - 1); fz = z - z0
    g00 = grid[:, v0][:, :, u0]; g01 = grid[:, v0][:, :, u1]
    g10 = grid[:, v1][:, :, u0]; g11 = grid[:, v1][:, :, u1]
    fu_ = fu[None, None, :, None, None]; fv_ = fv[None, :, None, None, None]
    gs = (g00 * (1 - fu_) + g01 * fu_) * (1 - fv_) + (g10 * (1 - fu_) + g11 * fu_) * fv_
    a0 = jnp.take_along_axis(gs, z0[..., None, None], axis=3)[..., 0, :]
    a1 = jnp.take_along_axis(gs, z1[..., None, None], axis=3)[..., 0, :]
    A = a0 * (1 - fz[..., None]) + a1 * fz[..., None]   # (N,H,W,12)
    A = A.reshape(N, H, W, 3, 4)
    out = (A[..., :3] * x[..., None, :]).sum(-1) + A[..., 3]
    return jnp.clip(out, 0, 1)


def lumaj(x):
    return x[..., 0] * 0.299 + x[..., 1] * 0.587 + x[..., 2] * 0.114


def structure_maps(p, x):
    """Half resolution structure net -> (N,H/2,W/2,2): a (log shading), k (detail gain)."""
    N, H, W, _ = x.shape
    L = lumaj(x)
    half = L.reshape(N, H // 2, 2, W // 2, 2).mean((2, 4))[..., None]
    r = jax.nn.relu
    f = half
    for i in range(5):
        f = r(conv(f, p['s%dw' % (i + 1)], p['s%db' % (i + 1)], 1, DIL[i]))
    return conv(f, p['s6w'], p['s6b'], 1, DIL[5])


def upsample2(m):
    """Bilinear 2x upsampling with clamped edges (= GPU SampleLevel of the half-res map)."""
    def up1(a, axis):
        n = a.shape[axis]
        pos = (jnp.arange(2 * n) + 0.5) / 2 - 0.5
        pos = jnp.clip(pos, 0, n - 1)
        i0 = jnp.floor(pos).astype(jnp.int32); i1 = jnp.minimum(i0 + 1, n - 1); f = pos - i0
        shape = [1] * a.ndim; shape[axis] = 2 * n
        f = f.reshape(shape)
        return jnp.take(a, i0, axis) * (1 - f) + jnp.take(a, i1, axis) * f
    return up1(up1(m, 1), 2)


def highpass(L):
    """L - [1 2 1]/4 x [1 2 1]/4 blur, edge clamped."""
    Lp = jnp.pad(L, ((0, 0), (1, 1), (1, 1)), mode='edge')
    h = (Lp[:, :, :-2] + 2 * Lp[:, :, 1:-1] + Lp[:, :, 2:]) / 4
    b = (h[:, :-2] + 2 * h[:, 1:-1] + h[:, 2:]) / 4
    return L - b


def structure_apply(tone, x, sm, s=1.0):
    m = upsample2(sm)
    a = jnp.clip(m[..., 0], -1.5, 1.0)
    k = jnp.clip(m[..., 1], -1.0, 3.0)
    hp = highpass(lumaj(x))
    return jnp.clip(tone * jnp.exp(s * a)[..., None] + (s * k * hp)[..., None], 0, 1)


def forward(p, xl, x):
    grid = lowres_grid(p, xl)
    tone = slice_apply(grid, x, guide(p, x))
    return structure_apply(tone, x, structure_maps(p, x))


def loss_fn(p, xl, x, y):
    out = forward(p, xl, x)
    l1 = jnp.abs(out - y).mean()
    # gradient term: local contrast and structure are the point of this network
    gx = lambda a: a[:, :, 1:] - a[:, :, :-1]
    gy = lambda a: a[:, 1:] - a[:, :-1]
    lg = jnp.abs(gx(out) - gx(y)).mean() + jnp.abs(gy(out) - gy(y)).mean()
    # mid-frequency (contact shading) term on 2x2 averages
    d2 = lambda a: a.reshape(a.shape[0], a.shape[1] // 2, 2, a.shape[2] // 2, 2, 3).mean((2, 4))
    o2, y2 = d2(out), d2(y)
    lg2 = jnp.abs(gx(o2) - gx(y2)).mean() + jnp.abs(gy(o2) - gy(y2)).mean()
    return l1 + 0.5 * lg + 0.5 * lg2


def psnr(a, b):
    return float(10 * np.log10(1.0 / max(np.mean((np.asarray(a) - np.asarray(b)) ** 2), 1e-12)))


# ----------------------------------------------------------------------------- export
ORDER = ['c1w', 'c1b', 'c2w', 'c2b', 'c3w', 'c3b', 'c4w', 'c4b', 'c5w', 'c5b', 'c6w', 'c6b',
         'f1w', 'f1b', 'f2w', 'f2b', 'ow', 'ob', 'ccm', 'ccmb', 'slopes', 'mix', 'mixb',
         's1w', 's1b', 's2w', 's2b', 's3w', 's3b', 's4w', 's4b', 's5w', 's5b', 's6w', 's6b']


def flatten(p):
    """Conv weights as [cout][cin][ky][kx]; FC as [out][in]; everything else row-major."""
    out, offsets = [], {}
    for k in ORDER:
        a = np.asarray(p[k], np.float32)
        if k.endswith('w') and a.ndim == 4:
            a = a.transpose(3, 2, 0, 1)
        elif k in ('f1w', 'f2w', 'ow'):
            a = a.T
        offsets[k] = len(out)
        out.extend(a.ravel().tolist())
    return np.array(out, np.float32), offsets


def export(p):
    flat, off = flatten(p)
    with open(os.path.join(ROOT, 'src/engine/render_weights.h'), 'w') as f:
        f.write('// AUTO-GENERATED by tools/train_render.py - MoonUp Neural Render v2 weights.\n#pragma once\n')
        f.write('namespace sw { namespace render {\n')
        for k, v in off.items():
            f.write(f'static const int kOff_{k} = {v};\n')
        f.write(f'static const int kWeightCount = {len(flat)};\n')
        def lit(v):
            t = '%.8g' % v
            return (t if ('.' in t or 'e' in t) else t + '.0') + 'f'
        f.write('static const float kWeights[%d] = {%s};\n' % (len(flat), ','.join(lit(v) for v in flat)))
        f.write('}}\n')
    with open(os.path.join(ROOT, 'ui/js/render_weights.js'), 'w') as f:
        f.write('// AUTO-GENERATED by tools/train_render.py\n')
        f.write('export const OFFSETS = %s;\n' % str({k: v for k, v in off.items()}).replace("'", '"'))
        f.write('export default [%s];\n' % ','.join('%.8g' % v for v in flat))
    print('exported', len(flat), 'weights', off)


# ----------------------------------------------------------------------------- main
def main():
    steps = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
    B = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    t0 = time.time()
    ims = load_photos()
    print('photos', len(ims))
    test_ims = ims[::12]
    train_ims = [im for i, im in enumerate(ims) if i % 12]
    X, XL, Y = make_pairs(train_ims, 2000)
    TX, TXL, TY = make_pairs(test_ims, 40)
    del ims, train_ims, test_ims  # keep memory low (the data is in X / XL / Y now)
    print('pairs ready', X.shape, '%.0fs' % (time.time() - t0), flush=True)
    tf = lambda a: jnp.asarray(a, jnp.float32) / 255.0
    TXf, TXLf, TYf = tf(TX), tf(TXL), tf(TY)
    print('test identity psnr %.2f' % psnr(TXf, TYf))

    p = init_params(jax.random.PRNGKey(0))
    sched = optax.cosine_onecycle_schedule(steps, 2e-3, pct_start=0.1)
    opt = optax.chain(optax.clip_by_global_norm(1.0), optax.adam(sched))
    st = opt.init(p)
    # Checkpoint / resume (long CPU runs can be interrupted).
    ckpt = os.environ.get('RENDER_CKPT', '/tmp/claude-0/render_ckpt.npz')
    start = 0
    if os.path.exists(ckpt):
        z = np.load(ckpt)
        p = {k: jnp.asarray(z[k]) for k in p}
        start = int(z['__step'])
        st = opt.init(p)
        # advance the schedule: optax counts updates in the adam state
        st = jax.tree_util.tree_map(lambda x: x, st)
        print('resumed at step', start, flush=True)

    @jax.jit
    def step(p, st, xl, x, y):
        l, g = jax.value_and_grad(loss_fn)(p, xl, x, y)
        u, st = opt.update(g, st, p)
        return optax.apply_updates(p, u), st, l

    fwd = jax.jit(forward)
    def set_count(state, n):
        return jax.tree_util.tree_map(lambda x: jnp.asarray(n, x.dtype) if (hasattr(x, 'shape') and x.shape == () and x.dtype == jnp.int32) else x, state)
    if start:
        st = set_count(st, start)
    for i in range(start, steps):
        idx = rng.integers(0, len(X), B)
        p, st, l = step(p, st, tf(XL[idx]), tf(X[idx]), tf(Y[idx]))
        if i % 250 == 0 or i == steps - 1:
            out = np.concatenate([np.asarray(fwd(p, TXLf[j:j + 10], TXf[j:j + 10])) for j in range(0, len(TX), 10)])
            print('step %d loss %.4f test psnr %.2f (%.0fs)' % (i, float(l), psnr(out, TYf), time.time() - t0), flush=True)
        if i % 250 == 249:
            np.savez(ckpt, __step=i + 1, **{k: np.asarray(v) for k, v in p.items()})
        if i % 1000 == 999:
            export(p)
    export(p)
    if os.path.exists(ckpt):
        os.remove(ckpt)
    out = np.asarray(fwd(p, TXLf[:6], TXf[:6]))
    rows = [np.concatenate([np.asarray(TXf[k]), out[k], np.asarray(TYf[k])], 1) for k in range(6)]
    Image.fromarray((np.clip(np.concatenate(rows, 0), 0, 1) * 255).astype(np.uint8)).save('/tmp/claude-0/render_sample.png')


if __name__ == '__main__':
    main()
