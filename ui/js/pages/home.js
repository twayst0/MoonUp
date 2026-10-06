// Control: processing chain, telemetry, target selection and the engage bar.
import { call } from "../bridge.js";
import { icon } from "../icons.js";
import { t, fmt } from "../i18n.js";
import { state, profile, hotkeyLabel, subscribe, primaryMonitor } from "../store.js";
import { select, bindControls, esc } from "../ui.js";
import { analyze } from "../advisor.js";
import { Scope, Ticker } from "../scope.js";

let unsub = null;
let scope = null;
let tickers = {};
let windows = [];
let refreshTimer = null;
let elapsedTimer = null;
let startedAt = 0;

export function upscalerOptions() {
  return ["neural", "anime", "edge", "fsr", "nis", "lanczos", "bicubic", "bilinear", "nearest", "pixel"]
    .filter((u) => (u !== "neural" && u !== "anime") || state.neuralBundled)
    .map((u) => ({ v: u, label: t(`upscaler.${u}.name`) }));
}

export function fgOptions() {
  return ["off", "x2", "x3", "x4", "adaptive"].map((v) => ({ v, label: t(`fg.${v}`) }));
}

function profileOptions() {
  return Object.entries(state.settings.profiles).map(([id, p]) => ({ v: id, label: p.name || id }));
}

function chain() {
  const p = profile();
  const s = state.engine.state === "running" ? state.stats : null;
  const hz = state.engine.refresh || primaryMonitor().refresh;
  const short = (u) => t(`upscaler.${u}.name`).replace("MoonUp ", "");
  const mods = [
    { k: "capture", go: "scaling", on: true, v: p.capture === "dda" ? "DXGI" : "WGC", d: p.capture === "dda" ? t("capture.dda") : t("capture.wgc"), st: s ? `${fmt(s.baseFps, 0)} fps` : "" },
    (() => {
      const nr = !!p.render?.enabled, vi = !!p.vision?.enabled;
      const v = nr ? t("nav.render") : vi ? t(`preset.${p.vision.preset || "custom"}`) : t("common.off");
      const d = nr && vi ? `+ ${t("nav.vision")}` : nr ? `${Math.round((p.render.tone ?? 1.0) * 100)}% · ${Math.round((p.render.structure ?? 1.0) * 100)}%` : t("nav.vision");
      return { k: "enhance", go: nr || !vi ? "render" : "vision", on: nr || vi, v, d, st: "" };
    })(),
    { k: "scale", go: "scaling", on: p.scaleMode !== "off", v: p.scaleMode === "off" ? t("common.off") : short(p.upscaler), d: t(`mode.${p.scaleMode}`), st: s ? `${s.src[0]}×${s.src[1]} → ${s.out[0]}×${s.out[1]}` : "" },
    { k: "motion", go: "motion", on: p.frameGen !== "off", v: t(`fg.${p.frameGen}`), d: t(`q.${p.flowQuality}`), st: s && s.baseFps > 0.5 && p.frameGen !== "off" ? `×${fmt(s.outFps / s.baseFps, 2)}` : "" },
    { k: "output", go: "motion", on: true, v: p.vsync ? "V-Sync" : t("motion.tearing"), d: `${hz} Hz`, st: s ? `${fmt(s.outFps, 0)} fps` : "" },
  ];
  return mods
    .map((m, i) => `<button class="mod ${m.on ? "on" : ""}" data-go="${m.go}">
      <span class="label"><i></i>0${i + 1} ${esc(t(`home.pipe.${m.k}`))}</span>
      <span class="mv">${esc(m.v)}</span><span class="ms">${esc(m.d)}</span><span class="mstat">${esc(m.st)}</span></button>`)
    .join("");
}

function targetRows() {
  const picked = state.pickedWindow?.hwnd;
  const auto = `<button class="tgt ${!picked ? "on" : ""}" data-hwnd=""><span class="radio"></span><span class="ph"></span>
      <span><b>${esc(t("home.auto"))}</b><span class="s">${esc(t("home.autoD", { n: state.settings.scaleDelay }))}</span></span></button>`;
  const rows = windows
    .map((w) => `<button class="tgt ${picked === w.hwnd ? "on" : ""}" data-hwnd="${esc(w.hwnd)}"><span class="radio"></span>
      ${w.icon ? `<img src="${w.icon}" alt="">` : `<span class="ph"></span>`}
      <span><b>${esc(w.title)}</b><span class="s">${esc(w.exe)} · ${w.width}×${w.height}</span></span></button>`)
    .join("");
  return auto + rows;
}

