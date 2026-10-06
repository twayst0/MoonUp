// Vision: image enhancement controls with a live before/after preview.
import { t } from "../i18n.js";
import { profile, setProfile } from "../store.js";
import { toggle, slider, setRow, bindControls, placeThumb, esc } from "../ui.js";
import { profileHead } from "./scaling.js";
import { CompareView, process, sample } from "../lab/viewer.js";

export const PRESETS = {
  natural: { clarity: 0.3, detail: 0.2, vibrance: 0.15, contrast: 0.08, warmth: 0, brightness: 0 },
  vivid: { clarity: 0.45, detail: 0.3, vibrance: 0.5, contrast: 0.2, warmth: 0.05, brightness: 0.03 },
  cinematic: { clarity: 0.35, detail: 0.15, vibrance: -0.1, contrast: 0.3, warmth: 0.25, brightness: -0.04 },
  crisp: { clarity: 0.55, detail: 0.6, vibrance: 0.1, contrast: 0.12, warmth: -0.1, brightness: 0 },
};
const KEYS = ["clarity", "detail", "vibrance", "contrast", "warmth", "brightness"];
let view = null;

export function render(root) {
  const v = profile().vision;
  root.innerHTML = `
  ${profileHead(t("vision.title"), t("vision.sub"))}
  <div class="vision-grid">
    <div>
      <div class="section">
        <div class="set-list">${setRow(esc(t("vision.enable")), esc(t("vision.enable.d")), toggle("vision.enabled"))}</div>
      </div>
      <div class="section">
        <div class="section-head"><span class="label">${esc(t("vision.preset"))}</span></div>
        <div class="seg" id="vPreset"><span class="thumb"></span>${["natural", "vivid", "cinematic", "crisp", "custom"].map((p) => `<button data-v="${p}" class="${v.preset === p ? "on" : ""}">${esc(t("preset." + p))}</button>`).join("")}</div>
      </div>
      <div class="section">
        <div class="set-list" id="vSliders">${sliders()}</div>
      </div>
      <div class="note">${esc(t("vision.note"))}</div>
    </div>
    <div class="section">
      <div class="section-head"><span class="label">${esc(t("vision.preview"))}</span><span class="aside mono">${esc(t("vision.before"))} / ${esc(t("vision.after"))}</span></div>
      <div class="cv tall" id="vcv"><canvas></canvas><div class="cv-handle"><span></span></div></div>
    </div>
  </div>`;

  const ps = root.querySelector("#vPreset");
  requestAnimationFrame(() => placeThumb(ps));
  ps.addEventListener("click", (e) => {
    const b = e.target.closest("button");
    if (!b) return;
    ps.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x === b));
    placeThumb(ps);
    const p = profile();
    p.vision.preset = b.dataset.v;
    if (PRESETS[b.dataset.v]) Object.assign(p.vision, PRESETS[b.dataset.v]);
    setProfile("vision.preset", b.dataset.v);
    root.querySelector("#vSliders").innerHTML = sliders();
    bindSliders(root);
    preview();
  });
  bindControls(root.querySelector(".set-list"), () => preview());
  bindSliders(root);

  view?.destroy();
  view = new CompareView(root.querySelector("#vcv"));
  preview();
}

function sliders() {
  return KEYS.map((k) => setRow(esc(t("vision." + k)), "", slider("vision." + k, { min: k === "clarity" || k === "detail" ? 0 : -1, max: 1, digits: 2, center: !(k === "clarity" || k === "detail") }), { wide: true })).join("");
}

function bindSliders(root) {
  bindControls(root.querySelector("#vSliders"), () => {
    // Manual changes switch the preset to Custom.
    const p = profile();
    if (p.vision.preset !== "custom") {
      p.vision.preset = "custom";
      const ps = root.querySelector("#vPreset");
      ps.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x.dataset.v === "custom"));
      placeThumb(ps);
    }
    clearTimeout(timer);
    timer = setTimeout(preview, 120);
  });
}

let timer = null;
let token = 0;
async function preview() {
  if (!view) return;
  const my = ++token;
  const src = sample("scene");
  const out = await process({ op: "vision", src, visionParams: { ...profile().vision } });
  if (my === token && view) view.set(src, out);
}

export function leave() {
  view?.destroy();
  view = null;
}
