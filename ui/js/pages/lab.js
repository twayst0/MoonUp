// Compare: run two upscalers on the same image and inspect them side by side.
import { t } from "../i18n.js";
import { state } from "../store.js";
import { esc, placeThumb } from "../ui.js";
import { icon } from "../icons.js";
import { CompareView, process, sample, fromFile, neuralWeights } from "../lab/viewer.js";

const ALGOS = ["neural", "anime", "edge", "fsr", "lanczos", "bicubic", "bilinear", "nearest", "pixel"];
const lab = { sample: "scene", left: "bilinear", right: "edge", factor: 3, zoom: 1, custom: null, sharp: 0.35 };
let view = null;

function segHtml(id, opts, cur) {
  return `<div class="seg" id="${id}"><span class="thumb"></span>${opts.map((o) => `<button data-v="${o.v}" class="${String(o.v) === String(cur) ? "on" : ""}">${esc(o.label)}</button>`).join("")}</div>`;
}

function algoSelect(id, cur) {
  const list = ALGOS.filter((a) => (a !== "neural" && a !== "anime") || state.neuralBundled);
  return `<select class="input lab-sel" id="${id}">${list.map((a) => `<option value="${a}" ${a === cur ? "selected" : ""}>${esc(t(`upscaler.${a}.name`))}</option>`).join("")}</select>`;
}

export function render(root) {
  root.innerHTML = `
  <div class="page-head"><div><h1>${esc(t("lab.title"))}</h1><p>${esc(t("lab.sub"))}</p></div></div>
  <div class="lab-bar">
    <div class="lab-field"><span class="label">${esc(t("lab.sample"))}</span>
      ${segHtml("labSample", [{ v: "scene", label: t("lab.sample.scene") }, { v: "ui", label: t("lab.sample.ui") }, { v: "pixel", label: t("lab.sample.pixel") }, { v: "town", label: t("lab.sample.town") }], lab.custom ? "" : lab.sample)}
      <label class="btn sm" title="${esc(t("lab.open"))}">${icon("folder")}<input type="file" accept="image/*" id="labFile" hidden></label></div>
    <div class="lab-field"><span class="label">${esc(t("lab.factor"))}</span>${segHtml("labFactor", [1.5, 2, 3, 4].map((f) => ({ v: f, label: f + "×" })), lab.factor)}</div>
    <div class="lab-field"><span class="label">${esc(t("lab.zoom"))}</span>${segHtml("labZoom", [1, 2, 4, 8].map((z) => ({ v: z, label: z + "×" })), lab.zoom)}</div>
  </div>
  <div class="cv" id="cv">
    <canvas></canvas><div class="cv-handle"><span></span></div>
    <div class="cv-tag l">${algoSelect("labLeft", lab.left)}</div>
    <div class="cv-tag r">${algoSelect("labRight", lab.right)}</div>
    <div class="cv-busy hidden" id="cvBusy">${esc(t("lab.rendering"))}</div>
  </div>
  <div class="lab-foot"><span class="faint" id="labInfo"></span><span class="spacer"></span><span class="faint">${esc(t("lab.drag"))}</span></div>`;

  view?.destroy();
  view = new CompareView(root.querySelector("#cv"));
  view.setZoom(lab.zoom);
  root.querySelectorAll(".seg").forEach((s) => requestAnimationFrame(() => placeThumb(s)));

  const segBind = (id, fn) => {
    const el = root.querySelector("#" + id);
    el.addEventListener("click", (e) => {
      const b = e.target.closest("button");
      if (!b) return;
      el.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x === b));
      placeThumb(el);
      fn(b.dataset.v);
    });
  };
  segBind("labSample", (v) => {
    lab.sample = v;
    lab.custom = null;
    refresh(root);
  });
  segBind("labFactor", (v) => {
    lab.factor = +v;
    refresh(root);
  });
  segBind("labZoom", (v) => {
    lab.zoom = +v;
    view.setZoom(lab.zoom);
  });
  root.querySelector("#labLeft").addEventListener("change", (e) => {
    lab.left = e.target.value;
    refresh(root);
  });
  root.querySelector("#labRight").addEventListener("change", (e) => {
    lab.right = e.target.value;
    refresh(root);
  });
  root.querySelector("#labFile").addEventListener("change", async (e) => {
    const f = e.target.files?.[0];
    if (!f) return;
    lab.custom = await fromFile(f);
    root.querySelectorAll("#labSample button").forEach((x) => x.classList.remove("on"));
    placeThumb(root.querySelector("#labSample"));
    refresh(root);
  });
  refresh(root);
}

let token = 0;
async function refresh(root) {
  const my = ++token;
  const src = lab.custom || sample(lab.sample);
  const W = Math.round(src.w * lab.factor), H = Math.round(src.h * lab.factor);
  root.querySelector("#cvBusy").classList.remove("hidden");
  root.querySelector("#labInfo").textContent = `${src.w}×${src.h} → ${W}×${H}`;
  const weights = await neuralWeights();
  const job = (name) => process({ op: "scale", src, W, H, name, sharp: name === "nearest" || name === "pixel" ? 0 : lab.sharp, weights });
  const [a, b] = await Promise.all([job(lab.left), job(lab.right)]);
  if (my !== token || !root.querySelector("#cv")) return;
  view.set(a, b);
  root.querySelector("#cvBusy").classList.add("hidden");
}

export function leave() {
  view?.destroy();
  view = null;
}
