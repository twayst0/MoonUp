// MoonUp UI bootstrap: shell, navigation, native events, intro and first-run setup.
import { call, send, on, isNative, setMockSettings } from "./bridge.js";
import { icon } from "./icons.js";
import { t, loadLanguage, detectLanguage, lang } from "./i18n.js";
import { state, notify, saveNow, saveSoon, defaultSettings, addTestProfiles, mainGpu, primaryMonitor } from "./store.js";
import { esc, toast } from "./ui.js";
import { playIntro } from "./intro.js";
import { runOnboarding } from "./onboarding.js";

const PAGES = [
  { id: "home", icon: "home" },
  { id: "scaling", icon: "scaling" },
  { id: "render", icon: "render" },
  { id: "upgraph", icon: "sparkle" },
  { id: "motion", icon: "motion" },
  { id: "vision", icon: "vision" },
  { id: "sep" },
  { id: "profiles", icon: "profiles" },
  { id: "advisor", icon: "advisor" },
  { id: "lab", icon: "lab" },
  { id: "sep" },
  { id: "settings", icon: "settings" },
  { id: "about", icon: "about" },
];

const modules = {};
let currentModule = null;

async function loadPage(id) {
  if (!modules[id]) modules[id] = await import(`./pages/${id}.js`);
  return modules[id];
}

export async function go(id, { instant = false } = {}) {
  const mod = await loadPage(id);
  currentModule?.leave?.();
  state.page = id;
  const page = document.getElementById("page");
  page.classList.remove("page-enter");
  page.scrollTop = 0;
  mod.render(page);
  if (!instant) {
    void page.offsetWidth;
    page.classList.add("page-enter");
  }
  currentModule = mod;
  document.querySelectorAll(".nav-item").forEach((n) => n.classList.toggle("active", n.dataset.page === id));
  moveIndicator();
}

function moveIndicator() {
  const ind = document.querySelector(".nav-indicator");
  const act = document.querySelector(".nav-item.active");
  if (!ind || !act) return;
  ind.style.transform = `translateY(${act.offsetTop}px)`;
}

function renderNav() {
  const nav = document.getElementById("nav");
  nav.innerHTML =
    `<span class="nav-indicator"></span>` +
    PAGES.map((p) =>
      p.id === "sep"
        ? `<div class="nav-sep"></div>`
        : `<button class="nav-item ${state.page === p.id ? "active" : ""}" data-page="${p.id}">${icon(p.icon)}<span>${esc(t(`nav.${p.id}`))}</span>${
            p.id === "advisor" ? `<span class="badge hidden" id="advBadge"></span>` : ""
          }</button>`
    ).join("");
  nav.querySelectorAll(".nav-item").forEach((b) => b.addEventListener("click", () => go(b.dataset.page)));
  requestAnimationFrame(moveIndicator);
}

function renderSideFoot() {
  const g = mainGpu();
  const m = primaryMonitor();
  const vendor = (g?.vendor || "").toLowerCase();
  document.getElementById("sideFoot").innerHTML = g
    ? `<div class="sys-block">
        <span class="label">${esc(t("side.gpu"))}</span>
        <span class="v"><span class="vendor ${vendor}"></span>${esc(g.name.replace(/\(R\)|\(TM\)/g, "").replace(/\s+/g, " "))}</span>
        <span class="m">${g.vramMB ? (g.vramMB / 1024).toFixed(g.vramMB >= 10240 ? 0 : 1) + " GB · " : ""}${m.width}×${m.height} · ${m.refresh} Hz</span>
      </div>`
    : "";
}

