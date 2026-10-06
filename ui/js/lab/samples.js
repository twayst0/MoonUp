// Procedural test images rendered at game-like low resolutions (no bundled photos needed).
// Each returns { w, h, data: Float32Array(w*h*3) } with values in 0..1.

function img(w, h) {
  return { w, h, data: new Float32Array(w * h * 3) };
}

const clamp = (x, a, b) => Math.min(b, Math.max(a, x));
const mix = (a, b, t) => a + (b - a) * t;
const smooth = (e0, e1, x) => {
  const t = clamp((x - e0) / (e1 - e0), 0, 1);
  return t * t * (3 - 2 * t);
};

// ------------------------------------------------------------------ 3D scene (2x2 supersampled)
export function scene(w = 320, h = 180) {
  const out = img(w, h);
  const horizon = 0.56;
  const spheres = [
    { x: -1.4, z: 7, r: 0.9, c: [0.85, 0.32, 0.22] },
    { x: 1.1, z: 5.2, r: 0.7, c: [0.2, 0.55, 0.95] },
    { x: 0.1, z: 11, r: 1.3, c: [0.9, 0.85, 0.75] },
  ];
  const sun = [0.55, 0.42, -0.72];
  const sl = Math.hypot(...sun);
  const L = sun.map((v) => v / sl);

  const shade = (sx, sy) => {
    // camera at y=1.2 looking down -z (we use +z forward)
    const fov = 1.1;
    const dx = (sx - 0.5) * 2 * fov * (w / h);
    const dy = -(sy - horizon) * 2 * fov;
    const dz = 1;
    const dl = Math.hypot(dx, dy, dz);
    const d = [dx / dl, dy / dl, dz / dl];
    const o = [0, 1.2, 0];
    let best = 1e9, col = null;
    for (const s of spheres) {
      const c = [s.x, s.r, s.z];
      const oc = [o[0] - c[0], o[1] - c[1], o[2] - c[2]];
      const b = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
      const cc = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - s.r * s.r;
      const disc = b * b - cc;
      if (disc > 0) {
        const tt = -b - Math.sqrt(disc);
        if (tt > 0 && tt < best) {
          best = tt;
          const p = [o[0] + d[0] * tt, o[1] + d[1] * tt, o[2] + d[2] * tt];
          const n = [(p[0] - c[0]) / s.r, (p[1] - c[1]) / s.r, (p[2] - c[2]) / s.r];
          const diff = Math.max(0, n[0] * L[0] + n[1] * L[1] + n[2] * L[2]);
          const hv = [L[0] - d[0], L[1] - d[1], L[2] - d[2]];
          const hl = Math.hypot(...hv);
          const spec = Math.pow(Math.max(0, (n[0] * hv[0] + n[1] * hv[1] + n[2] * hv[2]) / hl), 48);
          const amb = 0.18 + 0.12 * n[1];
          col = s.c.map((v) => v * (amb + diff * 0.9) + spec * 0.8);
        }
      }
    }
    if (col) return col;
    if (d[1] < -0.002) {
      // ground plane y = 0
      const tt = -o[1] / d[1];
      const px = o[0] + d[0] * tt, pz = o[2] + d[2] * tt;
      const chk = (Math.floor(px * 0.9) + Math.floor(pz * 0.9)) & 1;
      let g = chk ? [0.78, 0.76, 0.7] : [0.16, 0.17, 0.2];
      // sphere shadows (approximate, from the sun direction)
      for (const s of spheres) {
        const ddx = px - (s.x - L[0] * s.r * 1.2), ddz = pz - (s.z - L[2] * s.r * 1.2);
        if (ddx * ddx + ddz * ddz < s.r * s.r * 0.8) g = g.map((v) => v * 0.45);
      }
      const fog = 1 - Math.exp(-tt * 0.045);
      const fogc = [0.62, 0.66, 0.74];
      return g.map((v, i) => mix(v, fogc[i], fog));
    }
    // sky + mountains
    const ex = sx * 9;
    const m1 = horizon - 0.06 - 0.07 * (0.5 + 0.5 * Math.sin(ex * 0.9 + 1.3)) - 0.03 * Math.sin(ex * 2.7);
    const m2 = horizon - 0.025 - 0.04 * (0.5 + 0.5 * Math.sin(ex * 1.7 + 0.2)) - 0.015 * Math.sin(ex * 5.1);
    if (sy > m2) return [0.27, 0.3, 0.36];
    if (sy > m1) return [0.42, 0.46, 0.55];
    const t = clamp(sy / horizon, 0, 1);
    let sky = [mix(0.12, 0.85, t * t), mix(0.25, 0.78, t * t), mix(0.55, 0.78, t)];
    const sdx = sx - 0.74, sdy = sy - 0.2;
    const sd = Math.hypot(sdx * (w / h), sdy);
    sky = sky.map((v) => v + 0.9 * (1 - smooth(0.028, 0.034, sd)) + 0.25 * Math.exp(-sd * 9));
    // power line (thin catenary), aliasing test
    const ly = 0.12 + 0.08 * Math.pow((sx - 0.45) * 2, 2);
    const lw = Math.abs(sy - ly) * h;
    if (lw < 0.6) sky = sky.map((v) => mix(v, 0.05, 1 - lw / 0.6));
    return sky;
  };

  const ss = [0.25, 0.75];
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      let r = 0, g = 0, b = 0;
      for (const oy of ss)
        for (const ox of ss) {
          const c = shade((x + ox) / w, (y + oy) / h);
          r += c[0];
          g += c[1];
          b += c[2];
        }
      const i = (y * w + x) * 3;
      out.data[i] = clamp(r / 4, 0, 1);
      out.data[i + 1] = clamp(g / 4, 0, 1);
      out.data[i + 2] = clamp(b / 4, 0, 1);
    }
  }
  return out;
}

