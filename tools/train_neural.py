"""Trains MoonUp Neural SR v2 (2x luma super-resolution for games).

Architecture (same layout as ArtCNN C4F16, MIT, https://github.com/Artoriuz/ArtCNN, so the same
compute shaders run both networks; must match src/engine/shaders/neural.hlsl):
    c0 conv3x3 1->16                    (kept as a skip)
    c1..c4 conv3x3 16->16 ReLU
    c5 conv3x3 16->16, + c0
    c6 conv3x3 16->4  -> pixel shuffle (channel s = sy*2+sx -> HR pixel (2x+sx, 2y+sy))
MoonUp predicts the luma residual over a Catmull-Rom 2x base (residual mode); ArtCNN's own
weights predict luma directly (direct mode). Edge-replicate padding (the shader clamps loads).

Data: ~790 photographs (Kodak, Urban100, BSDS500, General100/T91, scikit-image samples), synthetic
game frames (tools/bench scenes: textured 3D-like views with HUD), synthetic UI / text and
perspective scenes, and game screenshots placed in /tmp/sr/games when available. LR images are
produced like a game renders at low resolution: area (supersampled), point (aliased), bicubic,
plus TAA-like softness and mild noise on some samples.

Outputs: src/engine/neural_weights.h (MoonUp + ArtCNN weights), ui/js/neural_weights.js
"""
import glob, math, os, random, sys, time

import jax
import jax.numpy as jnp
import numpy as np
import optax
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
rng = np.random.default_rng(1234)
random.seed(1234)
F = 16


def to_y(img):
    a = np.asarray(img.convert("RGB"), dtype=np.float32) / 255.0
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


# ----------------------------------------------------------------------------- data
def load_photos():
    files = sorted(glob.glob("/tmp/sr/kodak/*.png")) + sorted(glob.glob("/tmp/sr/urban/*.png"))
    files += sorted(glob.glob("/tmp/ds/BSDS500/BSDS500/data/images/*/*.jpg"))
    files += sorted(glob.glob("/tmp/ds/FSRCNN-TensorFlow/Train/*.png"))
    ims = [Image.open(f) for f in files]
    try:
        import skimage.data as d

        for n in ["astronaut", "camera", "chelsea", "coffee", "rocket", "page", "text", "brick", "grass", "gravel", "cat"]:
            a = getattr(d, n)()
            if a.dtype != np.uint8:
                a = (np.clip(a, 0, 1) * 255).astype(np.uint8)
            ims.append(Image.fromarray(a))
    except Exception as e:
        print("skimage unavailable", e)
    return [to_y(im) for im in ims if min(im.size) >= 100]


def load_games():
    files = sorted(glob.glob("/tmp/claude-0/scenes/*/*.png")) + sorted(glob.glob("/tmp/sr/games/*.*"))
    return [to_y(Image.open(f)) for f in files]


FONTS = [f for f in glob.glob("/usr/share/fonts/**/*.[ot]tf", recursive=True) if "Inter" in f or "DejaVu" in f or "Noto" in f][:20]


def synth_ui(w=512, h=512):
    im = Image.new("RGB", (w, h), tuple(int(x) for x in rng.integers(0, 80, 3)))
    dr = ImageDraw.Draw(im)
    for _ in range(rng.integers(6, 14)):
        x0, y0 = rng.integers(0, w - 40), rng.integers(0, h - 20)
        x1, y1 = x0 + rng.integers(20, 220), y0 + rng.integers(8, 120)
        dr.rectangle([x0, y0, x1, y1], fill=tuple(int(x) for x in rng.integers(0, 255, 3)))
    for _ in range(rng.integers(8, 20)):
        f = ImageFont.truetype(random.choice(FONTS), int(rng.integers(9, 30))) if FONTS else None
        txt = "".join(random.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 :/-%") for _ in range(rng.integers(4, 24)))
        dr.text((int(rng.integers(0, w - 60)), int(rng.integers(0, h - 20))), txt, fill=tuple(int(x) for x in rng.integers(120, 256, 3)), font=f)
    for _ in range(rng.integers(4, 12)):
        dr.line([tuple(rng.integers(0, w, 2)), tuple(rng.integers(0, h, 2))], fill=tuple(int(x) for x in rng.integers(0, 256, 3)), width=int(rng.integers(1, 3)))
    for _ in range(rng.integers(2, 6)):
        c, r = rng.integers(40, w - 40, 2), rng.integers(6, 40)
        dr.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], outline=tuple(int(x) for x in rng.integers(0, 256, 3)), width=int(rng.integers(1, 3)))
    return im


