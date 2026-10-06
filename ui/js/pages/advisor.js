// Advisor: hardware summary, the recommended setup and actionable suggestions.
import { call } from "../bridge.js";
import { icon } from "../icons.js";
import { t } from "../i18n.js";
import { state, mainGpu, primaryMonitor, notify } from "../store.js";
import { esc } from "../ui.js";
import { analyze, gpuTier } from "../advisor.js";

function spec() {
  const s = state.system || {};
  const g = mainGpu();
  const mon = primaryMonitor();
  const gpus = (s.gpus || []).map((x) => `${x.name.replace(/\(R\)|\(TM\)/g, "")}${x.integrated ? ` (${t("sys.integrated")})` : ""}`).join(" + ");
  const onOff = (v) => (v ? t("common.on") : t("common.off"));
  const rows = [
    [t("sys.gpu"), gpus || "—"],
    [t("sys.vram"), g?.vramMB ? `${(g.vramMB / 1024).toFixed(1)} GB` : "—"],
    [t("advisor.tier"), t("tier." + gpuTier(g))],
    [t("sys.display"), `${mon.width}×${mon.height} · ${mon.refresh} Hz${mon.maxRefresh > mon.refresh ? ` (max ${mon.maxRefresh})` : ""}`],
    [t("sys.cpu"), (s.cpu || "—").replace(/\(R\)|\(TM\)/g, "").replace(/\s+/g, " ").trim()],
    [t("sys.ram"), s.ramMB ? `${Math.round(s.ramMB / 1024)} GB` : "—"],
    [t("sys.os"), s.os ? `${s.os.windows11 ? "11" : "10"} · build ${s.os.build}` : "—"],
    [t("sys.hags"), onOff(s.hags)],
    [t("sys.gameMode"), onOff(s.gameMode)],
    [t("sys.power"), s.battery?.present ? (s.battery.onBattery ? t("sys.battery", { p: s.battery.percent }) : t("sys.ac")) : t("sys.ac")],
  ];
  return rows.map(([k, v]) => `<div class="spec-row"><span class="k">${esc(k)}</span><span class="v mono">${esc(v)}</span></div>`).join("");
}

function recRows() {
  const { recs } = analyze();
  if (!recs.length) return `<div class="empty">${esc(t("advisor.none"))}</div>`;
  return recs
    .map((r) => {
      const action = r.applied
        ? `<span class="tag ok">${esc(t("advisor.applied"))}</span>`
        : r.apply
        ? `<button class="btn sm" data-rec="${r.id}">${esc(t("advisor.apply"))}</button>`
        : r.run
        ? `<button class="btn sm" data-rec="${r.id}">${esc(r.action || "")}${icon("external")}</button>`
        : "";
      return `<div class="rec ${r.sev} ${r.applied ? "done" : ""}"><span class="mark"></span>
        <div><b>${esc(r.title)}</b><span>${esc(r.body)}</span></div><div class="ra">${action}</div></div>`;
    })
    .join("");
}

export function render(root) {
  root.innerHTML = `
  <div class="page-head"><div><h1>${esc(t("advisor.title"))}</h1><p>${esc(t("advisor.sub"))}</p></div>
    <div class="side"><button class="btn" id="rescan">${icon("refresh")}${esc(t("advisor.rescan"))}</button></div></div>
  <div class="adv-grid">
    <div class="section">
      <div class="section-head"><span class="label">${esc(t("advisor.recs"))}</span></div>
      <div class="recs" id="recs">${recRows()}</div>
    </div>
    <div class="section">
      <div class="section-head"><span class="label">${esc(t("advisor.system"))}</span></div>
      <div class="spec" id="spec">${spec()}</div>
    </div>
  </div>`;

  root.querySelector("#recs").addEventListener("click", (e) => {
    const b = e.target.closest("[data-rec]");
    if (!b) return;
    const r = analyze().recs.find((x) => x.id === b.dataset.rec);
    if (!r) return;
    if (r.apply) {
      r.apply();
      notify("profile-external");
    } else if (r.run) r.run();
    root.querySelector("#recs").innerHTML = recRows();
  });
  root.querySelector("#rescan").addEventListener("click", async () => {
    const sys = await call("systemInfo").catch(() => null);
    if (sys) state.system = sys;
    root.querySelector("#spec").innerHTML = spec();
    root.querySelector("#recs").innerHTML = recRows();
  });
}
