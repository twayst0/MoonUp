// Split-view comparison canvas with a draggable divider and integer zoom.
import { SAMPLES } from "./samples.js";

let worker = null;
let seq = 0;
const waiting = new Map();
let weightsPromise = null;

export function process(msg) {
  if (!worker) {
    worker = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });
    worker.onmessage = (e) => {
      const p = waiting.get(e.data.id);
      waiting.delete(e.data.id);
      p && p(e.data.out);
    };
  }
  const id = ++seq;
  return new Promise((resolve) => {
    waiting.set(id, resolve);
    worker.postMessage({ ...msg, id });
  });
}

export function neuralWeights() {
  if (!weightsPromise)
    weightsPromise = import("../neural_weights.js")
      .then((m) => (m.default && m.default.length ? { neural: new Float32Array(m.default), anime: m.ARTCNN ? new Float32Array(m.ARTCNN) : null } : null))
      .catch(() => null);
  return weightsPromise;
}

const cache = new Map();
export function sample(name) {
  if (!cache.has(name)) cache.set(name, SAMPLES[name]());
  return cache.get(name);
}

export function toImageData(img) {
  const d = new ImageData(img.w, img.h);
  for (let i = 0, j = 0; j < img.data.length; i += 4, j += 3) {
    d.data[i] = img.data[j] * 255 + 0.5;
    d.data[i + 1] = img.data[j + 1] * 255 + 0.5;
    d.data[i + 2] = img.data[j + 2] * 255 + 0.5;
    d.data[i + 3] = 255;
  }
  return d;
}

export async function fromFile(file, maxW = 480, maxH = 270) {
  const bmp = await createImageBitmap(file);
  // Take a centred crop so the CPU preview stays responsive.
  const w = Math.min(bmp.width, maxW), h = Math.min(bmp.height, maxH);
  const c = new OffscreenCanvas(w, h);
  const x = c.getContext("2d");
  x.drawImage(bmp, (bmp.width - w) / 2, (bmp.height - h) / 2, w, h, 0, 0, w, h);
  const d = x.getImageData(0, 0, w, h).data;
  const out = { w, h, data: new Float32Array(w * h * 3) };
  for (let i = 0, j = 0; i < d.length; i += 4, j += 3) {
    out.data[j] = d[i] / 255;
    out.data[j + 1] = d[i + 1] / 255;
    out.data[j + 2] = d[i + 2] / 255;
  }
  return out;
}

// Compare view: two images of identical size drawn in one canvas, split at 'pos' (0..1).
export class CompareView {
  constructor(host) {
    this.host = host;
    this.canvas = host.querySelector("canvas");
    this.ctx = this.canvas.getContext("2d");
    this.split = 0.5;
    this.zoom = 1;
    this.panX = 0.5;
    this.panY = 0.5;
    this.left = null;
    this.right = null;
    this.handle = host.querySelector(".cv-handle");
    this.ro = new ResizeObserver(() => this.draw());
    this.ro.observe(host);
    host.addEventListener("pointerdown", (e) => {
      const r = host.getBoundingClientRect();
      const onHandle = Math.abs(e.clientX - (r.left + this.split * r.width)) < 14;
      host.setPointerCapture(e.pointerId);
      const startX = e.clientX, startY = e.clientY, px = this.panX, py = this.panY;
      const move = (ev) => {
        if (onHandle || this.zoom === 1) {
          this.split = Math.min(1, Math.max(0, (ev.clientX - r.left) / r.width));
        } else {
          this.panX = Math.min(1, Math.max(0, px - (ev.clientX - startX) / (r.width * this.zoom)));
          this.panY = Math.min(1, Math.max(0, py - (ev.clientY - startY) / (r.height * this.zoom)));
        }
        this.draw();
      };
      if (onHandle || this.zoom === 1) move(e);
      const up = () => {
        host.removeEventListener("pointermove", move);
        host.removeEventListener("pointerup", up);
      };
      host.addEventListener("pointermove", move);
      host.addEventListener("pointerup", up);
    });
  }

  set(left, right) {
    this.left = left ? this.bitmap(left) : null;
    this.right = right ? this.bitmap(right) : null;
    this.draw();
  }

  bitmap(img) {
    const c = new OffscreenCanvas(img.w, img.h);
    c.getContext("2d").putImageData(toImageData(img), 0, 0);
    return c;
  }

  setZoom(z) {
    this.zoom = z;
    this.draw();
  }

  draw() {
    const r = this.host.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const W = Math.round(r.width * dpr), H = Math.round(r.height * dpr);
    if (this.canvas.width !== W || this.canvas.height !== H) {
      this.canvas.width = W;
      this.canvas.height = H;
    }
    const ctx = this.ctx;
    ctx.fillStyle = "#060607";
    ctx.fillRect(0, 0, W, H);
    const img = this.left || this.right;
    if (!img) return;
    // Fit, then zoom around the pan centre. Nearest filtering so pixels are shown honestly.
    const fit = Math.min(W / img.width, H / img.height);
    const s = fit * this.zoom;
    const dw = img.width * s, dh = img.height * s;
    const ox = (W - dw) / 2 - (this.panX - 0.5) * dw * (this.zoom > 1 ? 1 : 0);
    const oy = (H - dh) / 2 - (this.panY - 0.5) * dh * (this.zoom > 1 ? 1 : 0);
    ctx.imageSmoothingEnabled = false;
    const cut = Math.round(this.split * W);
    if (this.left) {
      ctx.save();
      ctx.beginPath();
      ctx.rect(0, 0, cut, H);
      ctx.clip();
      ctx.drawImage(this.left, ox, oy, dw, dh);
      ctx.restore();
    }
    if (this.right) {
      ctx.save();
      ctx.beginPath();
      ctx.rect(cut, 0, W - cut, H);
      ctx.clip();
      ctx.drawImage(this.right, ox, oy, dw, dh);
      ctx.restore();
    }
    if (this.handle) this.handle.style.left = `${this.split * 100}%`;
  }

  destroy() {
    this.ro.disconnect();
  }
}
