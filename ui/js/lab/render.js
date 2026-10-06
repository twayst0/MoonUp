// CPU port of MoonUp Neural Render v3 for the live preview (same maths as shaders/render.hlsl
// and tools/render_ref.py). Images are { w, h, data: Float32Array rgb 0..1 }.

// 3x3 convolution, edge clamped, optional stride and dilation. x: HWC float array.
function conv(x, W, H, C, w, b, cout, stride, relu, dil = 1) {
  const ow = Math.floor((W - 1) / stride) + 1, oh = Math.floor((H - 1) / stride) + 1;
  const out = new Float32Array(ow * oh * cout);
  for (let y = 0; y < oh; y++)
    for (let xo = 0; xo < ow; xo++) {
      const o = (y * ow + xo) * cout;
      for (let co = 0; co < cout; co++) out[o + co] = b[co];
      for (let ky = 0; ky < 3; ky++) {
        const sy = Math.min(H - 1, Math.max(0, y * stride + (ky - 1) * dil));
        for (let kx = 0; kx < 3; kx++) {
          const sx = Math.min(W - 1, Math.max(0, xo * stride + (kx - 1) * dil));
          const p = (sy * W + sx) * C;
          for (let ci = 0; ci < C; ci++) {
            const v = x[p + ci];
            if (v === 0) continue;
            const base = (ci * 3 + ky) * 3 + kx;
            for (let co = 0; co < cout; co++) out[o + co] += v * w[co * C * 9 + base];
          }
        }
      }
      if (relu) for (let co = 0; co < cout; co++) if (out[o + co] < 0) out[o + co] = 0;
    }
  return { data: out, w: ow, h: oh };
}

function lowres(src, LW, LH) {
  const out = new Float32Array(LW * LH * 3);
  const fx = src.w / LW, fy = src.h / LH;
  for (let y = 0; y < LH; y++)
    for (let x = 0; x < LW; x++) {
      const x0 = Math.floor(x * fx), x1 = Math.max(x0 + 1, Math.floor((x + 1) * fx));
      const y0 = Math.floor(y * fy), y1 = Math.max(y0 + 1, Math.floor((y + 1) * fy));
      let r = 0, g = 0, b = 0, n = 0;
      for (let yy = y0; yy < Math.min(y1, src.h); yy++)
        for (let xx = x0; xx < Math.min(x1, src.w); xx++) {
          const p = (yy * src.w + xx) * 3;
          r += src.data[p]; g += src.data[p + 1]; b += src.data[p + 2]; n++;
        }
      const o = (y * LW + x) * 3;
      out[o] = r / n; out[o + 1] = g / n; out[o + 2] = b / n;
    }
  return out;
}

const luma = (r, g, b) => r * 0.299 + g * 0.587 + b * 0.114;

