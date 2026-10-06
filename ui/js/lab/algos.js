// CPU ports of the MoonUp shaders (same maths as src/engine/shaders), used by the
// Compare lab and the Vision preview. Images: { w, h, data: Float32Array(w*h*3) }, 0..1.

const PI = Math.PI;
const clamp = (x, a, b) => (x < a ? a : x > b ? b : x);
const luma = (r, g, b) => 0.299 * r + 0.587 * g + 0.114 * b;

function sinc(x) {
  x *= PI;
  return Math.abs(x) < 1e-4 ? 1 : Math.sin(x) / x;
}
function lanczos(x, a) {
  x = Math.abs(x);
  return x < a ? sinc(x) * sinc(x / a) : 0;
}
function catmullRom(x) {
  x = Math.abs(x);
  const x2 = x * x, x3 = x2 * x;
  if (x < 1) return 1.5 * x3 - 2.5 * x2 + 1;
  if (x < 2) return -0.5 * x3 + 2.5 * x2 - 4 * x + 2;
  return 0;
}

function make(w, h) {
  return { w, h, data: new Float32Array(w * h * 3) };
}

function loader(src) {
  const { w, h, data } = src;
  return (x, y, c) => {
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    return data[(y * w + x) * 3 + c];
  };
}

// Bilinear sample with texel centres at i + 0.5 (uv in 0..1), clamp addressing.
function bilinear(src, u, v, out) {
  const { w, h, data } = src;
  const x = u * w - 0.5, y = v * h - 0.5;
  const x0 = Math.floor(x), y0 = Math.floor(y);
  const fx = x - x0, fy = y - y0;
  const xa = clamp(x0, 0, w - 1), xb = clamp(x0 + 1, 0, w - 1);
  const ya = clamp(y0, 0, h - 1), yb = clamp(y0 + 1, 0, h - 1);
  for (let c = 0; c < 3; c++) {
    const a = data[(ya * w + xa) * 3 + c], b = data[(ya * w + xb) * 3 + c];
    const d = data[(yb * w + xa) * 3 + c], e = data[(yb * w + xb) * 3 + c];
    out[c] = (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
  }
  return out;
}

function forEachDst(src, W, H, fn) {
  const out = make(W, H);
  const sx = src.w / W, sy = src.h / H;
  const px = [0, 0, 0];
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      fn((x + 0.5) * sx - 0.5, (y + 0.5) * sy - 0.5, px, x, y);
      const i = (y * W + x) * 3;
      out.data[i] = clamp(px[0], 0, 1);
      out.data[i + 1] = clamp(px[1], 0, 1);
      out.data[i + 2] = clamp(px[2], 0, 1);
    }
  }
  return out;
}

export function nearest(src, W, H) {
  const L = loader(src);
  return forEachDst(src, W, H, (fx, fy, px) => {
    const x = Math.floor(fx + 0.5), y = Math.floor(fy + 0.5);
    for (let c = 0; c < 3; c++) px[c] = L(x, y, c);
  });
}

export function bilinearUp(src, W, H) {
  return forEachDst(src, W, H, (fx, fy, px, x, y) => bilinear(src, (x + 0.5) / W, (y + 0.5) / H, px));
}

export function pixelArt(src, W, H) {
  const scale = [W / src.w, H / src.h];
  const tmp = [0, 0, 0];
  return forEachDst(src, W, H, (fx, fy, px, x, y) => {
    const p = [((x + 0.5) * src.w) / W, ((y + 0.5) * src.h) / H];
    const uv = p.map((v, i) => {
      const cell = Math.floor(v);
      const f = v - cell;
      const region = 0.5 - 0.5 / Math.max(scale[i], 1);
      const t = clamp((f - region) / Math.max(1 - 2 * region, 1e-4), 0, 1);
      return (cell + t) / (i ? src.h : src.w);
    });
    bilinear(src, uv[0], uv[1], tmp);
    px[0] = tmp[0];
    px[1] = tmp[1];
    px[2] = tmp[2];
  });
}

