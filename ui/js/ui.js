// UI building blocks: segmented controls, toggles, sliders, selects, option cards, modals, toasts.
import { icon } from "./icons.js";
import { t, fmt } from "./i18n.js";
import { state, profile, setProfile, setSetting } from "./store.js";

// ------------------------------------------------------------------ value access
function getPath(obj, path) {
  return path.split(".").reduce((o, k) => (o == null ? undefined : o[k]), obj);
}
export function getValue(scope, path) {
  return getPath(scope === "settings" ? state.settings : profile(), path);
}
export function setValue(scope, path, v) {
  scope === "settings" ? setSetting(path, v) : setProfile(path, v);
}

export const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);

// ------------------------------------------------------------------ markup helpers
export function seg(path, options, { scope = "profile", full = false, cls = "" } = {}) {
  const v = getValue(scope, path);
  return `<div class="seg ${full ? "full" : ""} ${cls}" data-seg="${path}" data-scope="${scope}">
    <span class="thumb"></span>
    ${options.map((o) => `<button data-v="${esc(o.v)}" class="${String(o.v) === String(v) ? "on" : ""}" ${o.title ? `title="${esc(o.title)}"` : ""}>${esc(o.label)}</button>`).join("")}
  </div>`;
}

// Liquid-glass loading indicator with the MoonUp logo.
export function loader(text, { small = false } = {}) {
  return `<div class="sw-loader ${small ? "sm" : ""}" role="status" aria-live="polite">
    <div class="sw-orb"><span class="glass"></span><span class="ring"></span><img src="img/logo.png" alt=""></div>
    ${text ? `<div class="sw-loader-t">${esc(String(text).replace(/[.…]+$/, ""))}<span class="dots"></span></div>` : ""}
  </div>`;
}

export function toggle(path, { scope = "profile" } = {}) {
  return `<div class="toggle ${getValue(scope, path) ? "on" : ""}" role="switch" tabindex="0" data-toggle="${path}" data-scope="${scope}"></div>`;
}

export function slider(path, { scope = "profile", min = 0, max = 1, step = 0.01, digits = 2, center = false, suffix = "" } = {}) {
  const v = Number(getValue(scope, path) ?? min);
  return `<div class="slider-row">
    <div class="slider" data-slider="${path}" data-scope="${scope}" data-min="${min}" data-max="${max}" data-step="${step}" data-digits="${digits}" data-suffix="${esc(suffix)}" data-center="${center ? 1 : 0}">
      <div class="track"></div>${center ? '<div class="center-mark"></div>' : ""}<div class="fill"></div><div class="knob"></div>
    </div>
    <div class="val" data-val-for="${path}">${fmt(v, digits)}${suffix}</div>
  </div>`;
}

export function select(path, options, { scope = "profile", width = 220 } = {}) {
  const v = getValue(scope, path);
  const cur = options.find((o) => String(o.v) === String(v)) || options[0];
  return `<div class="select" data-select="${path}" data-scope="${scope}" style="width:${width}px">
    <button class="select-btn"><span class="cur">${esc(cur?.label ?? "")}</span>${icon("chev", "chev")}</button>
    <script type="application/json">${JSON.stringify(options).replace(/</g, "\\u003c")}</script>
  </div>`;
}

export function optCard(path, v, title, desc, { scope = "profile", extra = "", right = "", disabled = false } = {}) {
  const on = String(getValue(scope, path)) === String(v);
  return `<button class="choice ${on ? "on" : ""}" data-opt="${path}" data-scope="${scope}" data-v="${esc(v)}" ${disabled ? "disabled" : ""}>
    <span class="radio"></span>
    <span><span class="t">${title}</span>${desc ? `<span class="d">${desc}</span>` : ""}${extra}</span>
    <span class="r">${right}</span>
  </button>`;
}

export function choices(inner) {
  return `<div class="choices">${inner}</div>`;
}

export function costMeter(n, total = 5) {
  return `<span class="cost" title="${esc(t("cost"))}">${Array.from({ length: total }, (_, i) => `<i class="${i < n ? "f" : ""}"></i>`).join("")}</span>`;
}

export function setRow(title, desc, control, { wide = false } = {}) {
  return `<div class="set-row"><div class="lbl"><b>${title}</b>${desc ? `<span>${desc}</span>` : ""}</div><div class="ctl ${wide ? "wide" : ""}">${control}</div></div>`;
}

// ------------------------------------------------------------------ behaviour
function parseVal(raw, current) {
  if (typeof current === "number") return Number(raw);
  if (typeof current === "boolean") return raw === "true";
  return raw;
}

