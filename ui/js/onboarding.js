// First run: language, detected hardware with tuned defaults, and a short how-to.
import { t, LANGS, lang } from "./i18n.js";
import { state, saveNow, hotkeyLabel, mainGpu, primaryMonitor } from "./store.js";
import { modal, esc } from "./ui.js";
import { applyPlan, gpuTier } from "./advisor.js";
import { applyLanguage } from "./app.js";

export function runOnboarding() {
  return new Promise((resolve) => {
    let step = 0;
    let plan = null;
    modal({
      title: `<span class="ob-title">${esc(t("ob.welcome"))}</span>`,
      width: 560,
      body: `<div id="obBody" class="ob"></div>`,
      foot: `<div class="ob-dots" id="obDots"></div><span class="spacer"></span><button class="btn ghost" id="obBack">${esc(t("ob.back"))}</button><button class="btn primary" id="obNext">${esc(t("ob.next"))}</button>`,
      onMount: (m, close) => {
        const body = m.querySelector("#obBody");
        const next = m.querySelector("#obNext");
        const back = m.querySelector("#obBack");
        const draw = () => {
          m.querySelector(".ob-title").textContent = t("ob.welcome");
          back.textContent = t("ob.back");
          next.textContent = step === 2 ? t("ob.start") : t("ob.next");
          back.style.visibility = step === 0 ? "hidden" : "visible";
          m.querySelector("#obDots").innerHTML = [0, 1, 2].map((i) => `<i class="${i === step ? "on" : ""}"></i>`).join("");
          if (step === 0) {
            body.innerHTML = `<p class="muted" style="margin-bottom:16px">${esc(t("ob.welcome.d"))}</p>
              <div class="label ob-k">${esc(t("ob.lang"))}</div>
              <div class="lang-grid">${LANGS.map((l) => `<button class="lang-btn ${l.id === lang() ? "on" : ""}" data-l="${l.id}">${esc(l.name)}</button>`).join("")}</div>`;
            body.querySelectorAll(".lang-btn").forEach((b) =>
              b.addEventListener("click", async () => {
                state.settings.language = b.dataset.l;
                await applyLanguage(b.dataset.l);
                draw();
              })
            );
          } else if (step === 1) {
            const g = mainGpu();
            const mon = primaryMonitor();
            if (!plan) plan = applyPlan(state.settings.profiles[state.settings.activeProfile]);
            body.innerHTML = `<div class="label ob-k">${esc(t("ob.detected"))}</div>
              <div class="spec">
                <div class="spec-row"><span class="k">${esc(t("sys.gpu"))}</span><span class="v">${esc((g?.name || "—").replace(/\(R\)|\(TM\)/g, ""))}</span></div>
                <div class="spec-row"><span class="k">${esc(t("advisor.tier"))}</span><span class="v">${esc(t("tier." + gpuTier(g)))}</span></div>
                <div class="spec-row"><span class="k">${esc(t("sys.display"))}</span><span class="v mono">${mon.width}×${mon.height} · ${mon.refresh} Hz</span></div>
              </div>
              <div class="label ob-k" style="margin-top:20px">${esc(t("ob.tuned"))}</div>
              <div class="spec">
                <div class="spec-row"><span class="k">${esc(t("scaling.algorithm"))}</span><span class="v">${esc(t(`upscaler.${plan.upscaler}.name`))}</span></div>
                <div class="spec-row"><span class="k">${esc(t("motion.quality"))}</span><span class="v">${esc(t("q." + plan.flowQuality))}</span></div>
                <div class="spec-row"><span class="k">${esc(t("scaling.sharpness"))}</span><span class="v mono">${plan.sharpness.toFixed(2)}</span></div>
              </div>`;
          } else {
            const hk = `<span class="kbd">${esc(hotkeyLabel())}</span>`;
            body.innerHTML = `<div class="label ob-k">${esc(t("ob.how"))}</div>
              <ol class="ob-steps">
                <li><span>01</span><p>${esc(t("ob.how1"))}</p></li>
                <li><span>02</span><p>${t("ob.how2", { hk })}</p></li>
                <li><span>03</span><p>${t("ob.how3", { hk })}</p></li>
              </ol>`;
          }
        };
        next.addEventListener("click", () => {
          if (step < 2) {
            step++;
            draw();
          } else close();
        });
        back.addEventListener("click", () => {
          if (step > 0) step--;
          draw();
        });
        draw();
      },
      onClose: () => {
        state.settings.firstRun = false;
        if (!state.settings.language) state.settings.language = lang();
        saveNow();
        window.dispatchEvent(new Event("sw-rerender"));
        resolve();
      },
    });
  });
}
