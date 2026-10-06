// MoonUp Upgraph: installs MoonUp' neural rendering into a game (ReShade + MoonUpUpgraph.fx),
// and AMD FSR 3.1 upscaling + frame generation (OptiScaler) for games that ship DLSS / FSR 2+ / XeSS.
import { call, on } from "../bridge.js";
import { icon } from "../icons.js";
import { t } from "../i18n.js";
import { state, setSetting, saveNow } from "../store.js";
import { esc, slider, setRow, bindControls, confirmDialog, toast, loader } from "../ui.js";

// Library: shown at once from the last scan (kept in this browser profile), refreshed in the background.
const CACHE_KEY = "sw.upgraph.library.v1";
let games = (() => {
  try {
    const v = JSON.parse(localStorage.getItem(CACHE_KEY) || "null");
    return Array.isArray(v) ? v : null;
  } catch {
    return null;
  }
})();
let loading = false;
let fresh = false;         // library scanned in this session
let selected = null;       // dir
let detail = null;         // ScanGame result for the selected game
let busy = "";             // "", "scan", "install", "remove"
let progress = null;       // { stage, fraction }
let filter = "";
let opts = { shader: true, optiscaler: false, frameGen: true, dlss5: false, forceFeeder: false, exe: "" };
const posters = new Map();
let off = null;
let rootEl = null;

function up() {
  if (!state.settings.upgraph) state.settings.upgraph = { folders: [], preset: {} };
  const u = state.settings.upgraph;
  u.folders ||= [];
  u.preset = { tone: 1, color: 0.8, structure: 1, temporal: 0.7, light: 0.6, lightRadius: 1, sharpness: 0.35, ...(u.preset || {}) };
  return u;
}

function errText(e) {
  const s = String(e?.message || e || "");
  const [code, ...rest] = s.split("|");
  const key = `upgraph.err.${code}`;
  const tr = t(key);
  const detail = rest.join("|").trim();
  if (tr === key) return detail || s;
  // the technical reason helps when reporting a problem (e.g. "reshade.me answered HTTP 403")
  return detail && detail.length < 140 ? `${tr} (${detail})` : tr;
}

export function render(root) {
  rootEl = root;
  up();
  root.innerHTML = `
  <div class="page-head"><div><h1>MoonUp Upgraph</h1><p>${esc(t("upgraph.sub"))}</p></div>
    <div class="side"><span class="tag accent">${esc(t("upgraph.inGame"))}</span></div></div>
  <div class="ug-grid">
    <div class="section ug-lib">
      <div class="section-head"><span class="label">${esc(t("upgraph.library"))}</span>
        <span class="row">
          <button class="btn ghost sm" id="ugAdd" title="${esc(t("upgraph.addFolder"))}">${icon("folder")}${icon("plus")}</button>
          <button class="btn ghost sm" id="ugRefresh" title="${esc(t("common.refresh"))}">${icon("refresh")}</button>
        </span></div>
      <input class="input ug-search" id="ugSearch" placeholder="${esc(t("upgraph.search"))}" value="${esc(filter)}">
      <div class="ug-list" id="ugList"></div>
    </div>
    <div class="ug-detail" id="ugDetail"></div>
  </div>
  <div class="section">
    <div class="section-head"><span class="label">${esc(t("upgraph.how"))}</span></div>
    <ul class="facts">
      <li><span class="n">01</span><span>${esc(t("upgraph.how1"))}</span></li>
      <li><span class="n">02</span><span>${esc(t("upgraph.how2"))}</span></li>
      <li><span class="n">03</span><span>${esc(t("upgraph.how3"))}</span></li>
      <li><span class="n">04</span><span>${esc(t("upgraph.how4"))}</span></li>
    </ul>
  </div>
  <div class="note accent">${esc(t("upgraph.note"))}</div>`;

  root.querySelector("#ugRefresh").addEventListener("click", () => loadLibrary(true));
  root.querySelector("#ugAdd").addEventListener("click", addFolder);
  root.querySelector("#ugSearch").addEventListener("input", (e) => {
    filter = e.target.value;
    renderList();
  });
  off?.();
  off = on("upgraph", (e) => {
    if (e.dir !== selected) return;
    progress = { stage: e.stage, fraction: e.fraction };
    renderDetail();
  });
  renderList();
  renderDetail();
  if (!fresh && !loading) loadLibrary(false);
  else setRefreshing();
}