export function bicubic(src, W, H) {
  const L = loader(src);
  return forEachDst(src, W, H, (fx, fy, px) => {
    const ix = Math.floor(fx), iy = Math.floor(fy);
    const ax = fx - ix, ay = fy - iy;
    px[0] = px[1] = px[2] = 0;
    let ws = 0;
    for (let j = -1; j <= 2; j++) {
      const wy = catmullRom(j - ay);
      for (let i = -1; i <= 2; i++) {
        const w = catmullRom(i - ax) * wy;
        ws += w;
        for (let c = 0; c < 3; c++) px[c] += w * L(ix + i, iy + j, c);
      }
    }
    for (let c = 0; c < 3; c++) px[c] /= ws;
  });
}

export function lanczos3(src, W, H, antiRing = 0.85) {
  const L = loader(src);
  return forEachDst(src, W, H, (fx, fy, px) => {
    const ix = Math.floor(fx), iy = Math.floor(fy);
    const ax = fx - ix, ay = fy - iy;
    const mn = [1, 1, 1], mx = [0, 0, 0];
    px[0] = px[1] = px[2] = 0;
    let ws = 0;
    for (let j = -2; j <= 3; j++) {
      const wy = lanczos(j - ay, 3);
      for (let i = -2; i <= 3; i++) {
        const w = lanczos(i - ax, 3) * wy;
        ws += w;
        for (let c = 0; c < 3; c++) {
          const v = L(ix + i, iy + j, c);
          px[c] += w * v;
          if (i >= 0 && i <= 1 && j >= 0 && j <= 1) {
            mn[c] = Math.min(mn[c], v);
            mx[c] = Math.max(mx[c], v);
          }
        }
      }
    }
    for (let c = 0; c < 3; c++) {
      const r = px[c] / ws;
      px[c] = r + (clamp(r, mn[c], mx[c]) - r) * antiRing;
    }
  });
}

// MoonUp Edge (port of CSEdge).
export function edge(src, W, H, sens = 1, antiRing = 0.85) {
  const L = loader(src);
  const c = new Float32Array(48), l = new Float32Array(16);
  return forEachDst(src, W, H, (fx, fy, px) => {
    const ix = Math.floor(fx), iy = Math.floor(fy);
    const ax = fx - ix, ay = fy - iy;
    for (let j = 0; j < 4; j++)
      for (let i = 0; i < 4; i++) {
        const k = j * 4 + i;
        const r = L(ix - 1 + i, iy - 1 + j, 0), g = L(ix - 1 + i, iy - 1 + j, 1), b = L(ix - 1 + i, iy - 1 + j, 2);
        c[k * 3] = r;
        c[k * 3 + 1] = g;
        c[k * 3 + 2] = b;
        l[k] = luma(r, g, b);
      }
    const wq = [(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay];
    const cq = [[1, 1], [2, 1], [1, 2], [2, 2]];
    let jxx = 0, jxy = 0, jyy = 0;
    for (let k = 0; k < 4; k++) {
      const [x, y] = cq[k];
      const gx = l[y * 4 + x + 1] - l[y * 4 + x - 1];
      const gy = l[(y + 1) * 4 + x] - l[(y - 1) * 4 + x];
      jxx += wq[k] * gx * gx;
      jxy += wq[k] * gx * gy;
      jyy += wq[k] * gy * gy;
    }
    const tr = jxx + jyy, det = jxx * jyy - jxy * jxy;
    const disc = Math.sqrt(Math.max(tr * tr * 0.25 - det, 0));
    const l1 = tr * 0.5 + disc, l2 = Math.max(tr * 0.5 - disc, 0);
    let gxv, gyv;
    if (Math.abs(jxy) > 1e-7) {
      const n = Math.hypot(jxy, l1 - jxx);
      gxv = jxy / n;
      gyv = (l1 - jxx) / n;
    } else if (jxx >= jyy) {
      gxv = 1;
      gyv = 0;
    } else {
      gxv = 0;
      gyv = 1;
    }
    const exv = -gyv, eyv = gxv;
    const anis = (l1 - l2) / (l1 + l2 + 1e-6);
    const strength = Math.sqrt(l1);
    const edgeAmt = clamp(strength * 6 * sens, 0, 1) * anis * anis * anis;
    const along = 1 / (1 + 0.2 * edgeAmt), across = 1 + 0.1 * edgeAmt;
    const lobe = clamp(strength * 10, 0, 1);
    px[0] = px[1] = px[2] = 0;
    let ws = 0;
    for (let j = 0; j < 4; j++)
      for (let i = 0; i < 4; i++) {
        const dx = i - 1 - ax, dy = j - 1 - ay;
        const u = (dx * exv + dy * eyv) * along;
        const v = (dx * gxv + dy * gyv) * across;
        let w = lanczos(Math.sqrt(u * u + v * v), 2);
        w = Math.max(w, 0) + (w - Math.max(w, 0)) * lobe;
        const k = (j * 4 + i) * 3;
        px[0] += w * c[k];
        px[1] += w * c[k + 1];
        px[2] += w * c[k + 2];
        ws += w;
      }
    ws = Math.max(ws, 1e-4);
    for (let ch = 0; ch < 3; ch++) {
      const r = px[ch] / ws;
      const a = c[5 * 3 + ch], b = c[6 * 3 + ch], d = c[9 * 3 + ch], e = c[10 * 3 + ch];
      const mn = Math.min(a, b, d, e), mx = Math.max(a, b, d, e);
      px[ch] = r + (clamp(r, mn, mx) - r) * antiRing;
    }
  });
}

// Contrast adaptive sharpening (port of CSSharpen).
export function sharpen(src, amount) {
  if (amount <= 0.01) return src;
  const { w, h } = src;
  const L = loader(src);
  const out = make(w, h);
  const peak = -1 / (8 + (4 - 8) * clamp(amount, 0, 1));
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const P = (xx, yy) => [L(xx, yy, 0), L(xx, yy, 1), L(xx, yy, 2)];
      const a = P(x, y - 1), b = P(x - 1, y), c = P(x, y), d = P(x + 1, y), e = P(x, y + 1);
      const mn = [0, 1, 2].map((i) => Math.min(a[i], b[i], c[i], d[i], e[i]));
      const mx = [0, 1, 2].map((i) => Math.max(a[i], b[i], c[i], d[i], e[i]));
      const range = luma(...mx) - luma(...mn);
      const t = clamp((range - 0.012) / (0.05 - 0.012), 0, 1);
      const guard = t * t * (3 - 2 * t);
      const o = (y * w + x) * 3;
      for (let i = 0; i < 3; i++) {
        const amp = Math.sqrt(clamp(Math.min(mn[i], 1 - mx[i]) / Math.max(mx[i], 1e-4), 0, 1));
        const wgt = amp * peak * guard * clamp(amount * 4, 0, 1);
        out.data[o + i] = clamp((c[i] + (a[i] + b[i] + d[i] + e[i]) * wgt) / (1 + 4 * wgt), 0, 1);
      }
    }
  return out;
}

