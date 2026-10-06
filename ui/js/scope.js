// Frame-time oscilloscope: fixed grid with reference lines, crisp 1px trace.
export class Scope {
  constructor(canvas) {
    this.c = canvas;
    this.ctx = canvas?.getContext("2d");
    this.values = [];
    this.max = 33.3;
  }

  colors() {
    const css = getComputedStyle(document.documentElement);
    return {
      grid: css.getPropertyValue("--line").trim(),
      ref: css.getPropertyValue("--line-3").trim(),
      text: css.getPropertyValue("--t4").trim(),
      trace: css.getPropertyValue("--accent-hi").trim(),
      mono: css.getPropertyValue("--mono").trim(),
    };
  }

  draw(values) {
    if (!this.ctx) return;
    if (values) this.values = values;
    const r = this.c.getBoundingClientRect();
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.round(r.width)), h = Math.max(1, Math.round(r.height));
    if (this.c.width !== w * dpr || this.c.height !== h * dpr) {
      this.c.width = w * dpr;
      this.c.height = h * dpr;
    }
    const ctx = this.ctx;
    const col = this.colors();
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    // Vertical scale snaps to 16.7 / 33.3 / 50 ms so the grid stays stable.
    const peak = this.values.length ? Math.max(...this.values) : 0;
    const target = peak > 40 ? 66.7 : peak > 28 ? 50 : 33.3;
    this.max += (target - this.max) * 0.25;
    const padR = 44;
    const gw = w - padR;
    const y = (ms) => Math.round(h - 8 - (Math.min(ms, this.max) / this.max) * (h - 18)) + 0.5;

    ctx.lineWidth = 1;
    ctx.strokeStyle = col.grid;
    ctx.beginPath();
    for (let x = 0; x <= gw; x += gw / 12) {
      ctx.moveTo(Math.round(x) + 0.5, 0);
      ctx.lineTo(Math.round(x) + 0.5, h);
    }
    ctx.stroke();

    ctx.font = `10px ${col.mono || "monospace"}`;
    ctx.textBaseline = "middle";
    for (const ms of [8.3, 16.7, 33.3]) {
      if (ms > this.max) continue;
      const yy = y(ms);
      ctx.strokeStyle = col.ref;
      ctx.setLineDash([2, 3]);
      ctx.beginPath();
      ctx.moveTo(0, yy);
      ctx.lineTo(gw, yy);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = col.text;
      ctx.fillText(ms.toFixed(1), gw + 8, yy);
    }

    const v = this.values;
    if (v.length < 2) return;
    ctx.strokeStyle = col.trace;
    ctx.lineWidth = 1.25;
    ctx.lineJoin = "miter";
    ctx.beginPath();
    v.forEach((ms, i) => {
      const x = (i / (v.length - 1)) * gw;
      i ? ctx.lineTo(x, y(ms)) : ctx.moveTo(x, y(ms));
    });
    ctx.stroke();
  }
}

// Smoothly animates a number shown in an element.
export class Ticker {
  constructor(el, digits = 0, suffix = "") {
    this.el = el;
    this.digits = digits;
    this.suffix = suffix;
    this.value = null;
    this.target = null;
  }
  set(v) {
    if (v == null || !isFinite(v)) {
      this.value = this.target = null;
      this.el.innerHTML = `—`;
      this.el.classList.add("dim");
      return;
    }
    this.el.classList.remove("dim");
    this.target = v;
    if (this.value == null) this.value = v;
    if (!this.raf) this.tick();
  }
  tick() {
    this.value += (this.target - this.value) * 0.22;
    if (Math.abs(this.target - this.value) < Math.pow(10, -this.digits) / 2) this.value = this.target;
    this.el.innerHTML = this.value.toFixed(this.digits) + this.suffix;
    this.raf = this.value !== this.target ? requestAnimationFrame(() => this.tick()) : null;
  }
}