// Scans the library in the background shortly after start, so the page opens instantly.
export function prefetch() {
  if (!fresh && !loading) loadLibrary(false);
}

function setRefreshing() {
  rootEl?.querySelector("#ugRefresh")?.classList.toggle("spinning", loading);
}

export function leave() {
  off?.();
  off = null;
  rootEl = null;
}

async function loadLibrary(force) {
  if (loading) return;
  loading = true;
  renderList();
  setRefreshing();
  try {
    games = (await call("upgraphLibrary")) || [];
    fresh = true;
    try {
      localStorage.setItem(CACHE_KEY, JSON.stringify(games));
    } catch {}
  } catch (e) {
    if (!games) games = [];
    toast(errText(e), "bad");
  }
  loading = false;
  renderList();
  setRefreshing();
  if (force && selected) select(selected);
}

// Posters: at most four requests at a time (each one reads and encodes an image).
const posterQueue = [];
let posterActive = 0;
function posterUrl(file) {
  if (!posters.has(file))
    posters.set(
      file,
      new Promise((resolve) => {
        posterQueue.push({ file, resolve });
        pumpPosters();
      })
    );
  return posters.get(file);
}
function pumpPosters() {
  while (posterActive < 4 && posterQueue.length) {
    const { file, resolve } = posterQueue.shift();
    posterActive++;
    call("upgraphPoster", { file })
      .catch(() => "")
      .then((url) => {
        posterActive--;
        resolve(url || "");
        pumpPosters();
      });
  }
}

async function addFolder() {
  const dir = await call("upgraphPickFolder");
  if (!dir) return;
  const u = up();
  if (!u.folders.includes(dir)) u.folders.push(dir);
  setSetting("upgraph.folders", u.folders);
  await saveNow();
  loadLibrary(true);
}

function renderList() {
  const el = rootEl?.querySelector("#ugList");
  if (!el) return;
  if (loading && !games) {
    el.innerHTML = loader(t("upgraph.scanning"));
    return;
  }
  const list = (games || []).filter((g) => !filter || g.name.toLowerCase().includes(filter.toLowerCase()));
  if (!list.length) {
    el.innerHTML = `<div class="empty">${esc(t(games && games.length ? "upgraph.noMatch" : "upgraph.empty"))}</div>`;
    return;
  }
  el.innerHTML = list
    .map(
      (g) => `<button class="ug-item ${g.dir === selected ? "on" : ""}" data-dir="${esc(g.dir)}">
        <span class="ug-thumb" data-poster="${esc(g.poster || "")}">${esc((g.name || "?").slice(0, 1))}</span>
        <span class="ug-name"><b>${esc(g.name)}</b><span>${esc(g.launcher)}</span></span>
        ${g.installed ? `<span class="tag ok">${esc(t("upgraph.active"))}</span>` : ""}
      </button>`
    )
    .join("");
  el.querySelectorAll(".ug-item").forEach((b) => b.addEventListener("click", () => select(b.dataset.dir)));
  // posters (Steam library art), loaded lazily
  el.querySelectorAll("[data-poster]").forEach(async (s) => {
    const f = s.dataset.poster;
    if (!f) return;
    const url = await posterUrl(f);
    if (url) {
      s.style.backgroundImage = `url("${url}")`;
      s.textContent = "";
    }
  });
}