// ------------------------------------------------------------------ game HUD with small text
export function ui(w = 320, h = 180) {
  const c = new OffscreenCanvas(w, h);
  const x = c.getContext("2d");
  const g = x.createLinearGradient(0, 0, 0, h);
  g.addColorStop(0, "#28323f");
  g.addColorStop(1, "#11151b");
  x.fillStyle = g;
  x.fillRect(0, 0, w, h);
  // scenery blocks
  x.fillStyle = "#3a4656";
  for (let i = 0; i < 9; i++) x.fillRect(i * 38 + 6, 70 + ((i * 37) % 23), 30, 120);
  x.fillStyle = "#e9b949";
  for (let i = 0; i < 9; i++) for (let j = 0; j < 5; j++) if ((i * 7 + j * 3) % 4 === 0) x.fillRect(i * 38 + 12 + (j % 2) * 10, 80 + j * 14 + ((i * 37) % 23), 5, 6);
  // health bar
  x.fillStyle = "rgba(0,0,0,0.55)";
  x.fillRect(8, 8, 112, 30);
  x.fillStyle = "#e5484d";
  x.fillRect(12, 24, 80, 6);
  x.strokeStyle = "#ffffff";
  x.lineWidth = 1;
  x.strokeRect(12.5, 24.5, 100, 6);
  x.fillStyle = "#ffffff";
  x.font = "600 9px Inter, sans-serif";
  x.fillText("HP  87 / 100", 12, 19);
  // minimap
  x.fillStyle = "rgba(0,0,0,0.55)";
  x.beginPath();
  x.arc(w - 30, 30, 22, 0, Math.PI * 2);
  x.fill();
  x.strokeStyle = "#9fb4c8";
  x.stroke();
  x.fillStyle = "#4fd1c5";
  x.beginPath();
  x.moveTo(w - 30, 24);
  x.lineTo(w - 26, 34);
  x.lineTo(w - 34, 34);
  x.fill();
  x.fillStyle = "#f6c945";
  [[-12, -6], [8, 10], [14, -12]].forEach(([dx, dy]) => x.fillRect(w - 30 + dx, 30 + dy, 2, 2));
  // quest text
  x.fillStyle = "rgba(0,0,0,0.5)";
  x.fillRect(8, h - 44, 168, 36);
  x.fillStyle = "#f2f2f2";
  x.font = "600 10px Inter, sans-serif";
  x.fillText("Reach the signal tower", 13, h - 30);
  x.fillStyle = "#b9c3cf";
  x.font = "8px Inter, sans-serif";
  x.fillText("240 m · Avoid the patrol route", 13, h - 17);
  // ammo
  x.fillStyle = "#ffffff";
  x.font = "700 18px 'JetBrains Mono', monospace";
  x.textAlign = "right";
  x.fillText("30", w - 44, h - 14);
  x.font = "10px 'JetBrains Mono', monospace";
  x.fillStyle = "#9aa6b2";
  x.fillText("/ 120", w - 12, h - 14);
  // crosshair
  x.strokeStyle = "#ffffff";
  x.beginPath();
  x.moveTo(w / 2 - 6, h / 2 + 0.5);
  x.lineTo(w / 2 - 2, h / 2 + 0.5);
  x.moveTo(w / 2 + 2, h / 2 + 0.5);
  x.lineTo(w / 2 + 6, h / 2 + 0.5);
  x.moveTo(w / 2 + 0.5, h / 2 - 6);
  x.lineTo(w / 2 + 0.5, h / 2 - 2);
  x.moveTo(w / 2 + 0.5, h / 2 + 2);
  x.lineTo(w / 2 + 0.5, h / 2 + 6);
  x.stroke();
  return fromCanvas(x, w, h);
}