// Tone branch: 256x144 input -> 16x9x8 bilateral grid of 3x4 transforms.
function toneGrid(src, g) {
  const C3 = 64, GW = 16, GH = 9, cells = GW * GH;
  const low = lowres(src, 256, 144);
  let f = conv(low, 256, 144, 3, g("c1w", 432), g("c1b", 16), 16, 2, true);
  f = conv(f.data, f.w, f.h, 16, g("c2w", 4608), g("c2b", 32), 32, 2, true);
  f = conv(f.data, f.w, f.h, 32, g("c3w", 32 * C3 * 9), g("c3b", C3), C3, 2, true);
  const f4 = conv(f.data, f.w, f.h, C3, g("c4w", C3 * C3 * 9), g("c4b", C3), C3, 2, true);
  let loc = conv(f4.data, GW, GH, C3, g("c5w", C3 * C3 * 9), g("c5b", C3), C3, 1, true);
  loc = conv(loc.data, GW, GH, C3, g("c6w", C3 * C3 * 9), g("c6b", C3), C3, 1, false);
  const mean = new Float32Array(C3);
  for (let i = 0; i < cells; i++) for (let c = 0; c < C3; c++) mean[c] += f4.data[i * C3 + c] / cells;
  const f1w = g("f1w", C3 * C3), f1b = g("f1b", C3), f2w = g("f2w", C3 * C3), f2b = g("f2b", C3);
  const hid = new Float32Array(C3), glob = new Float32Array(C3);
  for (let c = 0; c < C3; c++) {
    let s = f1b[c];
    for (let i = 0; i < C3; i++) s += f1w[c * C3 + i] * mean[i];
    hid[c] = Math.max(0, s);
  }
  for (let c = 0; c < C3; c++) {
    let s = f2b[c];
    for (let i = 0; i < C3; i++) s += f2w[c * C3 + i] * hid[i];
    glob[c] = s;
  }
  const ow = g("ow", C3 * 96), ob = g("ob", 96);
  const grid = new Float32Array(cells * 96);
  const fz = new Float32Array(C3);
  for (let cell = 0; cell < cells; cell++) {
    for (let i = 0; i < C3; i++) fz[i] = Math.max(0, loc.data[cell * C3 + i] + glob[i]);
    for (let k = 0; k < 96; k++) {
      let s = ob[k];
      for (let i = 0; i < C3; i++) s += ow[k * C3 + i] * fz[i];
      grid[cell * 96 + k] = s;
    }
  }
  return grid;
}

// Structure branch: half resolution luma -> (a, k) maps.
function structureMaps(src, g) {
  const { w, h } = src;
  const hw = Math.ceil(w / 2), hh = Math.ceil(h / 2);
  const half = new Float32Array(hw * hh);
  for (let y = 0; y < hh; y++)
    for (let x = 0; x < hw; x++) {
      let s = 0;
      for (let j = 0; j < 2; j++)
        for (let i = 0; i < 2; i++) {
          const p = (Math.min(h - 1, y * 2 + j) * w + Math.min(w - 1, x * 2 + i)) * 3;
          s += luma(src.data[p], src.data[p + 1], src.data[p + 2]);
        }
      half[y * hw + x] = s / 4;
    }
  const DIL = [1, 2, 4, 8, 1], SC = 12;
  let f = { data: half };
  let cin = 1;
  for (let i = 0; i < 5; i++) {
    const n = i + 1;
    f = conv(f.data, hw, hh, cin, g(`s${n}w`, SC * cin * 9), g(`s${n}b`, SC), SC, 1, true, DIL[i]);
    cin = SC;
  }
  const m = conv(f.data, hw, hh, SC, g("s6w", 2 * SC * 9), g("s6b", 2), 2, 1, false, 1);
  return { data: m.data, w: hw, h: hh };
}