function renderStatus() {
  const chip = document.getElementById("statusChip");
  const label = document.getElementById("statusLabel");
  chip.className = "status-chip";
  const e = state.engine;
  // Keep the GPU free for the game: the ambient field stops drifting while scaling runs.
  document.body.classList.toggle("calm", e.state === "running" || e.state === "starting" || e.state === "paused");
  if (state.countdown >= 0) {
    chip.classList.add("count");
    label.textContent = t("status.countdown", { n: state.countdown });
  } else if (e.state === "running") {
    chip.classList.add("live");
    label.textContent = t("status.running", { title: e.title || e.exe || "" });
  } else if (e.state === "paused") {
    chip.classList.add("paused");
    label.textContent = t("status.paused");
  } else if (e.state === "starting") {
    chip.classList.add("count");
    label.textContent = t("status.starting");
  } else {
    label.textContent = t("status.idle");
  }
}

function applyAppearance() {
  const s = state.settings;
  // MoonUp: the moonlight theme replaces the old default blue once.
  if (!s.moonTheme) {
    if (!s.accent || s.accent === "azure") s.accent = "moon";
    s.moonTheme = true;
    saveSoon();
  }
  document.documentElement.dataset.accent = s.accent || "moon";
  document.body.classList.toggle("reduced-motion", !!s.reducedMotion);
}

export async function applyLanguage(id) {
  await loadLanguage(id);
  renderNav();
  renderSideFoot();
  renderStatus();
  const skip = document.getElementById("introSkip");
  if (skip) skip.textContent = t("intro.skip");
  call("trayStrings", { open: t("tray.open"), stop: t("tray.stop"), quit: t("tray.quit"), display: t("fps.display") }).catch(() => {});
  if (state.page) go(state.page, { instant: true });
}

function wireWindowChrome() {
  document.querySelectorAll("[data-drag]").forEach((el) => {
    el.addEventListener("mousedown", (e) => {
      if (e.button !== 0 || e.detail > 1) return;
      send("window", { action: "drag" });
    });
    el.addEventListener("dblclick", () => call("window", { action: "maximize" }));
  });
  document.querySelectorAll(".rz").forEach((el) =>
    el.addEventListener("mousedown", (e) => {
      if (e.button === 0) send("window", { action: "resize", edge: el.dataset.edge });
    })
  );
  document.getElementById("wcMin").onclick = () => call("window", { action: "minimize" });
  document.getElementById("wcMax").onclick = () => call("window", { action: "maximize" });
  document.getElementById("wcClose").onclick = () => call("window", { action: "close" });
}

function setMaximized(m) {
  state.maximized = m;
  document.body.classList.toggle("maximized", m);
  document.getElementById("wcMax").innerHTML = m
    ? `<svg viewBox="0 0 10 10"><rect x="1.5" y="3" width="5.5" height="5.5" rx="1"/><path d="M3 3V1.8h5.2V7H7"/></svg>`
    : `<svg viewBox="0 0 10 10"><rect x="1.5" y="1.5" width="7" height="7" rx="1"/></svg>`;
}

// Average of captured fps during a session, remembered for the advisor.
let sessionFps = [];

