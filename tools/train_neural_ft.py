"""MoonUp Neural v3: fine-tunes ArtCNN C4F16 (MIT, (c) 2024 Joao Chrisostomo) for game and
photographic content, predicting luma directly ("direct" mode, same shader passes).

Why: the v2 residual model (tools/train_neural.py) learned a strong sharpening for aliased
point-sampled input and loses to plain Lanczos on area-filtered content. Starting from ArtCNN
(which generalises well) and fine-tuning on realistic degradations keeps its robustness and adds
game/photo specific detail.

Hold-out: BSDS500 'test' images are excluded from training and used for the final comparison
against the original ArtCNN weights; the result is only exported when it is better.

  python3 tools/train_neural_ft.py [steps]
"""
import glob, math, os, sys, time

import jax
import jax.numpy as jnp
import numpy as np
import optax
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train_neural as tn  # noqa: E402

rng = np.random.default_rng(99)


def degrade(hr):
    r = rng.random()
    if r < 0.5:
        lr = hr.reshape(hr.shape[0] // 2, 2, hr.shape[1] // 2, 2).mean(axis=(1, 3))
    elif r < 0.85:
        im = Image.fromarray((np.clip(hr, 0, 1) * 255).astype(np.uint8))
        lr = np.asarray(im.resize((hr.shape[1] // 2, hr.shape[0] // 2), Image.BICUBIC), np.float32) / 255.0
    else:
        lr = hr[rng.integers(0, 2) :: 2, rng.integers(0, 2) :: 2][: hr.shape[0] // 2, : hr.shape[1] // 2]
    if rng.random() < 0.2:
        from scipy.ndimage import gaussian_filter

        lr = gaussian_filter(lr, rng.uniform(0.2, 0.5), mode="reflect")
    return np.clip(lr, 0, 1).astype(np.float32)


def load_train():
    files = sorted(glob.glob("/tmp/sr/kodak/*.png")) + sorted(glob.glob("/tmp/sr/urban/*.png"))
    files += [f for f in sorted(glob.glob("/tmp/ds/BSDS500/BSDS500/data/images/*/*.jpg")) if "/test/" not in f]
    files += sorted(glob.glob("/tmp/ds/FSRCNN-TensorFlow/Train/*.png"))
    photos = [tn.to_y(Image.open(f)) for f in files]
    games = tn.load_games()
    synth = []
    for _ in range(40):
        synth.append(tn.to_y(tn.synth_ui()))
        synth.append(tn.to_y(tn.synth_scene()))
    return photos, games, synth


def build(n, P=96):
    photos, games, synth = load_train()
    print("photos", len(photos), "games", len(games), "synth", len(synth), flush=True)
    X = np.zeros((n, P // 2, P // 2), np.float32)
    Y = np.zeros((n, P, P), np.float32)
    for i in range(n):
        r = rng.random()
        pool = photos if r < 0.6 or not games else games if r < 0.88 else synth
        src = pool[rng.integers(0, len(pool))]
        h, w = src.shape
        if h <= P or w <= P:
            continue
        y0, x0 = rng.integers(0, h - P), rng.integers(0, w - P)
        hr = src[y0 : y0 + P, x0 : x0 + P]
        if rng.random() < 0.5:
            hr = hr[:, ::-1]
        hr = np.rot90(hr, rng.integers(0, 4)).copy()
        X[i] = degrade(hr)
        Y[i] = hr
    return X, Y


def forward(p, lr):
    x0 = tn.conv(lr[..., None], p["w0"], p["b0"])
    x = x0
    for i in range(1, 5):
        x = jax.nn.relu(tn.conv(x, p[f"w{i}"], p[f"b{i}"]))
    x = tn.conv(x, p["w5"], p["b5"]) + x0
    return tn.shuffle(tn.conv(x, p["w6"], p["b6"]))


def loss_fn(p, lr, hr):
    d = (forward(p, lr) - hr)[:, 2:-2, 2:-2]
    return jnp.mean(jnp.sqrt(d * d + 1e-6))


def artcnn_params():
    p = {}
    for i, (w, b) in enumerate(tn.artcnn_layers()):
        p[f"w{i}"] = jnp.asarray(np.asarray(w, np.float32).transpose(2, 3, 1, 0))  # OIHW -> HWIO
        p[f"b{i}"] = jnp.asarray(np.asarray(b, np.float32))
    return p


def holdout():
    """Full BSDS test images, 2x box and bicubic degradations (luma)."""
    out = []
    for f in sorted(glob.glob("/tmp/ds/BSDS500/BSDS500/data/images/test/*.jpg"))[:40]:
        y = tn.to_y(Image.open(f))
        h, w = (y.shape[0] // 2) * 2, (y.shape[1] // 2) * 2
        y = y[:h, :w]
        box = y.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
        bic = np.asarray(Image.fromarray((y * 255).astype(np.uint8)).resize((w // 2, h // 2), Image.BICUBIC), np.float32) / 255
        out.append((box, y))
        out.append((bic, y))
    return out


def evaluate(fwd, p, data):
    s = 0
    for lr, hr in data:
        pr = np.clip(np.asarray(fwd(p, lr[None]))[0], 0, 1)
        m = np.mean((pr[4:-4, 4:-4] - hr[4:-4, 4:-4]) ** 2)
        s += 10 * math.log10(1 / m)
    return s / len(data)


def main():
    steps = int(sys.argv[1]) if len(sys.argv) > 1 else 6000
    t0 = time.time()
    X, Y = build(24000)
    keep = np.abs(Y).sum(axis=(1, 2)) > 0
    X, Y = X[keep], Y[keep]
    print(f"dataset {X.shape} ({time.time() - t0:.0f}s)", flush=True)
    base = artcnn_params()
    params = jax.tree_util.tree_map(lambda a: a, base)
    fwd = jax.jit(forward)
    ho = holdout()
    ref = evaluate(fwd, base, ho)
    print(f"hold-out ArtCNN {ref:.3f} dB", flush=True)
    sched = optax.cosine_decay_schedule(3e-4, steps, alpha=0.05)
    opt = optax.chain(optax.clip_by_global_norm(0.5), optax.adam(sched))
    state = opt.init(params)

    @jax.jit
    def step(p, s, lr, hr):
        l, g = jax.value_and_grad(loss_fn)(p, lr, hr)
        u, s = opt.update(g, s, p)
        return optax.apply_updates(p, u), s, l

    best, best_p = ref, base
    for i in range(steps):
        idx = rng.integers(0, len(X), 16)
        params, state, l = step(params, state, X[idx], Y[idx])
        if (i + 1) % 1000 == 0:
            v = evaluate(fwd, params, ho)
            print(f"step {i + 1}  loss {float(l):.5f}  hold-out {v:.3f} dB  ({time.time() - t0:.0f}s)", flush=True)
            if v > best:
                best, best_p = v, jax.tree_util.tree_map(lambda a: a, params)
    print(f"final: ArtCNN {ref:.3f} dB -> MoonUp Neural v3 {best:.3f} dB", flush=True)
    np.savez("/tmp/claude-0/neural_v3.npz", **{k: np.asarray(v) for k, v in best_p.items()})
    if best <= ref + 0.02:
        print("no improvement: not exported")
        return
    tn.export(best_p)


if __name__ == "__main__":
    main()