// Vision (port of CSDown4 + CSBlur + CSVision).
export function vision(src, v) {
  const { w, h } = src;
  const qw = Math.max(1, Math.floor(w / 4)), qh = Math.max(1, Math.floor(h / 4));
  let q = make(qw, qh);
  const tmp = [0, 0, 0];
  for (let y = 0; y < qh; y++)
    for (let x = 0; x < qw; x++) {
      const cx = (x * 4 + 2) / w, cy = (y * 4 + 2) / h, ox = 1 / w, oy = 1 / h;
      const acc = [0, 0, 0];
      for (const [dx, dy] of [[-ox, -oy], [ox, -oy], [-ox, oy], [ox, oy]]) {
        bilinear(src, cx + dx, cy + dy, tmp);
        acc[0] += tmp[0];
        acc[1] += tmp[1];
        acc[2] += tmp[2];
      }
      const i = (y * qw + x) * 3;
      q.data[i] = acc[0] / 4;
      q.data[i + 1] = acc[1] / 4;
      q.data[i + 2] = acc[2] / 4;
    }
  const blur = (img, dx, dy) => {
    const o = make(img.w, img.h);
    const K = [[0, 0.227027027], [1.3846153846, 0.3162162162], [-1.3846153846, 0.3162162162], [3.2307692308, 0.0702702703], [-3.2307692308, 0.0702702703]];
    for (let y = 0; y < img.h; y++)
      for (let x = 0; x < img.w; x++) {
        const u = (x + 0.5) / img.w, vv = (y + 0.5) / img.h;
        const acc = [0, 0, 0];
        for (const [off, wt] of K) {
          bilinear(img, u + (dx * off) / img.w, vv + (dy * off) / img.h, tmp);
          acc[0] += tmp[0] * wt;
          acc[1] += tmp[1] * wt;
          acc[2] += tmp[2] * wt;
        }
        const i = (y * img.w + x) * 3;
        o.data.set(acc, i);
      }
    return o;
  };
  q = blur(blur(q, 1, 0), 0, 1);
  const L = loader(src);
  const out = make(w, h);
  const soft = (x, k) => x / (1 + Math.abs(x) * k);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      let c = [L(x, y, 0), L(x, y, 1), L(x, y, 2)];
      bilinear(q, (x + 0.5) / w, (y + 0.5) / h, tmp);
      const Lc = luma(...c);
      const mid = 1 - Math.pow(Math.abs(2 * Lc - 1), 2);
      const boost = soft(Lc - luma(...tmp), 6) * v.clarity * 1.4 * mid;
      const n = [0, 1, 2].map((i) => (L(x + 1, y, i) + L(x - 1, y, i) + L(x, y + 1, i) + L(x, y - 1, i)) * 0.25);
      const micro = soft(Lc - luma(...n), 10) * v.detail * 1.2;
      c = c.map((ch) => ch + boost + micro);
      const s = c.map((ch) => ch * ch * (3 - 2 * ch));
      c = c.map((ch, i) => ch + (s[i] - ch) * v.contrast * 0.6);
      c = c.map((ch) => ch * (1 + v.brightness * 0.25));
      const L2 = luma(...c);
      const sat = Math.max(...c) - Math.min(...c);
      const vib = v.vibrance * (1 - sat) * 1.2;
      c = c.map((ch) => L2 + (ch - L2) * (1 + vib));
      c[0] *= 1 + v.warmth * 0.06;
      c[2] *= 1 - v.warmth * 0.06;
      const i = (y * w + x) * 3;
      out.data[i] = clamp(c[0], 0, 1);
      out.data[i + 1] = clamp(c[1], 0, 1);
      out.data[i + 2] = clamp(c[2], 0, 1);
    }
  return out;
}

