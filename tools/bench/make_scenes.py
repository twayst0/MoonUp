"""Synthetic game-like test sequences with exact ground truth for frame generation.

Every scene is rendered at twice the 'game' frame rate: even frames are what the game would
show, odd frames are the exact in-between images (t = 0.5) used as ground truth.
Rendering is 2x supersampled so sub-pixel motion is exact.

  python3 make_scenes.py <outdir> [frames]
"""
import os, sys, math
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import map_coordinates

W, H = 960, 540
SS = 2
KODAK = '/tmp/sr/kodak'
rng = np.random.default_rng(7)


def tex(i):
    return np.asarray(Image.open(f'{KODAK}/{i:02d}.png').convert('RGB'), np.float32) / 255.0


def sample(img, x, y, wrap=True):
    """Bilinear sample img (h,w,3) at float coords (same shape arrays)."""
    h, w = img.shape[:2]
    if wrap:
        x = np.mod(x, w); y = np.mod(y, h)
    out = np.empty(x.shape + (3,), np.float32)
    for c in range(3):
        out[..., c] = map_coordinates(img[..., c], [y, x], order=1, mode='grid-wrap' if wrap else 'nearest')
    return out


def grid():
    ys, xs = np.mgrid[0:H * SS, 0:W * SS].astype(np.float32)
    return (xs + 0.5) / SS, (ys + 0.5) / SS


def down(img):
    return img.reshape(H, SS, W, SS, 3).mean(axis=(1, 3))


def comp(dst, src, alpha):
    a = alpha[..., None]
    return dst * (1 - a) + src * a