// ------------------------------------------------------------------ pixel art
const SPRITE = [
  "....2222....",
  "...222222...",
  "...33313....",
  "..3133113...",
  "..31133113..",
  "..3311113...",
  "....11111...",
  "...224422...",
  "..2224422...",
  "..1124421...",
  "....44.44...",
  "...55...55..",
];
const PAL = { 1: "#f1c27d", 2: "#d63f3f", 3: "#5b3a1e", 4: "#3157c4", 5: "#4a2c14" };

export function pixel(w = 160, h = 90) {
  const c = new OffscreenCanvas(w, h);
  const x = c.getContext("2d");
  x.imageSmoothingEnabled = false;
  const bands = ["#6fb7ff", "#7cc0ff", "#8ac8ff", "#9bd1ff", "#acd9ff"];
  bands.forEach((b, i) => {
    x.fillStyle = b;
    x.fillRect(0, i * 12, w, 12);
  });
  x.fillStyle = "#ffffff";
  [[18, 10], [90, 6], [128, 16]].forEach(([cx, cy]) => {
    x.fillRect(cx, cy + 2, 18, 4);
    x.fillRect(cx + 3, cy, 10, 2);
    x.fillRect(cx + 2, cy + 6, 14, 2);
  });
  // hills
  x.fillStyle = "#3f9b4f";
  for (let i = 0; i < w; i++) {
    const hh = Math.round(10 + 6 * Math.sin(i / 9) + 3 * Math.sin(i / 4.3));
    x.fillRect(i, 62 - hh, 1, hh);
  }
  // ground tiles
  for (let tx = 0; tx < w; tx += 8) {
    x.fillStyle = "#58b947";
    x.fillRect(tx, 62, 8, 3);
    x.fillStyle = "#9b6a3c";
    x.fillRect(tx, 65, 8, 25);
    x.fillStyle = "#7d522c";
    x.fillRect(tx + 1, 70, 2, 2);
    x.fillRect(tx + 5, 78, 2, 2);
    x.fillRect(tx + 3, 85, 1, 1);
  }
  // brick platform
  for (let bx = 92; bx < 140; bx += 8) {
    x.fillStyle = "#c8643c";
    x.fillRect(bx, 40, 8, 8);
    x.fillStyle = "#7c3216";
    x.fillRect(bx, 43, 8, 1);
    x.fillRect(bx + ((bx / 8) % 2 ? 2 : 5), 40, 1, 3);
    x.fillRect(bx + ((bx / 8) % 2 ? 5 : 2), 44, 1, 4);
  }
  // coins
  [[100, 30], [112, 30], [124, 30]].forEach(([cx, cy]) => {
    x.fillStyle = "#ffd23f";
    x.fillRect(cx + 1, cy, 3, 6);
    x.fillRect(cx, cy + 1, 5, 4);
    x.fillStyle = "#c99a00";
    x.fillRect(cx + 2, cy + 1, 1, 4);
  });
  // character
  SPRITE.forEach((row, sy) =>
    [...row].forEach((ch, sx) => {
      if (ch === ".") return;
      x.fillStyle = PAL[ch];
      x.fillRect(40 + sx, 50 + sy, 1, 1);
    })
  );
  return fromCanvas(x, w, h);
}


