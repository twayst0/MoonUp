// Neural Render v2: the one-step neural rendering stage (tone, colour, structure) with a live
// before/after preview computed by the same network on the CPU.
import { t } from "../i18n.js";
import { profile } from "../store.js";
import { toggle, slider, setRow, bindControls, esc } from "../ui.js";
import { profileHead } from "./scaling.js";
import { CompareView, process, sample } from "../lab/viewer.js";

let view = null;
let timer = null;
let token = 0;

export function render(root) {
  root.innerHTML = `
  ${profileHead(t("render.title"), t("render.sub"))}
  <div class="vision-grid">
    <div>
      <div class="section">
        <div class="set-list">${setRow(esc(t("render.enable")), esc(t("render.enable.d")), toggle("render.enabled"))}</div>
      </div>
      <div class="section">
        <div class="section-head"><span class="label">${esc(t("render.controls"))}</span></div>
        <div class="set-list" id="rSliders">
          ${setRow(esc(t("render.tone")), esc(t("render.tone.d")), slider("render.tone", { min: 0, max: 2, digits: 2 }), { wide: true })}
          ${setRow(esc(t("render.structure")), esc(t("render.structure.d")), slider("render.structure", { min: 0, max: 2, digits: 2 }), { wide: true })}
          ${setRow(esc(t("render.color")), esc(t("render.color.d")), slider("render.color", { min: 0, max: 2, digits: 2 }), { wide: true })}
          ${setRow(esc(t("render.temporal")), esc(t("render.temporal.d")), slider("render.temporal", { min: 0, max: 1, digits: 2 }), { wide: true })}
        </div>
      </div>
    </div>
    <div class="section">
      <div class="section-head"><span class="label">${esc(t("vision.preview"))}</span><span class="aside">${esc(t("vision.before"))} / ${esc(t("vision.after"))}</span></div>
      <div class="cv tall" id="rcv"><canvas></canvas><div class="cv-handle"><span></span></div><div class="cv-busy hidden" id="rBusy">${esc(t("lab.rendering"))}</div></div>
    </div>
  </div>

  <div class="section">
    <div class="section-head"><span class="label">${esc(t("render.how"))}</span></div>
    <ul class="facts">
      <li><span class="n">01</span><span>${esc(t("render.tech1"))}</span></li>
      <li><span class="n">02</span><span>${esc(t("render.tech2"))}</span></li>
      <li><span class="n">03</span><span>${esc(t("render.tech3"))}</span></li>
      <li><span class="n">04</span><span>${esc(t("render.tech4"))}</span></li>
      <li><span class="n">05</span><span>${esc(t("render.tech5"))}</span></li>
    </ul>
  </div>
  <div class="note accent">${esc(t("render.note"))}</div>`;

  bindControls(root.querySelector(".set-list"), () => {});
  bindControls(root.querySelector("#rSliders"), () => {
    clearTimeout(timer);
    timer = setTimeout(preview, 140);
  });
  view?.destroy();
  view = new CompareView(root.querySelector("#rcv"));
  preview();
}

async function preview() {
  if (!view) return;
  const my = ++token;
  const busy = document.getElementById("rBusy");
  busy?.classList.remove("hidden");
  const src = sample("town");
  const r = profile().render || {};
  const out = await process({ op: "render", src, renderParams: { tone: r.tone ?? 1.0, color: r.color ?? 0.8, structure: r.structure ?? 1.0 } });
  if (my === token && view) {
    view.set(src, out);
    busy?.classList.add("hidden");
  }
}

export function leave() {
  view?.destroy();
  view = null;
}