def synth_scene(w=512, h=512):
    # Perspective checkerboard + sky gradient + shaded discs, rendered with 4x supersampling.
    S = 4
    ys, xs = np.mgrid[0 : h * S, 0 : w * S].astype(np.float32)
    u, v = (xs + 0.5) / (w * S), (ys + 0.5) / (h * S)
    hor = rng.uniform(0.3, 0.6)
    img = np.zeros((h * S, w * S), np.float32)
    sky = 0.3 + 0.6 * v / hor
    ground = v > hor
    dy = np.maximum(v - hor, 1e-4)
    z = rng.uniform(0.3, 1.2) / dy
    xw = (u - 0.5) * z * rng.uniform(1, 3)
    chk = ((np.floor(xw) + np.floor(z * rng.uniform(0.5, 2))) % 2).astype(np.float32)
    fog = np.exp(-z * 0.08)
    g = (chk * 0.7 + 0.15) * fog + 0.55 * (1 - fog)
    img = np.where(ground, g, sky)
    for _ in range(rng.integers(1, 5)):
        cx, cy, r = rng.uniform(0.1, 0.9), rng.uniform(0.2, 0.9), rng.uniform(0.03, 0.15)
        d = np.sqrt((u - cx) ** 2 + (v - cy) ** 2)
        shade = np.clip(1 - ((u - cx + r * 0.4) ** 2 + (v - cy + r * 0.4) ** 2) / (r * r * 2.2), 0, 1)
        img = np.where(d < r, 0.15 + 0.8 * shade, img)
    img = img.reshape(h, S, w, S).mean(axis=(1, 3))
    a = (np.clip(img, 0, 1) * 255).astype(np.uint8)
    return Image.fromarray(np.stack([a] * 3, -1))