export function placeThumb(segEl) {
  const thumb = segEl.querySelector(".thumb");
  const on = segEl.querySelector("button.on");
  if (!thumb) return;
  if (!on) {
    thumb.style.width = "0px";
    return;
  }
  thumb.style.left = on.offsetLeft + "px";
  thumb.style.width = on.offsetWidth + "px";
}

function sliderSet(el, v, fire) {
  const min = +el.dataset.min, max = +el.dataset.max, step = +el.dataset.step;
  v = Math.min(max, Math.max(min, Math.round((v - min) / step) * step + min));
  v = +v.toFixed(4);
  const p = (v - min) / (max - min);
  const center = el.dataset.center === "1";
  el.querySelector(".knob").style.left = `${p * 100}%`;
  const fill = el.querySelector(".fill");
  if (center) {
    const c = 0.5;
    fill.style.left = `${Math.min(p, c) * 100}%`;
    fill.style.width = `${Math.abs(p - c) * 100}%`;
  } else {
    fill.style.left = "0";
    fill.style.width = `${p * 100}%`;
  }
  const lab = el.closest(".slider-row")?.querySelector(".val");
  if (lab) lab.textContent = fmt(v, +el.dataset.digits) + (el.dataset.suffix || "");
  if (fire) fire(v);
  return v;
}

export function bindControls(root, onChange = () => {}) {
  root.querySelectorAll("[data-seg]").forEach((el) => {
    requestAnimationFrame(() => placeThumb(el));
    el.addEventListener("click", (e) => {
      const b = e.target.closest("button");
      if (!b) return;
      const scope = el.dataset.scope, path = el.dataset.seg;
      const v = parseVal(b.dataset.v, getValue(scope, path));
      el.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x === b));
      placeThumb(el);
      setValue(scope, path, v);
      onChange(path, v, scope);
    });
  });

  root.querySelectorAll("[data-toggle]").forEach((el) => {
    const flip = () => {
      const scope = el.dataset.scope, path = el.dataset.toggle;
      const v = !getValue(scope, path);
      el.classList.toggle("on", v);
      setValue(scope, path, v);
      onChange(path, v, scope);
    };
    el.addEventListener("click", flip);
    el.addEventListener("keydown", (e) => (e.key === " " || e.key === "Enter") && (e.preventDefault(), flip()));
  });

  root.querySelectorAll("[data-slider]").forEach((el) => {
    const scope = el.dataset.scope, path = el.dataset.slider;
    sliderSet(el, Number(getValue(scope, path) ?? el.dataset.min));
    const commit = (v) => {
      setValue(scope, path, v);
      onChange(path, v, scope);
    };
    const fromEvent = (e) => {
      const r = el.getBoundingClientRect();
      const p = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width));
      return +el.dataset.min + p * (+el.dataset.max - +el.dataset.min);
    };
    el.addEventListener("pointerdown", (e) => {
      el.setPointerCapture(e.pointerId);
      el.classList.add("drag");
      sliderSet(el, fromEvent(e), commit);
      const move = (ev) => sliderSet(el, fromEvent(ev), commit);
      const up = () => {
        el.classList.remove("drag");
        el.removeEventListener("pointermove", move);
        el.removeEventListener("pointerup", up);
      };
      el.addEventListener("pointermove", move);
      el.addEventListener("pointerup", up);
    });
    el.addEventListener("dblclick", () => {
      if (el.dataset.center === "1") sliderSet(el, 0, commit);
    });
  });

  root.querySelectorAll("[data-select]").forEach((el) => {
    const options = JSON.parse(el.querySelector("script").textContent);
    const btn = el.querySelector(".select-btn");
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      const open = el.classList.contains("open");
      document.querySelectorAll(".select.open").forEach((s) => closeSelect(s));
      if (open) return;
      el.classList.add("open");
      const scope = el.dataset.scope, path = el.dataset.select;
      const cur = getValue(scope, path);
      const menu = document.createElement("div");
      menu.className = "select-menu";
      menu.innerHTML = options
        .map((o) => `<div class="select-opt ${String(o.v) === String(cur) ? "on" : ""}" data-v="${esc(o.v)}">${esc(o.label)}${o.hint ? `<span class="hint">${esc(o.hint)}</span>` : ""}</div>`)
        .join("");
      el.appendChild(menu);
      menu.addEventListener("click", (ev) => {
        const o = ev.target.closest(".select-opt");
        if (!o) return;
        const v = parseVal(o.dataset.v, cur);
        el.querySelector(".cur").textContent = options.find((x) => String(x.v) === o.dataset.v)?.label ?? "";
        closeSelect(el);
        setValue(scope, path, v);
        onChange(path, v, scope);
      });
    });
  });

  root.querySelectorAll("[data-opt]").forEach((el) => {
    el.addEventListener("click", () => {
      if (el.disabled) return;
      const scope = el.dataset.scope, path = el.dataset.opt;
      const v = parseVal(el.dataset.v, getValue(scope, path));
      root.querySelectorAll(`[data-opt="${path}"]`).forEach((x) => x.classList.toggle("on", x === el));
      setValue(scope, path, v);
      onChange(path, v, scope);
    });
  });
}

