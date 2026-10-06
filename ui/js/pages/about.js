// About: product, technology, a plain note on DLSS and third-party notices.
import { t } from "../i18n.js";
import { state } from "../store.js";
import { esc } from "../ui.js";

export function render(root) {
  root.innerHTML = `
  <div class="about-hero">
    <img src="img/logo.png" alt="" class="about-logo">
    <div>
      <div class="about-name">MoonUp</div>
      <p class="about-desc">${esc(t("about.desc"))}</p>
    </div>
  </div>
  <div class="section">
    <div class="section-head"><span class="label">${esc(t("about.tech"))}</span></div>
    <div class="spec">
      <div class="spec-row"><span class="k">MoonUp Edge</span><span class="v">${esc(t("tech.edge"))}</span></div>
      <div class="spec-row"><span class="k">MoonUp Neural</span><span class="v">${esc(t("tech.neural"))}</span></div>
      <div class="spec-row"><span class="k">MoonUp Neural Render</span><span class="v">${esc(t("tech.render"))}</span></div>
      <div class="spec-row"><span class="k">MoonUp Upgraph</span><span class="v">${esc(t("tech.upgraph"))}</span></div>
      <div class="spec-row"><span class="k">MoonUp Motion</span><span class="v">${esc(t("tech.motion"))}</span></div>
      <div class="spec-row"><span class="k">MoonUp Vision</span><span class="v">${esc(t("tech.vision"))}</span></div>
    </div>
  </div>
  <div class="section">
    <div class="section-head"><span class="label">${esc(t("about.dlss"))}</span></div>
    <div class="note">${esc(t("about.dlssText"))}</div>
  </div>
  <div class="section">
    <div class="section-head"><span class="label">${esc(t("about.notices"))}</span></div>
    <div class="notices">${esc(t("about.n1"))}<br>${esc(t("about.n2"))}<br>${esc(t("about.n3"))}<br>AMD FidelityFX Super Resolution 1 — MIT, © 2021 Advanced Micro Devices, Inc.<br>NVIDIA Image Scaling SDK v1.0.3 — MIT, © 2022 NVIDIA CORPORATION &amp; AFFILIATES<br>ArtCNN C4F16 — MIT, © 2024 João Chrisóstomo<br>ReShade (downloaded by Upgraph) — BSD 3-Clause, © 2014 Patrick Mours<br>OptiScaler (downloaded by Upgraph, optional) — GPL-3.0, optiscaler contributors<br>Upgraph design inspired by DLSS5-Swapper (MIT, Rakan Alkhaldi); no code or files from NVIDIA are used<br>Inter, JetBrains Mono — SIL Open Font License 1.1</div>
  </div>`;
}