function elapsed() {
  if (!startedAt) return "00:00";
  const s = Math.floor((Date.now() - startedAt) / 1000);
  const hh = Math.floor(s / 3600), mm = Math.floor((s % 3600) / 60), ss = s % 60;
  return (hh ? String(hh).padStart(2, "0") + ":" : "") + String(mm).padStart(2, "0") + ":" + String(ss).padStart(2, "0");
}

function engageState() {
  const e = state.engine.state;
  if (state.countdown >= 0) {
    const total = Math.max(1, state.settings.scaleDelay);
    return { cls: "counting", fill: `${(1 - state.countdown / total) * 100}%`, l: t("home.countdown"), c: String(state.countdown), r: t("home.cancel") };
  }
  if (e === "running" || e === "paused" || e === "starting")
    return { cls: "running", fill: "0", l: state.engine.title || "", c: t("home.stop"), r: e === "paused" ? t("status.paused") : elapsed() };
  return { cls: "", fill: "0", l: hotkeyLabel(), c: t("home.scale"), r: state.pickedWindow ? state.pickedWindow.title : t("home.auto") };
}

function renderEngage(root) {
  const g = engageState();
  const el = root.querySelector("#engage");
  if (!el) return;
  el.className = `engage ${g.cls}`;
  el.querySelector(".fill").style.width = g.fill;
  el.querySelector(".l").textContent = g.l;
  el.querySelector(".c").textContent = g.c;
  el.querySelector(".r").textContent = g.r;
}

export function render(root) {
  const recs = analyze().recs.filter((r) => !r.applied && r.sev !== "info").length;
  root.innerHTML = `
  <div class="page-head">
    <div><h1>${esc(t("home.title"))}</h1><p>${esc(t("home.sub"))}</p></div>
    <div class="side"><span class="label">${esc(t("home.profile"))}</span>${select("activeProfile", profileOptions(), { scope: "settings", width: 180 })}</div>
  </div>

  <div class="section">
    <div class="section-head"><span class="label">${esc(t("home.chain"))}</span>
      <span class="aside">${recs ? `<a href="#" data-go="advisor">${esc(t("home.recsLink", { n: recs }))}</a>` : esc(t("home.chainHint"))}</span></div>
    <div class="chain" id="chain">${chain()}</div>
  </div>

  <div class="home-split">
    <div class="section">
      <div class="section-head"><span class="label">${esc(t("home.telemetry"))}</span></div>
      <div class="panel tele">
        <div class="readouts">
          <div class="ro big"><span class="label">${esc(t("stat.output"))}</span><div class="v" id="roOut">—</div></div>
          <div class="ro"><span class="label">${esc(t("stat.base"))}</span><div class="v" id="roBase">—</div></div>
          <div class="ro"><span class="label">${esc(t("stat.mult"))}</span><div class="v" id="roMult">—</div></div>
          <div class="ro"><span class="label">${esc(t("stat.gpu"))}</span><div class="v" id="roGpu">—</div></div>
          <div class="ro"><span class="label">${esc(t("stat.latency"))}</span><div class="v" id="roLat">—</div></div>
        </div>
        <div class="scope-wrap"><canvas class="scope" id="scope"></canvas><div class="scope-msg" id="scopeMsg"><span>${esc(t("home.noSignal"))}</span></div></div>
        <div class="tele-foot"><span>${esc(t("stat.resolution"))} <b id="tfRes">—</b></span><span>${esc(t("stat.capture"))} <b id="tfCap">—</b></span></div>
      </div>
    </div>
    <div class="section">
      <div class="section-head"><span class="label">${esc(t("home.target"))}</span><button class="btn ghost sm" id="tgtRefresh" title="${esc(t("common.refresh"))}">${icon("refresh")}</button></div>
      <div class="panel target-list" id="targets">${targetRows()}</div>
    </div>
  </div>

  <div class="engage-dock"><button class="engage" id="engage"><span class="fill"></span><span class="l"></span><span class="c"></span><span class="r"></span></button></div>`;

  tickers = {
    out: new Ticker(root.querySelector("#roOut"), 0),
    base: new Ticker(root.querySelector("#roBase"), 0),
    mult: new Ticker(root.querySelector("#roMult"), 2),
    gpu: new Ticker(root.querySelector("#roGpu"), 2, "<small>ms</small>"),
    lat: new Ticker(root.querySelector("#roLat"), 1, "<small>ms</small>"),
  };
  scope = new Scope(root.querySelector("#scope"));
  renderEngage(root);
  updateLive(root);

  bindControls(root.querySelector(".page-head"), () => {
    root.querySelector("#chain").innerHTML = chain();
  });
  root.onclick = (e) => {
    const go = e.target.closest("[data-go]");
    if (go) {
      e.preventDefault();
      window.dispatchEvent(new CustomEvent("sw-nav", { detail: go.dataset.go }));
      return;
    }
    const tg = e.target.closest(".tgt");
    if (tg) {
      state.pickedWindow = windows.find((x) => x.hwnd === tg.dataset.hwnd) || null;
      root.querySelector("#targets").innerHTML = targetRows();
      renderEngage(root);
    }
  };
  root.querySelector("#tgtRefresh").onclick = () => loadWindows(root);
  root.querySelector("#engage").onclick = (e) => {
    e.stopPropagation();
    if (state.countdown >= 0) return call("cancelCountdown");
    if (state.engine.state !== "idle") return call("stop");
    if (state.pickedWindow) call("scale", { hwnd: state.pickedWindow.hwnd });
    else call("scale", { delay: state.settings.scaleDelay });
  };

  loadWindows(root);
  clearInterval(refreshTimer);
  refreshTimer = setInterval(() => {
    if (state.engine.state === "idle" && !document.hidden) loadWindows(root);
  }, 4000);

  unsub?.();
  unsub = subscribe((what) => {
    if (!root.querySelector("#engage")) return;
    if (what === "stats") updateLive(root);
    if (what === "engine" || what === "countdown") {
      if (state.engine.state === "running" && !startedAt) startedAt = Date.now();
      if (state.engine.state === "idle") startedAt = 0;
      renderEngage(root);
      root.querySelector("#chain").innerHTML = chain();
      updateLive(root);
    }
  });
  clearInterval(elapsedTimer);
  elapsedTimer = setInterval(() => state.engine.state === "running" && renderEngage(root), 1000);
}