async function select(dir) {
  selected = dir;
  detail = null;
  progress = null;
  busy = "scan";
  renderList();
  renderDetail();
  try {
    detail = await call("upgraphScan", { dir });
    const opti = detail?.status?.manifest?.optiscaler;
    opts = {
      shader: detail?.status?.installed ? !!detail.status.manifest?.reshade : true,
      optiscaler: detail?.status?.installed ? !!opti : (detail?.upscalers || []).some((u) => u.kind !== "Streamline"),
      frameGen: opti ? !!opti.frameGen : true,
      dlss5: !!detail?.status?.manifest?.dlss5,
      forceFeeder: detail?.status?.manifest?.dlss5?.route === "feeder",
      exe: detail?.exe?.path || "",
    };
  } catch (e) {
    toast(errText(e), "bad");
  }
  busy = "";
  if (selected === dir) renderDetail();
}

function stageText(p) {
  if (!p) return "";
  const pct = p.fraction > 0 && p.fraction < 1 ? ` · ${Math.round(p.fraction * 100)}%` : "";
  return t(`upgraph.stage.${p.stage}`) + pct;
}

function renderDetail() {
  const el = rootEl?.querySelector("#ugDetail");
  if (!el) return;
  if (!selected) {
    el.innerHTML = `<div class="section ug-pick"><div class="empty">${icon("sparkle")}<br>${esc(t("upgraph.pick"))}</div></div>`;
    return;
  }
  const g = (games || []).find((x) => x.dir === selected) || { name: selected.split("\\").pop(), launcher: "" };
  if (!detail) {
    el.innerHTML = busy === "scan"
      ? `<div class="section">${loader(t("upgraph.inspecting"))}</div>`
      : `<div class="section"><div class="empty">${esc(t("upgraph.unreadable"))}</div></div>`;
    return;
  }
  const st = detail.status || {};
  const installed = !!st.installed;
  const exe = detail.exe;
  const cands = detail.candidates || [];
  const upsc = (detail.upscalers || []).filter((u) => u.kind !== "Streamline");
  const canOpti = upsc.length > 0 && exe && exe.bits === 64 && (exe.api === "dxgi" || exe.api === "vulkan");
  const canShader = exe && ["dxgi", "d3d9", "opengl", "d3d10"].includes(exe.api);
  if (!canOpti) opts.optiscaler = false;
  if (!canShader) opts.shader = false;
  // NVIDIA DLSS 5: only offered on GeForce RTX. RTX 50 runs NVIDIA's public model; RTX 20/30/40
  // need a model file built for that GPU (supplied by the user).
  const gpus = detail.gpus || [];
  const rtx50 = gpus.some((x) => x.tier === "rtx50");
  const rtx = rtx50 || gpus.some((x) => x.tier === "rtx");
  const nv = gpus.find((x) => x.tier === "rtx50") || gpus.find((x) => x.tier === "rtx");
  const hasModel = rtx; // RTX 20/30/40 use the modified runtime (or the user's own build)
  const dApi = exe && ["dxgi", "d3d10", "opengl", "d3d9", "d3d8"].includes(exe.api);
  const canDlss5 = rtx && hasModel && dApi;
  if (!canDlss5) opts.dlss5 = false;
  if (opts.dlss5) opts.optiscaler = false;
  const gameDlss = upsc.some((u) => u.kind === "DLSS");
  const route = exe && exe.bits === 64 && ["dxgi", "d3d10"].includes(exe.api) && gameDlss && !opts.forceFeeder ? "native" : "feeder";
  const dlss5Desc = !dApi
    ? t(exe?.api === "vulkan" ? "upgraph.dlss5.vulkan" : "upgraph.dlss5.api")
    : !hasModel
      ? t("upgraph.dlss5.needModel")
      : t(route === "native" ? "upgraph.dlss5.native" : "upgraph.dlss5.feeder") + (rtx50 ? "" : " " + t("upgraph.dlss5.olderNote"));
  const foreign = (st.hooks || []).filter((h) => !h.ours);
  const working = busy === "install" || busy === "remove";

  el.innerHTML = `
  <div class="section ug-card">
    <div class="ug-hero">
      <span class="ug-poster" data-poster="${esc(g.poster || "")}">${esc((g.name || "?").slice(0, 1))}</span>
      <div class="ug-title">
        <h2>${esc(g.name)}</h2>
        <div class="ug-tags">
          ${g.launcher ? `<span class="tag">${esc(g.launcher)}</span>` : ""}
          ${exe ? `<span class="tag accent">${esc(exe.apiLabel)}</span><span class="tag">${exe.bits}-bit</span>` : ""}
          ${detail.unreal ? `<span class="tag">Unreal Engine</span>` : ""}
          ${upsc.map((u) => `<span class="tag ok">${esc(u.kind)}</span>`).join("")}
          ${installed ? `<span class="tag ok">${esc(t("upgraph.installed"))}</span>` : ""}
        </div>
        <div class="ug-path mono">${esc(detail.dir)}</div>
      </div>
    </div>
    ${
      !exe
        ? `<div class="note warn">${esc(t("upgraph.noExe"))}</div>`
        : `<div class="set-list">
      ${setRow(esc(t("upgraph.exe")), esc(exe.rel || ""), cands.length > 1 ? `<select class="input ug-exe" id="ugExe">${cands.map((c) => `<option value="${esc(c.path)}" ${c.path === opts.exe ? "selected" : ""}>${esc(c.rel)} · ${esc(c.apiLabel)}</option>`).join("")}</select>` : `<span class="mono faint">${esc(exe.name)}</span>`)}
      ${setRow(esc(t("upgraph.shader")), esc(t(canShader ? "upgraph.shader.d" : exe.api === "vulkan" ? "upgraph.shader.vulkan" : "upgraph.shader.no")), `<div class="toggle ${opts.shader ? "on" : ""} ${canShader ? "" : "dis"}" data-ug="shader"></div>`)}
      ${setRow(esc(t("upgraph.fsr")), esc(t(canOpti ? "upgraph.fsr.d" : "upgraph.fsr.no")), `<div class="toggle ${opts.optiscaler ? "on" : ""} ${canOpti ? "" : "dis"}" data-ug="optiscaler"></div>`)}
      ${rtx ? setRow(`${esc(t("upgraph.dlss5"))} <span class="tag ${rtx50 ? "ok" : "warn"}">${esc(rtx50 ? "RTX 50" : t("upgraph.dlss5.older"))}</span>`, esc(dlss5Desc) + (nv ? `<br><span class="faint mono">${esc(nv.name)}${nv.driver ? " · " + esc(nv.driver) : ""}</span>` : ""), `<div class="toggle ${opts.dlss5 ? "on" : ""} ${canDlss5 ? "" : "dis"}" data-ug="dlss5"></div>`) : ""}
      ${rtx && !rtx50 ? setRow(esc(t("upgraph.dlss5.model")), esc(detail.dlss5Custom ? t("upgraph.dlss5.modelReady", { v: detail.dlss5Custom }) : t("upgraph.dlss5.model.d")), `<span class="row"><button class="btn sm" id="ugPickNr">${icon("folder")}${esc(t("upgraph.dlss5.pick"))}</button>${detail.dlss5Custom ? `<button class="btn ghost sm" id="ugClearNr">${icon("trash")}</button>` : ""}</span>`) : ""}
      ${canDlss5 && opts.dlss5 && gameDlss && exe.bits === 64 && ["dxgi", "d3d10"].includes(exe.api) ? setRow(esc(t("upgraph.dlss5.force")), esc(t("upgraph.dlss5.force.d")), `<div class="toggle ${opts.forceFeeder ? "on" : ""}" data-ug="forceFeeder"></div>`) : ""}
      ${canOpti && opts.optiscaler ? setRow(esc(t("upgraph.fg")), esc(t("upgraph.fg.d")), `<div class="toggle ${opts.frameGen ? "on" : ""}" data-ug="frameGen"></div>`) : ""}
    </div>`
    }
    ${detail.antiCheat ? `<div class="note warn">${esc(t("upgraph.anticheat"))}</div>` : ""}
    ${foreign.length ? `<div class="note warn">${esc(t("upgraph.foreign", { files: foreign.map((h) => `${h.file} (${h.kind})`).join(", ") }))}</div>` : ""}
    <div class="ug-actions">
      ${exe ? `<button class="btn primary" id="ugInstall" ${working || (!opts.shader && !opts.optiscaler && !opts.dlss5) ? "disabled" : ""}>${icon("sparkle")}${esc(t(installed ? "upgraph.reinstall" : "upgraph.install"))}</button>` : ""}
      ${installed ? `<button class="btn danger" id="ugRemove" ${working ? "disabled" : ""}>${icon("trash")}${esc(t("upgraph.remove"))}</button>` : ""}
      <button class="btn ghost" id="ugOpen">${icon("folder")}${esc(t("upgraph.open"))}</button>
      <span class="ug-progress ${working ? "" : "hidden"}"><i style="width:${Math.round((progress?.fraction || 0) * 100)}%"></i></span>
      <span class="faint ug-stage">${working ? esc(stageText(progress)) : ""}</span>
    </div>
    ${installed ? `<div class="note accent">${esc(t("upgraph.keys"))}${st.manifest?.optiscaler ? " " + esc(t("upgraph.keysOpti")) : ""}${st.manifest?.dlss5 ? " " + esc(t(st.manifest.dlss5.route === "feeder" ? "upgraph.keysDlss5Feeder" : "upgraph.keysDlss5")) : ""}</div>` : ""}
    ${installed && st.manifest?.dlss5 ? `<div class="note"><span class="mono faint">DLSS 5 · ${esc(st.manifest.dlss5.route)} · model ${esc(st.manifest.dlss5.model || "")} · RenoDX ${esc(st.manifest.dlss5.consumer || "")}${st.manifest.dlss5.feeder ? " · Feeder " + esc(st.manifest.dlss5.feeder) : ""}</span></div>` : ""}
  </div>

  <div class="section">
    <div class="section-head"><span class="label">${esc(t("upgraph.look"))}</span><span class="aside">${esc(t("upgraph.look.d"))}</span></div>
    <div class="set-list" id="ugPreset">
      ${setRow(esc(t("render.tone")), esc(t("render.tone.d")), slider("upgraph.preset.tone", { scope: "settings", min: 0, max: 2 }), { wide: true })}
      ${setRow(esc(t("render.structure")), esc(t("render.structure.d")), slider("upgraph.preset.structure", { scope: "settings", min: 0, max: 2 }), { wide: true })}
      ${setRow(esc(t("render.color")), esc(t("render.color.d")), slider("upgraph.preset.color", { scope: "settings", min: 0, max: 2 }), { wide: true })}
      ${setRow(esc(t("upgraph.light")), esc(t("upgraph.light.d")), slider("upgraph.preset.light", { scope: "settings", min: 0, max: 1.5 }), { wide: true })}
      ${setRow(esc(t("upgraph.sharp")), esc(t("upgraph.sharp.d")), slider("upgraph.preset.sharpness", { scope: "settings", min: 0, max: 1 }), { wide: true })}
      ${setRow(esc(t("render.temporal")), esc(t("render.temporal.d")), slider("upgraph.preset.temporal", { scope: "settings", min: 0, max: 1 }), { wide: true })}
    </div>
    ${installed ? `<div class="ug-actions"><button class="btn sm" id="ugApply">${icon("check")}${esc(t("upgraph.applyLook"))}</button><span class="faint">${esc(t("upgraph.applyLook.d"))}</span></div>` : ""}
  </div>

  <div class="section">
    <div class="section-head"><span class="label">ReShade</span>
      <button class="btn ghost sm" id="ugPickReshade">${icon("folder")}${esc(t("upgraph.pickReshade"))}</button></div>
    <div class="note">${esc(detail.reshadeCached ? t("upgraph.reshadeReady", { v: detail.reshadeCached }) : t("upgraph.reshadeAuto"))}</div>
  </div>`;

  // poster
  const ps = el.querySelector(".ug-poster");
  if (g.poster) {
    if (!posters.has(g.poster)) posters.set(g.poster, call("upgraphPoster", { file: g.poster }).catch(() => ""));
    posters.get(g.poster).then((url) => {
      if (url && ps) {
        ps.style.backgroundImage = `url("${url}")`;
        ps.textContent = "";
      }
    });
  }
  el.querySelectorAll("[data-ug]").forEach((tg) =>
    tg.addEventListener("click", () => {
      if (tg.classList.contains("dis")) return;
      const k = tg.dataset.ug;
      opts[k] = !opts[k];
      // DLSS 5 and OptiScaler both take the game's DLSS calls: only one of them
      // DLSS 5 already re-renders the frame: the MoonUp effect on top is optional (off by default)
      if (k === "dlss5" && opts.dlss5) {
        opts.optiscaler = false;
        opts.shader = false;
      }
      if (k === "optiscaler" && opts.optiscaler) opts.dlss5 = false;
      renderDetail();
    })
  );
  el.querySelector("#ugExe")?.addEventListener("change", (e) => (opts.exe = e.target.value));
  el.querySelector("#ugInstall")?.addEventListener("click", install);
  el.querySelector("#ugRemove")?.addEventListener("click", remove);
  el.querySelector("#ugOpen")?.addEventListener("click", () => call("upgraphOpenFolder", { dir: detail.dir }));
  el.querySelector("#ugApply")?.addEventListener("click", applyLook);
  el.querySelector("#ugPickReshade")?.addEventListener("click", pickReshade);
  el.querySelector("#ugPickNr")?.addEventListener("click", async () => {
    try {
      const r = await call("upgraphPickDlssNr");
      if (r?.version) {
        detail.dlss5Custom = r.version;
        toast(t("upgraph.dlss5.modelReady", { v: r.version }), "ok");
        renderDetail();
      }
    } catch (e) {
      toast(errText(e), "bad");
    }
  });
  el.querySelector("#ugClearNr")?.addEventListener("click", async () => {
    await call("upgraphClearDlssNr").catch(() => {});
    detail.dlss5Custom = "";
    opts.dlss5 = false;
    renderDetail();
  });
  bindControls(el.querySelector("#ugPreset"), () => {});
}