export function neuralRender(src, params, flat, off) {
  const g = (k, n) => flat.subarray(off[k], off[k] + n);
  const grid = toneGrid(src, g);
  const sm = structureMaps(src, g);
  const ccm = g("ccm", 9), ccmb = g("ccmb", 3), slopes = g("slopes", 48), mix = g("mix", 3), mixb = flat[off.mixb];
  const { w, h } = src;
  const out = { w, h, data: new Float32Array(w * h * 3) };
  const A = new Float32Array(12);
  const T = params.tone ?? 1.0, Cc = params.color ?? 0.8, S = params.structure ?? 1.0;
  const L = new Float32Array(w * h);
  for (let i = 0; i < w * h; i++) L[i] = luma(src.data[i * 3], src.data[i * 3 + 1], src.data[i * 3 + 2]);
  const LA = (x, y) => L[Math.min(h - 1, Math.max(0, y)) * w + Math.min(w - 1, Math.max(0, x))];
  for (let y = 0; y < h; y++) {
    const v = Math.min(8, Math.max(0, ((y + 0.5) / h) * 9 - 0.5));
    const v0 = Math.floor(v), v1 = Math.min(v0 + 1, 8), fv = v - v0;
    const sv = Math.min(sm.h - 1, Math.max(0, ((y + 0.5) / h) * sm.h - 0.5));
    const sv0 = Math.floor(sv), sv1 = Math.min(sv0 + 1, sm.h - 1), fsv = sv - sv0;
    for (let x = 0; x < w; x++) {
      const p = (y * w + x) * 3;
      const r = src.data[p], gg = src.data[p + 1], b = src.data[p + 2];
      let gd = mixb;
      for (let c = 0; c < 3; c++) {
        const yy = ccm[c * 3] * r + ccm[c * 3 + 1] * gg + ccm[c * 3 + 2] * b + ccmb[c];
        let cv = 0;
        for (let k = 0; k < 16; k++) { const d = yy - k / 16; if (d > 0) cv += d * slopes[c * 16 + k]; }
        gd += cv * mix[c];
      }
      gd = Math.min(1, Math.max(0, gd));
      const u = Math.min(15, Math.max(0, ((x + 0.5) / w) * 16 - 0.5));
      const u0 = Math.floor(u), u1 = Math.min(u0 + 1, 15), fu = u - u0;
      const z = Math.min(7, Math.max(0, gd * 8 - 0.5));
      const z0 = Math.floor(z), z1 = Math.min(z0 + 1, 7), fzz = z - z0;
      for (let j = 0; j < 12; j++) {
        const at = (yy, xx, zz) => grid[(yy * 16 + xx) * 96 + zz * 12 + j];
        const s0 = (at(v0, u0, z0) * (1 - fu) + at(v0, u1, z0) * fu) * (1 - fv) + (at(v1, u0, z0) * (1 - fu) + at(v1, u1, z0) * fu) * fv;
        const s1 = (at(v0, u0, z1) * (1 - fu) + at(v0, u1, z1) * fu) * (1 - fv) + (at(v1, u0, z1) * (1 - fu) + at(v1, u1, z1) * fu) * fv;
        A[j] = s0 * (1 - fzz) + s1 * fzz;
      }
      const o0 = Math.min(1, Math.max(0, A[0] * r + A[1] * gg + A[2] * b + A[3]));
      const o1 = Math.min(1, Math.max(0, A[4] * r + A[5] * gg + A[6] * b + A[7]));
      const o2 = Math.min(1, Math.max(0, A[8] * r + A[9] * gg + A[10] * b + A[11]));
      const d0 = o0 - r, d1 = o1 - gg, d2 = o2 - b;
      const dl = luma(d0, d1, d2);
      // structure maps, bilinear (= GPU sampling of the half resolution texture)
      const su = Math.min(sm.w - 1, Math.max(0, ((x + 0.5) / w) * sm.w - 0.5));
      const su0 = Math.floor(su), su1 = Math.min(su0 + 1, sm.w - 1), fsu = su - su0;
      const smp = (c) => {
        const q = (yy, xx) => sm.data[(yy * sm.w + xx) * 2 + c];
        return (q(sv0, su0) * (1 - fsu) + q(sv0, su1) * fsu) * (1 - fsv) + (q(sv1, su0) * (1 - fsu) + q(sv1, su1) * fsu) * fsv;
      };
      const a = Math.min(1, Math.max(-1.5, smp(0)));
      const kk = Math.min(3, Math.max(-1, smp(1)));
      let hc = 0;
      for (let j = -1; j <= 1; j++) for (let i = -1; i <= 1; i++) hc += LA(x + i, y + j) * (i === 0 ? 2 : 1) * (j === 0 ? 2 : 1);
      const hp = L[y * w + x] - hc / 16;
      const sh = Math.exp(S * a), det = S * kk * hp;
      out.data[p] = Math.min(1, Math.max(0, (r + T * dl + Cc * (d0 - dl)) * sh + det));
      out.data[p + 1] = Math.min(1, Math.max(0, (gg + T * dl + Cc * (d1 - dl)) * sh + det));
      out.data[p + 2] = Math.min(1, Math.max(0, (b + T * dl + Cc * (d2 - dl)) * sh + det));
    }
  }
  return out;
}