async function loadWindows(root) {
  const list = (await call("listWindows").catch(() => [])) || [];
  windows = list.slice(0, 12);
  if (state.pickedWindow && !windows.find((w) => w.hwnd === state.pickedWindow.hwnd)) state.pickedWindow = null;
  const box = root.querySelector("#targets");
  if (box) box.innerHTML = targetRows();
}

function updateLive(root) {
  if (!root.querySelector("#chain")) return;
  const s = state.engine.state === "running" ? state.stats : null;
  root.querySelector("#chain").classList.toggle("live", !!s);
  tickers.out?.set(s ? s.outFps : null);
  tickers.base?.set(s ? s.baseFps : null);
  tickers.mult?.set(s && s.baseFps > 0.5 ? s.outFps / s.baseFps : null);
  tickers.gpu?.set(s ? s.gpuMs : null);
  tickers.lat?.set(s ? s.delayMs : null);
  root.querySelector("#scopeMsg").style.display = s ? "none" : "";
  root.querySelector("#tfRes").textContent = s ? `${s.src[0]}×${s.src[1]} → ${s.out[0]}×${s.out[1]}` : "—";
  root.querySelector("#tfCap").textContent = s ? s.capture : "—";
  scope?.draw(s ? s.frameTimes || [] : []);
  // Update only the live figures so the signal animation keeps running smoothly.
  const tmp = document.createElement("div");
  tmp.innerHTML = chain();
  const fresh = tmp.querySelectorAll(".mstat");
  root.querySelectorAll("#chain .mstat").forEach((el, i) => {
    if (fresh[i] && el.textContent !== fresh[i].textContent) el.textContent = fresh[i].textContent;
  });
}

export function leave() {
  unsub?.();
  unsub = null;
  clearInterval(refreshTimer);
  clearInterval(elapsedTimer);
}