async function install() {
  let acceptAntiCheat = false;
  if (detail.antiCheat) {
    acceptAntiCheat = await confirmDialog(t("upgraph.anticheatConfirm"), { okLabel: t("upgraph.install"), danger: true });
    if (!acceptAntiCheat) return;
  }
  const dir = selected;
  busy = "install";
  progress = { stage: "start", fraction: 0 };
  renderDetail();
  try {
    await saveNow();
    detail = await call("upgraphInstall", { dir, options: { ...opts, acceptAntiCheat, preset: up().preset } });
    toast(t("upgraph.done"), "ok");
    const g = (games || []).find((x) => x.dir === dir);
    if (g) g.installed = true;
  } catch (e) {
    toast(errText(e), "bad");
  }
  busy = "";
  progress = null;
  renderList();
  renderDetail();
}

async function remove() {
  if (!(await confirmDialog(t("upgraph.removeConfirm"), { okLabel: t("upgraph.remove"), danger: true }))) return;
  const dir = selected;
  busy = "remove";
  progress = { stage: "remove", fraction: 0 };
  renderDetail();
  try {
    detail = await call("upgraphRemove", { dir });
    toast(t("upgraph.removed"), "ok");
    const g = (games || []).find((x) => x.dir === dir);
    if (g) g.installed = false;
  } catch (e) {
    toast(errText(e), "bad");
  }
  busy = "";
  progress = null;
  renderList();
  renderDetail();
}

async function applyLook() {
  try {
    await saveNow();
    detail = await call("upgraphPreset", { dir: selected, preset: up().preset });
    toast(t("upgraph.lookSaved"), "ok");
  } catch (e) {
    toast(errText(e), "bad");
  }
}

async function pickReshade() {
  try {
    const r = await call("upgraphPickReShade");
    if (r?.version) {
      toast(t("upgraph.reshadeReady", { v: r.version }), "ok");
      if (detail) detail.reshadeCached = r.version;
      renderDetail();
    }
  } catch (e) {
    toast(errText(e), "bad");
  }
}