def degrade(hr):
    """hr: HxW float (even dims). Returns LR (H/2 x W/2) like a game rendered at lower resolution."""
    r = rng.random()
    if r < 0.45:
        lr = hr.reshape(hr.shape[0] // 2, 2, hr.shape[1] // 2, 2).mean(axis=(1, 3))
    elif r < 0.75:
        im = Image.fromarray((hr * 255).astype(np.uint8))
        lr = np.asarray(im.resize((hr.shape[1] // 2, hr.shape[0] // 2), Image.BICUBIC), np.float32) / 255.0
    else:
        lr = hr[rng.integers(0, 2) :: 2, rng.integers(0, 2) :: 2][: hr.shape[0] // 2, : hr.shape[1] // 2]
    if rng.random() < 0.3:  # TAA / upscaler softness
        from scipy.ndimage import gaussian_filter
        lr = gaussian_filter(lr, rng.uniform(0.3, 0.7), mode="reflect")
    if rng.random() < 0.15:
        lr = lr + rng.normal(0, 0.006, lr.shape).astype(np.float32)
    return np.clip(lr, 0, 1).astype(np.float32)


def build_dataset(n_patches=32000, P=96):
    photos = load_photos()
    games = load_games()
    synth = []
    for i in range(60):
        synth.append(to_y(synth_ui()))
        synth.append(to_y(synth_scene()))
    print("photos", len(photos), "game frames", len(games), "synthetic", len(synth))
    X = np.zeros((n_patches, P // 2, P // 2), np.float32)
    Y = np.zeros((n_patches, P, P), np.float32)
    for i in range(n_patches):
        r = rng.random()
        pool = photos if r < 0.6 or not games else games if r < 0.85 else synth
        src = pool[rng.integers(0, len(pool))]
        h, w = src.shape
        if h <= P or w <= P:
            src = np.asarray(Image.fromarray((src * 255).astype(np.uint8)).resize((max(w, P + 2), max(h, P + 2)), Image.BICUBIC), np.float32) / 255
            h, w = src.shape
        y0, x0 = rng.integers(0, h - P), rng.integers(0, w - P)
        hr = src[y0 : y0 + P, x0 : x0 + P]
        if rng.random() < 0.5:
            hr = hr[:, ::-1]
        hr = np.rot90(hr, rng.integers(0, 4)).copy()
        X[i] = degrade(hr)
        Y[i] = hr
    return X, Y


# ----------------------------------------------------------------------------- model
def cr(x):
    x = abs(x)
    if x < 1:
        return 1.5 * x**3 - 2.5 * x**2 + 1
    if x < 2:
        return -0.5 * x**3 + 2.5 * x**2 - 4 * x + 2
    return 0.0


# Phase weights for the exact 2x pixel-centre convention used by the shader.
W_EVEN = jnp.array([cr(-1.75), cr(-0.75), cr(0.25), cr(1.25)], jnp.float32)  # LR taps k-2..k+1
W_ODD = jnp.array([cr(-1.25), cr(-0.25), cr(0.75), cr(1.75)], jnp.float32)  # LR taps k-1..k+2


def upsample_1d(x, axis):
    n = x.shape[axis]
    xp = jnp.pad(x, [(2, 2) if a == axis else (0, 0) for a in range(x.ndim)], mode="edge")
    take = lambda s: jax.lax.slice_in_dim(xp, s, s + n, axis=axis)
    even = W_EVEN[0] * take(0) + W_EVEN[1] * take(1) + W_EVEN[2] * take(2) + W_EVEN[3] * take(3)
    odd = W_ODD[0] * take(1) + W_ODD[1] * take(2) + W_ODD[2] * take(3) + W_ODD[3] * take(4)
    out = jnp.stack([even, odd], axis=axis + 1)
    shape = list(x.shape)
    shape[axis] = 2 * n
    return out.reshape(shape)


def bicubic2x(x):  # x: (B, H, W)
    return upsample_1d(upsample_1d(x, 1), 2)


def conv(x, w, b):  # x NHWC, w HWIO
    xp = jnp.pad(x, ((0, 0), (1, 1), (1, 1), (0, 0)), mode="edge")
    y = jax.lax.conv_general_dilated(xp, w, (1, 1), "VALID", dimension_numbers=("NHWC", "HWIO", "NHWC"))
    return y + b


LAYERS = [(1, F), (F, F), (F, F), (F, F), (F, F), (F, F), (F, 4)]


def init_params(key):
    ks = jax.random.split(key, len(LAYERS))
    p = {}
    for i, (cin, cout) in enumerate(LAYERS):
        s = 0.1 if i == len(LAYERS) - 1 else (0.5 if i == 5 else 1.0)
        p[f"w{i}"] = jax.random.normal(ks[i], (3, 3, cin, cout)) * math.sqrt(2.0 / (9 * cin)) * s
        p[f"b{i}"] = jnp.zeros(cout)
    return p


def shuffle(r):
    B, h, w, _ = r.shape
    return r.reshape(B, h, w, 2, 2).transpose(0, 1, 3, 2, 4).reshape(B, h * 2, w * 2)


def forward(p, lr):  # lr: (B, h, w) -> luma (B, 2h, 2w)
    x0 = conv(lr[..., None], p["w0"], p["b0"])
    x = x0
    for i in range(1, 5):
        x = jax.nn.relu(conv(x, p[f"w{i}"], p[f"b{i}"]))
    x = conv(x, p["w5"], p["b5"]) + x0
    r = conv(x, p["w6"], p["b6"])
    return bicubic2x(lr) + shuffle(r)


def loss_fn(p, lr, hr):
    pred = forward(p, lr)
    d = (pred - hr)[:, 2:-2, 2:-2]  # Charbonnier, ignore a 2px border
    return jnp.mean(jnp.sqrt(d * d + 1e-6))


def psnr(a, b):
    m = float(np.mean((np.clip(a, 0, 1) - b) ** 2))
    return 10 * math.log10(1.0 / max(m, 1e-12))


# ----------------------------------------------------------------------------- export
def flatten_layers(ws):
    """ws: list of (w[cout][cin][3][3], b[cout]) -> flat float list, per layer w then b."""
    flat = []
    for w, b in ws:
        flat.extend(np.asarray(w, np.float32).ravel().tolist())
        flat.extend(np.asarray(b, np.float32).ravel().tolist())
    return flat


def artcnn_layers():
    """ArtCNN C4F16 weights from its ONNX file (MIT, (c) 2024 Joao Chrisostomo)."""
    import onnx
    from onnx import numpy_helper
    m = onnx.load(os.environ.get("ARTCNN_ONNX", "/tmp/ds/ArtCNN/ONNX/ArtCNN_C4F16.onnx"))
    init = {i.name: numpy_helper.to_array(i) for i in m.graph.initializer}
    out = []
    for n in m.graph.node:
        if n.op_type == "Conv":
            out.append((init[n.input[1]], init[n.input[2]]))
    assert [w.shape for w, _ in out] == [(c, i, 3, 3) for i, c in LAYERS], [w.shape for w, _ in out]
    return out


def export(params):
    ours = [(np.asarray(params[f"w{i}"]).transpose(3, 2, 0, 1), np.asarray(params[f"b{i}"])) for i in range(len(LAYERS))]
    a = flatten_layers(ours)
    b = flatten_layers(artcnn_layers())
    assert len(a) == len(b)
    n = len(a)
    lit = lambda v: (lambda t: (t if ("." in t or "e" in t) else t + ".0") + "f")("%.8g" % v)
    with open(os.path.join(ROOT, "src", "engine", "neural_weights.h"), "w") as f:
        f.write("// AUTO-GENERATED by tools/train_neural.py - MoonUp Neural SR v2 weights.\n#pragma once\n")
        f.write("namespace sw { namespace neural {\n")
        f.write(f"static const int kWeightCount = {n};\n")
        f.write("// MoonUp Neural (realistic games, residual over Catmull-Rom)\n")
        f.write(f"static const float kMoonUp[{n}] = {{{','.join(lit(v) for v in a)}}};\n")
        f.write("// ArtCNN C4F16 (anime / 2D, direct luma) - MIT License, Copyright (c) 2024 Joao Chrisostomo\n")
        f.write(f"static const float kArtCNN[{n}] = {{{','.join(lit(v) for v in b)}}};\n}}}}\n")
    with open(os.path.join(ROOT, "ui", "js", "neural_weights.js"), "w") as f:
        f.write("// AUTO-GENERATED by tools/train_neural.py - MoonUp Neural SR v2 + ArtCNN C4F16 (MIT) weights.\n")
        f.write("export const ARTCNN = [" + ",".join("%.8g" % v for v in b) + "];\n")
        f.write("export default [" + ",".join("%.8g" % v for v in a) + "];\n")
    print("exported", n, "weights per model")


def main():
    steps = int(sys.argv[1]) if len(sys.argv) > 1 else 12000
    t0 = time.time()
    X, Y = build_dataset()
    print(f"dataset {X.shape} in {time.time() - t0:.1f}s", flush=True)
    nval = 512
    Xv, Yv = X[:nval], Y[:nval]
    X, Y = X[nval:], Y[nval:]

    params = init_params(jax.random.PRNGKey(0))
    sched = optax.cosine_onecycle_schedule(steps, 2e-3, pct_start=0.05)
    opt = optax.chain(optax.clip_by_global_norm(1.0), optax.adam(sched))
    state = opt.init(params)

    @jax.jit
    def step(p, s, lr, hr):
        l, g = jax.value_and_grad(loss_fn)(p, lr, hr)
        u, s = opt.update(g, s, p)
        return optax.apply_updates(p, u), s, l

    fwd = jax.jit(forward)
    evalv = lambda p: np.concatenate([np.asarray(fwd(p, Xv[j:j + 128])) for j in range(0, nval, 128)])
    base_v = np.asarray(jax.jit(bicubic2x)(Xv))
    print(f"validation bicubic PSNR {psnr(base_v, Yv):.3f} dB", flush=True)
    B = 16
    for i in range(steps):
        idx = rng.integers(0, len(X), B)
        params, state, l = step(params, state, X[idx], Y[idx])
        if (i + 1) % 1000 == 0 or i == 0:
            print(f"step {i + 1:5d}  loss {float(l):.5f}  val PSNR {psnr(evalv(params), Yv):.3f} dB  ({time.time() - t0:.0f}s)", flush=True)
        if (i + 1) % 3000 == 0:
            export(params)
    export(params)
    print(f"final: bicubic {psnr(base_v, Yv):.3f} dB -> neural {psnr(evalv(params), Yv):.3f} dB")


if __name__ == "__main__":
    main()