function wireEvents() {
  on("countdown", (e) => {
    state.countdown = e.remaining;
    if (e.remaining < 0) state.countdown = -1;
    renderStatus();
    notify("countdown");
  });
  on("scaling", (e) => {
    state.countdown = -1;
    state.engine = { ...state.engine, state: "starting", title: e.title, exe: e.exe, profile: e.profile };
    sessionFps = [];
    renderStatus();
    notify("engine");
  });
  on("engine", (e) => {
    const prev = state.engine.state;
    if (e.state === "running") {
      state.engine = { ...state.engine, state: "running", capture: e.capture || state.engine.capture, gpu: e.gpu || state.engine.gpu, refresh: e.refresh || state.engine.refresh };
      if (prev === "starting") toast(t("toast.started", { title: state.engine.title || state.engine.exe }), "ok");
    } else if (e.state === "paused") {
      state.engine = { ...state.engine, state: "paused" };
    } else if (e.state === "starting") {
      state.engine = { ...state.engine, state: "starting" };
    } else if (e.state === "error") {
      const msg = t(`err.${e.code}`, { m: e.message || "" });
      toast(msg === `err.${e.code}` ? t("err.generic", { m: e.message || e.code }) : msg, "bad");
      // Every engine error ends the session; the engine also sends "stopped" when it can.
      state.engine = { state: "idle", title: "", exe: "", profile: "", capture: "", gpu: "", refresh: 0 };
      state.stats = null;
      state.countdown = -1;
    } else if (e.state === "stopped") {
      if (sessionFps.length > 4) {
        const avg = sessionFps.reduce((a, b) => a + b, 0) / sessionFps.length;
        state.settings.lastSession = { ...(state.settings.lastSession || {}), baseFps: Math.round(avg * 10) / 10 };
        saveNow();
      }
      if (e.reason === "window_closed") toast(t("err.window_closed"), "warn");
      else if (prev !== "idle" && e.reason !== "error") toast(t("toast.stopped"));
      state.engine = { state: "idle", title: "", exe: "", profile: "", capture: "", gpu: "", refresh: 0 };
      state.stats = null;
    }
    renderStatus();
    notify("engine");
  });
  on("stats", (e) => {
    state.stats = e;
    if (e.baseFps > 1) sessionFps.push(e.baseFps);
    if (sessionFps.length > 600) sessionFps.shift();
    notify("stats");
  });
  on("window", (e) => setMaximized(!!e.maximized));
  on("notice", (e) => {
    const key = `notice.${e.code}`;
    const msg = t(key, e);
    if (msg !== key) toast(msg, e.code === "resize_ok" ? "ok" : "warn");
  });
  on("hotkey", (e) => !e.ok && toast(t("err.hotkey"), "warn"));
  window.addEventListener("sw-nav", (e) => go(e.detail));
  window.addEventListener("resize", moveIndicator);
}

async function boot() {
  if (!isNative) setMockSettings(defaultSettings());
  const init = await call("init");
  state.settings = init.settings || defaultSettings();
  state.system = init.system;
  state.version = init.version;
  state.locale = init.locale;
  state.neuralBundled = !!init.neuralBundled;
  if (init.engine?.running) state.engine = { ...state.engine, state: "running", title: init.engine.target };

  const language = state.settings.language || detectLanguage(init.locale);
  await loadLanguage(language);
  if (addTestProfiles(state.settings, (n) => t(`testp.${n}`))) saveSoon();
  applyAppearance();
  setMaximized(!!init.maximized);
  renderNav();
  renderSideFoot();
  renderStatus();
  wireWindowChrome();
  wireEvents();
  call("trayStrings", { open: t("tray.open"), stop: t("tray.stop"), quit: t("tray.quit"), display: t("fps.display") }).catch(() => {});
  const skip = document.getElementById("introSkip");
  if (skip) skip.textContent = t("intro.skip");

  await go("home", { instant: true });
  // Scan the Upgraph game library in the background so its page opens instantly.
  if (isNative)
    setTimeout(() => loadPage("upgraph").then((m) => m.prefetch?.()).catch(() => {}), 1500);

  const showIntro = state.settings.intro !== false && !state.settings.reducedMotion && !init.startMinimized;
  if (showIntro) {
    // The native splash shows the same first frame (the logo at 168 px); it fades out as the sequence starts.
    const p = playIntro();
    call("uiReady").catch(() => {});
    await p;
  } else {
    call("uiReady").catch(() => {});
    document.getElementById("intro").remove();
    document.body.classList.remove("booting");
  }
  if (state.settings.firstRun) {
    await runOnboarding();
  }
}

window.addEventListener("sw-language", (e) => applyLanguage(e.detail));
window.addEventListener("sw-appearance", applyAppearance);
window.addEventListener("sw-rerender", () => go(state.page, { instant: true }));
window.addEventListener("contextmenu", (e) => {
  if (!e.target.closest("input, textarea")) e.preventDefault();
});
boot().catch((err) => {
  console.error(err);
  document.body.classList.remove("booting");
  document.getElementById("intro")?.remove();
  call("uiReady").catch(() => {});
});

export { lang };