// MoonUp Neural SR v2 / ArtCNN C4F16 (port of neural.hlsl). weights: Float32Array, per layer
// [cout][cin][3][3] then biases. direct: ArtCNN predicts luma, MoonUp predicts a residual.
const NN_LAYERS = [[1, 16], [16, 16], [16, 16], [16, 16], [16, 16], [16, 16], [16, 4]];
export function neural(src, W, H, weights, direct = false) {
  if (!weights) return edge(src, W, H);
  const { w, h } = src;
  const Y = new Float32Array(w * h);
  for (let i = 0; i < w * h; i++) Y[i] = luma(src.data[i * 3], src.data[i * 3 + 1], src.data[i * 3 + 2]);
  const conv = (inp, C, O, off, relu, skip) => {
    const out = new Float32Array(w * h * O);
    const bOff = off + O * C * 9;
    for (let y = 0; y < h; y++)
      for (let x = 0; x < w; x++) {
        const o0 = (y * w + x) * O;
        for (let o = 0; o < O; o++) out[o0 + o] = weights[bOff + o];
        for (let k = 0; k < 9; k++) {
          const sx = clamp(x + (k % 3) - 1, 0, w - 1), sy = clamp(y + Math.floor(k / 3) - 1, 0, h - 1);
          const p = (sy * w + sx) * C;
          for (let i = 0; i < C; i++) {
            const v = inp[p + i];
            if (v === 0) continue;
            for (let o = 0; o < O; o++) out[o0 + o] += weights[off + (o * C + i) * 9 + k] * v;
          }
        }
        if (skip) for (let o = 0; o < O; o++) out[o0 + o] += skip[o0 + o];
        if (relu) for (let o = 0; o < O; o++) if (out[o0 + o] < 0) out[o0 + o] = 0;
      }
    return out;
  };
  const offs = [];
  let off = 0;
  for (const [ci, co] of NN_LAYERS) { offs.push(off); off += co * ci * 9 + co; }
  const f0 = conv(Y, 1, 16, offs[0], false, null);
  let f = conv(f0, 16, 16, offs[1], true, null);
  f = conv(f, 16, 16, offs[2], true, null);
  f = conv(f, 16, 16, offs[3], true, null);
  f = conv(f, 16, 16, offs[4], true, null);
  f = conv(f, 16, 16, offs[5], false, f0);
  const r = conv(f, 16, 4, offs[6], false, null);
  const big = make(w * 2, h * 2);
  const L = loader(src);
  for (let y = 0; y < h * 2; y++)
    for (let x = 0; x < w * 2; x++) {
      const s = (y & 1) * 2 + (x & 1);
      const res = r[((y >> 1) * w + (x >> 1)) * 4 + s];
      const fx = (x + 0.5) * 0.5 - 0.5, fy = (y + 0.5) * 0.5 - 0.5;
      const ix = Math.floor(fx), iy = Math.floor(fy);
      const ax = fx - ix, ay = fy - iy;
      const base = [0, 0, 0];
      for (let j = -1; j <= 2; j++) {
        const wy = catmullRom(j - ay);
        for (let i = -1; i <= 2; i++) {
          const wgt = catmullRom(i - ax) * wy;
          for (let c = 0; c < 3; c++) base[c] += wgt * L(ix + i, iy + j, c);
        }
      }
      const yb = luma(base[0], base[1], base[2]);
      const d = (direct ? clamp(res, 0, 1) : yb + res) - yb;
      const o = (y * w * 2 + x) * 3;
      for (let c = 0; c < 3; c++) big.data[o + c] = clamp(base[c] + d, 0, 1);
    }
  if (W === w * 2 && H === h * 2) return big;
  return W > w * 2 ? edge(big, W, H) : bilinearUp(big, W, H);
}