function closeSelect(el) {
  el.classList.remove("open");
  el.querySelector(".select-menu")?.remove();
}
document.addEventListener("click", () => document.querySelectorAll(".select.open").forEach(closeSelect));
window.addEventListener("resize", () => document.querySelectorAll("[data-seg]").forEach(placeThumb));

// ------------------------------------------------------------------ modal / toast
export function modal({ title, body, foot = "", width = 640, onMount, onClose } = {}) {
  const root = document.getElementById("modalRoot");
  const back = document.createElement("div");
  back.className = "modal-back";
  back.innerHTML = `<div class="modal" style="width:min(${width}px, calc(100vw - 80px))">
      <div class="modal-head"><h2>${title}</h2><span class="spacer"></span><button class="btn ghost icon" data-close aria-label="${esc(t("common.close"))}"><svg viewBox="0 0 10 10" style="width:10px;height:10px"><path d="M1.5 1.5l7 7M8.5 1.5l-7 7" stroke="currentColor" fill="none"/></svg></button></div>
      <div class="modal-body">${body}</div>${foot ? `<div class="modal-foot">${foot}</div>` : ""}</div>`;
  root.appendChild(back);
  const close = () => {
    if (back.classList.contains("closing")) return;
    back.classList.add("closing");
    setTimeout(() => back.remove(), 240);
    document.removeEventListener("keydown", onKey);
    onClose && onClose();
  };
  const onKey = (e) => e.key === "Escape" && close();
  document.addEventListener("keydown", onKey);
  back.addEventListener("mousedown", (e) => e.target === back && close());
  back.querySelectorAll("[data-close]").forEach((b) => b.addEventListener("click", close));
  onMount && onMount(back.querySelector(".modal"), close);
  return close;
}

export function confirmDialog(text, { okLabel, danger = false } = {}) {
  return new Promise((resolve) => {
    let result = false;
    modal({
      title: esc(text),
      width: 460,
      body: "",
      foot: `<button class="btn" data-close>${esc(t("common.cancel"))}</button><button class="btn ${danger ? "danger" : "primary"}" id="dlgOk">${esc(okLabel || t("common.ok"))}</button>`,
      onMount: (m, close) => m.querySelector("#dlgOk").addEventListener("click", () => { result = true; close(); }),
      onClose: () => resolve(result),
    });
  });
}

export function promptDialog(title, value = "", { multiline = false } = {}) {
  return new Promise((resolve) => {
    let result = null;
    modal({
      title: esc(title),
      width: 480,
      body: multiline
        ? `<textarea class="input" id="dlgIn" style="width:100%;height:180px;padding:10px;font-family:var(--font-mono);font-size:12px;resize:none">${esc(value)}</textarea>`
        : `<input class="input" id="dlgIn" style="width:100%" value="${esc(value)}" spellcheck="false">`,
      foot: `<button class="btn" data-close>${esc(t("common.cancel"))}</button><button class="btn primary" id="dlgOk">${esc(t("common.ok"))}</button>`,
      onMount: (m, close) => {
        const inp = m.querySelector("#dlgIn");
        setTimeout(() => { inp.focus(); inp.select?.(); }, 50);
        const ok = () => { result = inp.value; close(); };
        m.querySelector("#dlgOk").addEventListener("click", ok);
        if (!multiline) inp.addEventListener("keydown", (e) => e.key === "Enter" && ok());
      },
      onClose: () => resolve(result),
    });
  });
}

export function toast(text, kind = "") {
  const root = document.getElementById("toastRoot");
  const el = document.createElement("div");
  el.className = `toast ${kind}`;
  el.innerHTML = `<span class="ic"></span><span>${esc(text)}</span>`;
  root.appendChild(el);
  setTimeout(() => {
    el.classList.add("out");
    setTimeout(() => el.remove(), 260);
  }, Math.min(9000, 3400 + String(text).length * 30));
}
