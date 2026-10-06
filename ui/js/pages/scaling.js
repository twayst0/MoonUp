// Scaling: scale mode, upscaler, sharpness, capture back end, GPU, cursor and focus.
import { t } from "../i18n.js";
import { state, profile } from "../store.js";
import { seg, optCard, choices, slider, toggle, select, setRow, costMeter, bindControls, esc } from "../ui.js";

const MODES = ["auto", "fullscreen", "custom", "integer", "off"];
const RENDER_SCALES = [100, 85, 75, 67, 50];
const UPSCALERS = [
  { id: "neural", cost: 5 },
  { id: "anime", cost: 5 },
  { id: "edge", cost: 2 },
  { id: "fsr", cost: 2 },
  { id: "nis", cost: 2 },
  { id: "lanczos", cost: 3 },
  { id: "bicubic", cost: 2 },
  { id: "bilinear", cost: 1 },
  { id: "nearest", cost: 1 },
  { id: "pixel", cost: 1 },
];

function gpuOptions() {
  const list = [{ v: -1, label: t("gpu.auto") }];
  (state.system?.gpus || []).forEach((g) => list.push({ v: g.index, label: g.name.replace(/\(R\)|\(TM\)/g, ""), hint: g.integrated ? t("sys.integrated") : "" }));
  return list;
}

export function profileHead(title, desc) {
  const p = profile();
  return `<div class="page-head"><div><h1>${esc(title)}</h1><p>${esc(desc)}</p></div>
    <div class="side"><span class="label">${esc(t("home.profile"))}</span><span class="tag accent">${esc(p.name)}</span></div></div>`;
}

export function render(root) {
  const p = profile();
  if (typeof p.renderScale !== "number") p.renderScale = Number(p.renderScale) || 100;
  const upList = UPSCALERS.map((u) => {
    const missing = (u.id === "neural" || u.id === "anime") && !state.neuralBundled;
    const right = `${missing ? `<span class="tag">${esc(t("upscaler.neuralMissing"))}</span>` : ""}${costMeter(u.cost)}`;
    return optCard("upscaler", u.id, esc(t(`upscaler.${u.id}.name`)), esc(t(`upscaler.${u.id}.desc`)), { right, disabled: missing });
  }).join("");

  root.innerHTML = `
  ${profileHead(t("scaling.title"), t("scaling.sub"))}
  <div class="section">
    <div class="section-head"><span class="label">${esc(t("scaling.mode"))}</span></div>
    ${seg("scaleMode", MODES.map((m) => ({ v: m, label: t("mode." + m) })))}
    <div class="mode-desc" id="modeDesc">${esc(t(`mode.${p.scaleMode}.d`))}</div>
    <div id="factorRow" class="${p.scaleMode === "custom" ? "" : "hidden"}" style="margin-top:12px">
      <div class="set-list">${setRow(esc(t("scaling.factor")), "", slider("factor", { min: 1, max: 4, step: 0.05, digits: 2, suffix: "×" }), { wide: true })}</div>
    </div>
    <div class="set-list" style="margin-top:12px">
      ${setRow(esc(t("scaling.renderScale")), esc(t("scaling.renderScale.d")), seg("renderScale", RENDER_SCALES.map((v) => ({ v, label: v === 100 ? t("renderScale.off") : v + "%" }))))}
    </div>
  </div>

  <div class="section">
    <div class="section-head"><span class="label">${esc(t("scaling.algorithm"))}</span><span class="aside">${esc(t("cost"))}</span></div>
    ${choices(upList)}
    <div class="set-list" style="border-top:0">${setRow(esc(t("scaling.sharpness")), esc(t("scaling.sharpness.d")), slider("sharpness", { digits: 2 }), { wide: true })}</div>
  </div>

  <div class="cols">
    <div class="section">
      <div class="section-head"><span class="label">${esc(t("scaling.capture"))}</span></div>
      ${choices(optCard("capture", "wgc", esc(t("capture.wgc")), esc(t("capture.wgc.d"))) + optCard("capture", "dda", esc(t("capture.dda")), esc(t("capture.dda.d"))))}
      <div class="set-list" style="border-top:0">${setRow(esc(t("scaling.gpu")), "", select("adapter", gpuOptions(), { width: 240 }))}</div>
    </div>
    <div class="section">
      <div class="section-head"><span class="label">${esc(t("scaling.cursor"))}</span></div>
      <div class="set-list">
        ${setRow(esc(t("cursor.draw")), esc(t("cursor.draw.d")), toggle("drawCursor"))}
        ${setRow(esc(t("cursor.clip")), esc(t("cursor.clip.d")), toggle("clipCursor"))}
        ${setRow(esc(t("scaling.pause")), esc(t("scaling.pause.d")), toggle("pauseWhenUnfocused"))}
      </div>
    </div>
  </div>`;

  bindControls(root, (path, v) => {
    if (path === "scaleMode") {
      root.querySelector("#factorRow").classList.toggle("hidden", v !== "custom");
      root.querySelector("#modeDesc").textContent = t(`mode.${v}.d`);
    }
  });
}