// AMD FidelityFX Super Resolution 1 EASU (port of shaders/fsr.hlsl, MIT, (c) 2021 AMD).
export function fsrEasu(src, W, H) {
  const { w, h, data } = src;
  const out = make(W, H);
  const T = (x, y) => (clamp(y, 0, h - 1) * w + clamp(x, 0, w - 1)) * 3;
  const Lm = (i) => data[i + 2] * 0.5 + (data[i] * 0.5 + data[i + 1]);
  const sx = w / W, sy = h / H;
  const tapX = [0, 1, -1, 0, 0, -1, 1, 2, 2, 1, 1, 0];
  const tapY = [-1, -1, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2];
  const lenOf = (a, b, c, d, e, wgt, acc) => {
    const dc = d - c, cb = c - b;
    let lx = Math.max(Math.abs(dc), Math.abs(cb));
    lx = lx > 0 ? 1 / lx : 1e30;
    const dx = d - b;
    acc.dx += dx * wgt;
    lx = clamp(Math.abs(dx) * lx, 0, 1);
    acc.len += lx * lx * wgt;
    const ec = e - c, ca = c - a;
    let ly = Math.max(Math.abs(ec), Math.abs(ca));
    ly = ly > 0 ? 1 / ly : 1e30;
    const dy = e - a;
    acc.dy += dy * wgt;
    ly = clamp(Math.abs(dy) * ly, 0, 1);
    acc.len += ly * ly * wgt;
  };
  for (let oy = 0; oy < H; oy++)
    for (let ox = 0; ox < W; ox++) {
      const px = ox * sx + 0.5 * sx - 0.5, py = oy * sy + 0.5 * sy - 0.5;
      const fx = Math.floor(px), fy = Math.floor(py), ppx = px - fx, ppy = py - fy;
      const L = (dx, dy) => Lm(T(fx + dx, fy + dy));
      const b = L(0, -1), c = L(1, -1), e = L(-1, 0), f = L(0, 0), g = L(1, 0), hh = L(2, 0);
      const i = L(-1, 1), j = L(0, 1), k = L(1, 1), l = L(2, 1), n = L(0, 2), o = L(1, 2);
      const acc = { dx: 0, dy: 0, len: 0 };
      lenOf(b, e, f, g, j, (1 - ppx) * (1 - ppy), acc);
      lenOf(c, f, g, hh, k, ppx * (1 - ppy), acc);
      lenOf(f, i, j, k, n, (1 - ppx) * ppy, acc);
      lenOf(g, j, k, l, o, ppx * ppy, acc);
      let dirx = acc.dx, diry = acc.dy;
      const dr = dirx * dirx + diry * diry;
      if (dr < 1 / 32768) { dirx = 1; diry = 0; } else { const r = 1 / Math.sqrt(dr); dirx *= r; diry *= r; }
      let len = acc.len * 0.5; len *= len;
      const stretch = (dirx * dirx + diry * diry) / Math.max(Math.abs(dirx), Math.abs(diry));
      const l2x = 1 + (stretch - 1) * len, l2y = 1 - 0.5 * len;
      const lob = 0.5 + (0.25 - 0.04 - 0.5) * len, clp = 1 / lob;
      let aw = 0;
      const ac = [0, 0, 0], mn = [1, 1, 1], mx = [0, 0, 0];
      for (let t = 0; t < 12; t++) {
        const ix = T(fx + tapX[t], fy + tapY[t]);
        const offx = tapX[t] - ppx, offy = tapY[t] - ppy;
        let vx = offx * dirx + offy * diry, vy = -offx * diry + offy * dirx;
        vx *= l2x; vy *= l2y;
        const d2 = Math.min(vx * vx + vy * vy, clp);
        let wB = 0.4 * d2 - 1, wA = lob * d2 - 1;
        wB = (25 / 16) * wB * wB - (25 / 16 - 1);
        const wt = wB * wA * wA;
        for (let ch = 0; ch < 3; ch++) ac[ch] += data[ix + ch] * wt;
        aw += wt;
        if (t === 3 || t === 4 || t === 6 || t === 9) for (let ch = 0; ch < 3; ch++) { mn[ch] = Math.min(mn[ch], data[ix + ch]); mx[ch] = Math.max(mx[ch], data[ix + ch]); }
      }
      const oi = (oy * W + ox) * 3;
      for (let ch = 0; ch < 3; ch++) out.data[oi + ch] = clamp(Math.min(mx[ch], Math.max(mn[ch], ac[ch] / aw)), 0, 1);
    }
  return out;
}

