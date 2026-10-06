// Settings: language, appearance, startup, hotkey, overlay and data.
import { call } from "../bridge.js";
import { icon } from "../icons.js";
import { t, LANGS, lang } from "../i18n.js";
import { state, setSetting, hotkeyLabel } from "../store.js";
import { seg, toggle, select, setRow, bindControls, confirmDialog, toast, esc } from "../ui.js";

const ACCENTS = [
  { id: "moon", c: "#d7dce5" },
  { id: "azure", c: "#3d7bff" },
  { id: "cyan", c: "#1fb6d6" },
  { id: "lime", c: "#a3d93a" },
  { id: "orange", c: "#ff7a1a" },
  { id: "white", c: "#e8e8ea" },
];

export function render(root) {
  const s = state.settings;
  const prof = state.settings.profiles[state.settings.activeProfile]?.name || "";
  root.innerHTML = `
  <div class="page-head"><div><h1>${esc(t("settings.title"))}</h1></div></div>

  <div class="section">
    <div class="section-head"><span class="label">${esc(t("settings.general"))}</span></div>
    <div class="set-list">
      ${setRow(esc(t("settings.language")), "", select("language", LANGS.map((l) => ({ v: l.id, label: l.name })), { scope: "settings", width: 220 }))}
      ${setRow(esc(t("settings.accent")), "", `<div class="accents">${ACCENTS.map((a) => `<button class="accent-dot ${(s.accent || "moon") === a.id ? "on" : ""}" data-accent-id="${a.id}" style="--c:${a.c}" title="${a.id}"></button>`).join("")}</div>`)}
      ${setRow(esc(t("settings.intro")), esc(t("settings.intro.d")), toggle("intro", { scope: "settings" }))}
      ${setRow(esc(t("settings.reduced")), esc(t("settings.reduced.d")), toggle("reducedMotion", { scope: "settings" }))}
      ${setRow(esc(t("settings.startup")), esc(t("settings.startup.d")), toggle("startWithWindows", { scope: "settings" }))}
      ${setRow(esc(t("settings.minTray")), "", toggle("minimizeToTray", { scope: "settings" }))}
      ${setRow(esc(t("settings.closeTray")), esc(t("settings.closeTray.d")), toggle("closeToTray", { scope: "settings" }))}
    </div>
  </div>

  <div class="section">
    <div class="section-head"><span class="label">${esc(t("settings.controls"))}</span></div>
    <div class="set-list">
      ${setRow(esc(t("settings.hotkey")), esc(t("settings.hotkey.d")), `<button class="btn hk-btn" id="hkBtn"><span class="kbd">${esc(hotkeyLabel())}</span></button>`)}
      ${setRow(esc(t("settings.delay")), esc(t("settings.delay.d")), seg("scaleDelay", [3, 5, 8, 10].map((n) => ({ v: n, label: t("seconds", { n }) })), { scope: "settings" }))}
    </div>
  </div>

  <div class="section">
    <div class="section-head"><span class="label">${esc(t("settings.overlay"))}</span><span class="aside">${esc(t("home.profile"))}: ${esc(prof)}</span></div>
    <div class="set-list">
      ${setRow(esc(t("settings.fps")), esc(t("settings.fps.d")), toggle("showFps"))}
      ${setRow(esc(t("settings.graph")), "", toggle("showGraph"))}
      ${setRow(esc(t("settings.position")), "", seg("hudPosition", ["tl", "tr", "bl", "br"].map((v) => ({ v, label: t("pos." + v) }))))}
    </div>
  </div>

  <div class="section">
    <div class="section-head"><span class="label">${esc(t("settings.data"))}</span></div>
    <div class="row">
      <button class="btn" id="openLogs">${icon("folder")}${esc(t("settings.logs"))}</button>
      <span class="spacer"></span>
      <button class="btn danger" id="resetAll">${esc(t("settings.reset"))}</button>
    </div>
  </div>`;

  bindControls(root, (path, v) => {
    if (path === "language") window.dispatchEvent(new CustomEvent("sw-language", { detail: v }));
    if (path === "reducedMotion") window.dispatchEvent(new Event("sw-appearance"));
  });

  root.querySelectorAll("[data-accent-id]").forEach((b) =>
    b.addEventListener("click", () => {
      setSetting("accent", b.dataset.accentId);
      root.querySelectorAll("[data-accent-id]").forEach((x) => x.classList.toggle("on", x === b));
      window.dispatchEvent(new Event("sw-appearance"));
    })
  );

  const hk = root.querySelector("#hkBtn");
  hk.addEventListener("click", () => {
    hk.classList.add("recording");
    hk.innerHTML = `<span>${esc(t("settings.hotkey.record"))}</span>`;
    const onKey = (e) => {
      e.preventDefault();
      if (e.key === "Escape") return finish();
      const c = e.code;
      const key = c.startsWith("Key") ? c.slice(3) : c.startsWith("Digit") ? c.slice(5) : /^F\d{1,2}$/.test(c) ? c : ["Home", "End", "Insert", "PageUp", "PageDown"].includes(c) ? c : null;
      if (!key) return;
      if (!(e.ctrlKey || e.altKey || e.shiftKey) && !/^F\d/.test(key)) return;
      setSetting("hotkey", { ctrl: e.ctrlKey, alt: e.altKey, shift: e.shiftKey, key });
      finish();
    };
    const finish = () => {
      document.removeEventListener("keydown", onKey, true);
      hk.classList.remove("recording");
      hk.innerHTML = `<span class="kbd">${esc(hotkeyLabel())}</span>`;
    };
    document.addEventListener("keydown", onKey, true);
  });

  root.querySelector("#openLogs").addEventListener("click", () => call("openLogs"));
  root.querySelector("#resetAll").addEventListener("click", async () => {
    if (!(await confirmDialog(t("settings.resetConfirm"), { danger: true, okLabel: t("settings.reset") }))) return;
    const fresh = await call("resetSettings").catch(() => null);
    if (fresh) {
      state.settings = fresh;
      window.dispatchEvent(new Event("sw-appearance"));
      window.dispatchEvent(new CustomEvent("sw-language", { detail: fresh.language || lang() }));
      toast(t("settings.saved"), "ok");
    }
  });
}
