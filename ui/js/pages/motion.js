// Frame generation: mode, target rate, flow quality, protections and presentation.
import { t } from "../i18n.js";
import { profile, setProfile, primaryMonitor } from "../store.js";
import { seg, toggle, setRow, bindControls, placeThumb, esc } from "../ui.js";
import { profileHead } from "./scaling.js";

const MODES = ["off", "x2", "x3", "x4", "adaptive"];

function timeline(mode) {
  const hz = primaryMonitor().refresh || 60;
  let gen;
  if (mode === "off") gen = 0;
  else if (mode === "adaptive") gen = Math.max(1, Math.round(hz / 60) - 1);
  else gen = parseInt(mode.slice(1), 10) - 1;
  const groups = Math.max(3, Math.floor(24 / (gen + 1)));
  let cells = "";
  let k = 0;
  for (let g = 0; g < groups; g++) {
    cells += `<span class="tl-f cap" style="--d:${k++}"></span>`;
    for (let i = 0; i < gen; i++) cells += `<span class="tl-f gen" style="--d:${k++}"></span>`;
  }
  return `<div class="tl">${cells}</div>
    <div class="tl-legend"><span><i class="cap"></i>${esc(t("motion.how.captured"))}</span><span><i class="gen"></i>${esc(t("motion.how.generated"))}</span></div>`;
}

export function render(root) {
  const p = profile();
  const hz = primaryMonitor().refresh || 60;
  const custom = p.targetFps > 0;
  root.innerHTML = `
  ${profileHead(t("motion.title"), t("motion.sub"))}
  <div class="section">
    <div class="section-head"><span class="label">${esc(t("motion.mode"))}</span></div>
    ${seg("frameGen", MODES.map((m) => ({ v: m, label: t("fg." + m) })))}
    <div class="mode-desc" id="fgDesc">${esc(t(`fg.${p.frameGen}.d`))}</div>
    <div style="margin-top:14px" id="tlBox">${timeline(p.frameGen)}</div>
  </div>

  <div class="cols">
    <div class="section">
      <div class="section-head"><span class="label">${esc(t("motion.quality"))}</span></div>
      <div class="set-list">
        ${setRow(esc(t("motion.quality")), esc(t("motion.quality.d")), seg("flowQuality", ["performance", "balanced", "quality"].map((v) => ({ v, label: t("q." + v) }))))}
        ${setRow(esc(t("motion.target")), "", `<div class="row"><div class="seg" id="tmSeg"><span class="thumb"></span>
            <button data-v="auto" class="${custom ? "" : "on"}">${hz} Hz</button>
            <button data-v="custom" class="${custom ? "on" : ""}">${esc(t("motion.targetCustom"))}</button></div>
          <input class="input num ${custom ? "" : "hidden"}" id="targetFps" type="number" min="30" max="500" value="${p.targetFps || hz}"></div>`)}
        ${setRow(esc(t("motion.hud")), esc(t("motion.hud.d")), toggle("hudProtect"))}
        ${setRow(esc(t("motion.scene")), esc(t("motion.scene.d")), toggle("sceneCut"))}
      </div>
    </div>
    <div class="section">
      <div class="section-head"><span class="label">${esc(t("motion.sync"))}</span></div>
      <div class="set-list">
        ${setRow(esc(t("motion.vsync")), esc(t("motion.vsync.d")), toggle("vsync"))}
        ${setRow(esc(t("motion.tearing")), esc(t("motion.tearing.d")), toggle("allowTearing"))}
        ${setRow(esc(t("motion.latency")), esc(t("motion.latency.d")), seg("maxLatency", [1, 2, 3].map((v) => ({ v, label: String(v) }))))}
      </div>
    </div>
  </div>

  <div class="section">
    <div class="section-head"><span class="label">${esc(t("motion.how"))}</span></div>
    <ul class="facts">
      <li><span class="n">01</span><span>${esc(t("motion.tech1"))}</span></li>
      <li><span class="n">02</span><span>${esc(t("motion.tech2"))}</span></li>
      <li><span class="n">03</span><span>${esc(t("motion.tech3"))}</span></li>
      <li><span class="n">—</span><span>${esc(t("motion.howText"))}</span></li>
    </ul>
  </div>`;

  const tm = root.querySelector("#tmSeg");
  const inp = root.querySelector("#targetFps");
  requestAnimationFrame(() => placeThumb(tm));
  tm.addEventListener("click", (e) => {
    const b = e.target.closest("button");
    if (!b) return;
    tm.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x === b));
    placeThumb(tm);
    inp.classList.toggle("hidden", b.dataset.v !== "custom");
    setProfile("targetFps", b.dataset.v === "custom" ? +inp.value || hz : 0);
  });
  inp.addEventListener("change", () => {
    const v = Math.max(30, Math.min(500, +inp.value || hz));
    inp.value = v;
    setProfile("targetFps", v);
  });

  bindControls(root, (path, v) => {
    if (path === "frameGen") {
      root.querySelector("#tlBox").innerHTML = timeline(v);
      root.querySelector("#fgDesc").textContent = t(`fg.${v}.d`);
    }
  });
}