// ------------------------------------------------------------------ town (flat-lit game frame)
// A small ray traced street: ground, box buildings with procedural facades, trees, hills and
// sky. Lighting is deliberately "game-like" (direct sun + flat ambient, no occlusion), which is
// what MoonUp Neural Render is trained to deepen.
function hash2(x, y) {
  let h = (x * 374761393 + y * 668265263) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967295;
}
function vnoise(x, y) {
  const xi = Math.floor(x), yi = Math.floor(y), fx = x - xi, fy = y - yi;
  const u = fx * fx * (3 - 2 * fx), v = fy * fy * (3 - 2 * fy);
  return mix(mix(hash2(xi, yi), hash2(xi + 1, yi), u), mix(hash2(xi, yi + 1), hash2(xi + 1, yi + 1), u), v);
}
function fbm(x, y) {
  return vnoise(x, y) * 0.5 + vnoise(x * 2.1, y * 2.1) * 0.25 + vnoise(x * 4.3, y * 4.3) * 0.125 + vnoise(x * 8.7, y * 8.7) * 0.0625;
}

export function town(w = 480, h = 270) {
  const out = img(w, h);
  const boxes = [
    { x0: -9, x1: -4.2, z0: 14, z1: 20, hgt: 7, c: [0.72, 0.52, 0.42], kind: 0 },
    { x0: -4, x1: -1.6, z0: 17, z1: 22, hgt: 10, c: [0.82, 0.8, 0.74], kind: 1 },
    { x0: 2.2, x1: 6.5, z0: 12, z1: 17, hgt: 6, c: [0.66, 0.6, 0.55], kind: 0 },
    { x0: 6.8, x1: 11, z0: 15, z1: 24, hgt: 12, c: [0.55, 0.6, 0.68], kind: 1 },
    { x0: -14, x1: -9.5, z0: 22, z1: 30, hgt: 14, c: [0.78, 0.7, 0.6], kind: 1 },
    { x0: 0.8, x1: 1.6, z0: 8, z1: 8.8, hgt: 1.2, c: [0.5, 0.36, 0.25], kind: 2 },
  ];
  const trees = [
    { x: -2.6, z: 10, r: 1.1 }, { x: 4.6, z: 9.5, r: 0.9 }, { x: -6.5, z: 11.5, r: 1.3 }, { x: 8.5, z: 12, r: 1.2 },
  ];
  const sun = [0.45, 0.7, -0.55];
  const sl = Math.hypot(...sun);
  const L = sun.map((v) => v / sl);
  const camY = 1.7, horizon = 0.5, fov = 0.75;
  for (let py = 0; py < h; py++)
    for (let px = 0; px < w; px++) {
      const dx = ((px + 0.5) / w - 0.5) * 2 * fov * (w / h);
      const dy = -((py + 0.5) / h - horizon) * 2 * fov;
      const dl = Math.hypot(dx, dy, 1);
      const d = [dx / dl, dy / dl, 1 / dl];
      let t = 1e9, n = null, base = null, hit = null;
      // ground
      if (d[1] < -1e-4) {
        const tg = camY / -d[1];
        if (tg < t) {
          t = tg; n = [0, 1, 0];
          const gx = d[0] * tg, gz = d[2] * tg;
          const road = Math.abs(gx - 0.3) < 1.6;
          const g = fbm(gx * 0.9, gz * 0.9);
          base = road ? [0.36 + g * 0.08, 0.35 + g * 0.08, 0.36 + g * 0.08] : [0.32 + g * 0.18, 0.48 + g * 0.2, 0.2 + g * 0.08];
          if (road && Math.abs(gx - 0.3) < 0.07 && Math.floor(gz * 0.8) % 2 === 0) base = [0.85, 0.82, 0.62];
          hit = [gx, 0, gz];
        }
      }
      // boxes (slab test)
      for (const b of boxes) {
        let tmin = 0, tmax = 1e9, nn = null;
        const o = [0, camY, 0];
        const lo = [b.x0, 0, b.z0], hi = [b.x1, b.hgt, b.z1];
        let ok = true;
        for (let a = 0; a < 3 && ok; a++) {
          if (Math.abs(d[a]) < 1e-6) { if (o[a] < lo[a] || o[a] > hi[a]) ok = false; continue; }
          let t0 = (lo[a] - o[a]) / d[a], t1 = (hi[a] - o[a]) / d[a];
          let sgn = -1;
          if (t0 > t1) { const tt = t0; t0 = t1; t1 = tt; sgn = 1; }
          if (t0 > tmin) { tmin = t0; nn = [0, 0, 0]; nn[a] = sgn * Math.sign(d[a]) * -1 * -1; nn[a] = d[a] > 0 ? -1 : 1; }
          tmax = Math.min(tmax, t1);
          if (tmin > tmax) ok = false;
        }
        if (ok && tmin > 0 && tmin < t && nn) {
          t = tmin; n = nn; hit = [d[0] * t, camY + d[1] * t, d[2] * t];
          const u = n[0] !== 0 ? hit[2] : hit[0];
          const v = hit[1];
          let c = b.c.slice();
          const grain = fbm(u * 3, v * 3) * 0.12;
          c = c.map((x) => x * (0.92 + grain));
          if (b.kind !== 2 && n[1] === 0) {
            // windows
            const wu = ((u % 1.6) + 1.6) % 1.6, wv = v % 2.2;
            if (v > 0.9 && wu > 0.45 && wu < 1.15 && wv > 0.7 && wv < 1.7) {
              const lit = hash2(Math.floor(u / 1.6), Math.floor(v / 2.2)) > 0.7;
              c = lit ? [0.95, 0.85, 0.55] : [0.25, 0.32, 0.4];
            }
            if (b.kind === 0 && ((Math.floor(v * 4) % 2 === 0 ? u : u + 0.25) * 2) % 1 < 0.05) c = c.map((x) => x * 0.8);
          }
          if (n[1] > 0) c = [0.35, 0.33, 0.33];
          base = c;
        }
      }
      // trees: spheres on trunks
      for (const tr of trees) {
        const c = [tr.x, tr.r + 1.2, tr.z];
        const oc = [-c[0], camY - c[1], -c[2]];
        const bq = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
        const cq = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - tr.r * tr.r;
        const disc = bq * bq - cq;
        if (disc > 0) {
          const tt = -bq - Math.sqrt(disc);
          if (tt > 0 && tt < t) {
            t = tt; hit = [d[0] * t, camY + d[1] * t, d[2] * t];
            n = [(hit[0] - c[0]) / tr.r, (hit[1] - c[1]) / tr.r, (hit[2] - c[2]) / tr.r];
            const f = fbm(hit[0] * 4 + 10, hit[1] * 4 + hit[2] * 2);
            base = [0.16 + f * 0.2, 0.36 + f * 0.25, 0.12 + f * 0.08];
          }
        }
        // trunk
        const tx = tr.x, tz = tr.z;
        const denom = d[2];
        const tt2 = tz / denom;
        if (tt2 > 0 && tt2 < t) {
          const hx = d[0] * tt2, hy = camY + d[1] * tt2;
          if (Math.abs(hx - tx) < 0.15 && hy >= 0 && hy < 1.3) { t = tt2; n = [0, 0, -1]; base = [0.35, 0.25, 0.16]; hit = [hx, hy, tz]; }
        }
      }
      let col;
      if (!base) {
        // sky with hills
        const sy = (py + 0.5) / h;
        const az = dx;
        const hill = horizon - 0.06 - 0.05 * Math.sin(az * 3.1) - 0.03 * Math.sin(az * 7.3 + 1);
        if (sy > hill) col = [0.42, 0.5, 0.55].map((x, i) => x + fbm(az * 6, sy * 6) * 0.06);
        else {
          const k = sy / horizon;
          col = [mix(0.38, 0.75, k), mix(0.56, 0.84, k), mix(0.85, 0.93, k)];
          const sd = Math.hypot(px / w - 0.78, py / h - 0.16);
          col = col.map((x) => x + Math.max(0, 0.25 - sd) * 2);
        }
      } else {
        const nd = Math.max(0, n[0] * L[0] + n[1] * L[1] + n[2] * L[2]);
        const amb = 0.55;
        col = base.map((x) => x * (amb + 0.6 * nd));
        const fog = clamp(t / 60, 0, 0.6);
        col = col.map((x, i) => mix(x, [0.62, 0.72, 0.84][i], fog));
      }
      const o = (py * w + px) * 3;
      out.data[o] = clamp(col[0], 0, 1);
      out.data[o + 1] = clamp(col[1], 0, 1);
      out.data[o + 2] = clamp(col[2], 0, 1);
    }
  return out;
}

function fromCanvas(x, w, h) {
  const d = x.getImageData(0, 0, w, h).data;
  const out = img(w, h);
  for (let i = 0, j = 0; i < d.length; i += 4, j += 3) {
    out.data[j] = d[i] / 255;
    out.data[j + 1] = d[i + 1] / 255;
    out.data[j + 2] = d[i + 2] / 255;
  }
  return out;
}

export const SAMPLES = { scene, ui, pixel, town };