FONT = None
def hud_layer():
    """Static HUD: panel, text, health bar, minimap frame, crosshair. Returns rgb, alpha at SS res."""
    global FONT
    im = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    try:
        FONT = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 15 * SS)
    except Exception:
        FONT = ImageFont.load_default()
    d.rounded_rectangle([24 * SS, (H - 74) * SS, 300 * SS, (H - 22) * SS], 8 * SS, fill=(10, 14, 24, 170))
    d.text((38 * SS, (H - 68) * SS), 'HP 87 / 100   AMMO 24', font=FONT, fill=(235, 240, 250, 255))
    d.rectangle([38 * SS, (H - 40) * SS, 280 * SS, (H - 32) * SS], fill=(60, 20, 20, 255))
    d.rectangle([38 * SS, (H - 40) * SS, 248 * SS, (H - 32) * SS], fill=(90, 220, 120, 255))
    d.rectangle([(W - 150) * SS, 20 * SS, (W - 20) * SS, 150 * SS], outline=(230, 230, 230, 220), width=2 * SS)
    cx, cy = W // 2 * SS, H // 2 * SS
    for dx, dy in [(-12, 0), (12, 0), (0, -12), (0, 12)]:
        d.line([cx + dx * SS // 2, cy + dy * SS // 2, cx + dx * SS, cy + dy * SS], fill=(255, 255, 255, 230), width=SS * 2)
    a = np.asarray(im, np.float32) / 255.0
    return a[..., :3], a[..., 3]


def disc_mask(xs, ys, cx, cy, r, soft=1.0):
    d = np.sqrt((xs - cx) ** 2 + (ys - cy) ** 2)
    return np.clip((r - d) / soft + 0.5, 0, 1)


def rot_rect_mask(xs, ys, cx, cy, hw, hh, ang):
    c, s = math.cos(ang), math.sin(ang)
    u = (xs - cx) * c + (ys - cy) * s
    v = -(xs - cx) * s + (ys - cy) * c
    return np.clip(hw - np.abs(u) + 0.5, 0, 1) * np.clip(hh - np.abs(v) + 0.5, 0, 1), u, v


# ----------------------------------------------------------------------------------- scenes
def scene_parallax(t, X, Y, T, hud):
    far, mid, near = T[0], T[1], T[2]
    img = sample(far, X * 0.8 + 60 * t, Y * 0.8 + 40)
    img = img * 0.85 + 0.05
    # mid hills
    hill = 300 + 50 * np.sin((X + 160 * t) * 0.012) + 25 * np.sin((X + 160 * t) * 0.031 + 1)
    m = np.clip(Y - hill + 0.5, 0, 1)
    img = comp(img, sample(mid, X + 160 * t, Y) * 0.8, m)
    # near pillars
    px = np.mod(X + 420 * t, 260)
    pm = np.clip(np.minimum(px - 180, 230 - px) + 0.5, 0, 1) * (Y > 120)
    img = comp(img, sample(near, X + 420 * t, Y * 1.0) * 0.6, pm)
    # sprites
    for k in range(4):
        cx = (130 + k * 230 + 260 * t * (1 if k % 2 else -1)) % (W + 120) - 60
        cy = 230 + 70 * math.sin(2.1 * t + k)
        mk, u, v = rot_rect_mask(X, Y, cx, cy, 34, 22, 1.4 * t + k)
        img = comp(img, sample(T[3 + k], u * 3 + 200, v * 3 + 200), mk)
    return comp(img, hud[0], hud[1])


def scene_fps(t, X, Y, T, hud):
    yaw = 0.35 * math.sin(t * 0.9) + 0.25 * t
    fwd = 260 * t
    horizon = H * 0.42
    f = 520.0
    sky = np.stack([0.35 + 0.0 * Y, 0.5 + 0.0 * Y, 0.72 + 0.0 * Y], -1) - (Y / H)[..., None] * 0.25
    # distant mountains rotate with yaw only
    ang = np.arctan2(X - W / 2, f) + yaw
    mh = horizon - 40 - 30 * np.sin(ang * 5) - 15 * np.sin(ang * 13 + 2)
    mm = np.clip(Y - mh + 0.5, 0, 1)
    img = comp(sky, sample(T[4], ang * 400, Y * 1.5) * 0.7, mm)
    # floor
    dy = np.maximum(Y - horizon, 1e-3)
    depth = 60.0 * f / dy
    rx = (X - W / 2) / f * depth
    c, s = math.cos(yaw), math.sin(yaw)
    wx = rx * c + depth * s
    wz = -rx * s + depth * c + fwd
    floor = sample(T[5], wx * 0.6, wz * 0.6)
    fog = np.clip(depth / 4000.0, 0, 1)[..., None]
    floor = floor * (1 - fog) + np.array([0.55, 0.62, 0.72]) * fog
    fm = np.clip(Y - horizon, 0, 1)
    img = comp(img, floor, fm)
    return comp(img, hud[0], hud[1])


def scene_orbit(t, X, Y, T, hud):
    ang = 0.35 * t
    z = 1.0 + 0.15 * math.sin(t * 1.3)
    c, s = math.cos(ang), math.sin(ang)
    u = ((X - W / 2) * c + (Y - H / 2) * s) / z
    v = (-(X - W / 2) * s + (Y - H / 2) * c) / z
    img = sample(T[6], u + 380, v + 250)
    for k in range(5):
        a = 1.6 * t + k * 2 * math.pi / 5
        r = 170 + 30 * math.sin(t + k)
        cx, cy = W / 2 + r * math.cos(a), H / 2 + 0.6 * r * math.sin(a)
        mk = disc_mask(X, Y, cx, cy, 42 + 10 * math.sin(a))
        img = comp(img, sample(T[7 + k], X - cx + 300, Y - cy + 200) * 1.05, mk)
    return comp(img, hud[0], hud[1])


def scene_fast(t, X, Y, T, hud):
    img = sample(T[8], X + 1500 * t, Y * 0.9 + 20)
    for k in range(3):
        cx = W * (0.25 + 0.25 * k)
        cy = (90 + 900 * t * (1 + 0.3 * k)) % (H + 140) - 70
        mk, u, v = rot_rect_mask(X, Y, cx, cy, 50, 30, 0.0)
        img = comp(img, sample(T[9 + k], u * 2 + 100, v * 2 + 100), mk)
    return comp(img, hud[0], hud[1])


SCENES = {
    'parallax': (scene_parallax, [1, 3, 5, 7, 9, 11, 13], 1 / 60),
    'fps': (scene_fps, [2, 4, 6, 8, 10, 12], 1 / 45),
    'orbit': (scene_orbit, [15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 14, 12], 1 / 40),
    'fast': (scene_fast, [3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 24], 1 / 30),
}


def main():
    out = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 9
    only = sys.argv[3].split(',') if len(sys.argv) > 3 else list(SCENES)
    X, Y = grid()
    hud = hud_layer()
    for name in only:
        fn, texs, dt = SCENES[name]
        T = [tex(i) for i in texs] + [tex(i) for i in range(1, 25)]
        d = os.path.join(out, name)
        os.makedirs(d, exist_ok=True)
        for i in range(n):
            t = i * dt / 2  # odd frames are exact midpoints
            img = down(fn(t, X, Y, T, hud))
            Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).save(f'{d}/{i:04d}.png')
        print(name, 'done')


if __name__ == '__main__':
    main()