// FSR 1 RCAS sharpening (denoise path). sharp: 0..1 -> 2..0 stops.
export function fsrRcas(src, sharp) {
  if (sharp <= 0.01) return src;
  const { w, h, data } = src;
  const out = make(w, h);
  const con = Math.pow(2, -2 * (1 - clamp(sharp, 0, 1)));
  const P = (x, y) => (clamp(y, 0, h - 1) * w + clamp(x, 0, w - 1)) * 3;
  const Lm = (i) => data[i + 2] * 0.5 + (data[i] * 0.5 + data[i + 1]);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const b = P(x, y - 1), d = P(x - 1, y), e = P(x, y), f = P(x + 1, y), hh = P(x, y + 1);
      const bL = Lm(b), dL = Lm(d), eL = Lm(e), fL = Lm(f), hL = Lm(hh);
      const rng = Math.max(bL, dL, eL, fL, hL) - Math.min(bL, dL, eL, fL, hL);
      let nz = Math.abs(0.25 * (bL + dL + fL + hL) - eL) / (rng > 0 ? rng : 1e-30);
      nz = -0.5 * clamp(nz, 0, 1) + 1;
      let lobe = -1;
      for (let c = 0; c < 3; c++) {
        const mn4 = Math.min(data[b + c], data[d + c], data[f + c], data[hh + c]);
        const mx4 = Math.max(data[b + c], data[d + c], data[f + c], data[hh + c]);
        const hitMin = Math.min(mn4, data[e + c]) / (4 * mx4);
        const hitMax = (1 - Math.max(mx4, data[e + c])) / (4 * mn4 - 4);
        const v = Math.max(-hitMin, hitMax);
        lobe = Math.max(lobe, isNaN(v) ? -1 : v);
      }
      lobe = Math.max(-(0.25 - 1 / 16), Math.min(lobe, 0)) * con * nz;
      const o = (y * w + x) * 3;
      for (let c = 0; c < 3; c++)
        out.data[o + c] = clamp((lobe * (data[b + c] + data[d + c] + data[hh + c] + data[f + c]) + data[e + c]) / (4 * lobe + 1), 0, 1);
    }
  return out;
}

export function run(name, src, W, H, opts = {}) {
  switch (name) {
    case "nearest": return nearest(src, W, H);
    case "bilinear": return bilinearUp(src, W, H);
    case "bicubic": return bicubic(src, W, H);
    case "lanczos": return lanczos3(src, W, H);
    case "pixel": return pixelArt(src, W, H);
    case "neural": return neural(src, W, H, opts.weights?.neural, true);
    case "anime": return neural(src, W, H, opts.weights?.anime, true);
    case "fsr": return fsrEasu(src, W, H);
    case "edge":
    default: return edge(src, W, H);
  }
}
