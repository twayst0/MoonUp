// Application state: settings, system information, engine status and live statistics.
import { call } from "./bridge.js";

const subs = new Set();

export const state = {
  settings: null,
  system: null,
  version: "",
  locale: "en-US",
  neuralBundled: false,
  engine: { state: "idle", title: "", exe: "", profile: "", capture: "", gpu: "", refresh: 0 },
  stats: null,
  countdown: -1,
  pickedWindow: null, // { hwnd, title, exe, icon }
  page: "home",
  maximized: false,
};

export function subscribe(fn) {
  subs.add(fn);
  return () => subs.delete(fn);
}

export function notify(what) {
  subs.forEach((fn) => fn(what));
}

let saveTimer = null;
export function saveSoon(delay = 250) {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => call("saveSettings", { settings: state.settings }).catch(() => {}), delay);
}

export function saveNow() {
  clearTimeout(saveTimer);
  return call("saveSettings", { settings: state.settings }).catch(() => {});
}

export function activeProfileId() {
  const s = state.settings;
  // While scaling, edits go to the profile in use for that game.
  if (state.engine.state !== "idle" && state.engine.profile && s.profiles[state.engine.profile]) return state.engine.profile;
  return s.profiles[s.activeProfile] ? s.activeProfile : Object.keys(s.profiles)[0];
}

export function profile() {
  const p = state.settings.profiles[activeProfileId()];
  // Neural Render v2 controls (tone / colour / structure) replace v1's strength / lighting.
  if (p && (!p.render || p.render.tone === undefined)) {
    const old = p.render || {};
    p.render = { enabled: !!old.enabled, tone: 1.0, color: old.color ?? 0.8, structure: 1.0, temporal: old.temporal ?? 0.7 };
  }
  return p;
}

// Updates a (possibly nested, dot separated) key in the active profile and persists it.
export function setProfile(path, value) {
  const p = profile();
  const keys = path.split(".");
  let o = p;
  for (let i = 0; i < keys.length - 1; i++) o = o[keys[i]] ??= {};
  o[keys[keys.length - 1]] = value;
  saveSoon();
  notify("profile");
}

export function setSetting(path, value) {
  const keys = path.split(".");
  let o = state.settings;
  for (let i = 0; i < keys.length - 1; i++) o = o[keys[i]] ??= {};
  o[keys[keys.length - 1]] = value;
  saveSoon();
  notify("settings");
}

export function hotkeyLabel() {
  const h = state.settings?.hotkey || {};
  const parts = [];
  if (h.ctrl) parts.push("Ctrl");
  if (h.alt) parts.push("Alt");
  if (h.shift) parts.push("Shift");
  parts.push(h.key || "S");
  return parts.join(" + ");
}

export function defaultProfile(name) {
  return {
    name, match: "", capture: "wgc", adapter: -1, scaleMode: "auto", factor: 1.5, upscaler: "edge", sharpness: 0.35,
    frameGen: "off", targetFps: 0, flowQuality: "balanced", hudProtect: true, sceneCut: true,
    vision: { enabled: false, preset: "natural", clarity: 0.35, detail: 0.25, vibrance: 0.2, contrast: 0.1, warmth: 0, brightness: 0 },
    render: { enabled: false, tone: 1.0, color: 0.8, structure: 1.0, temporal: 0.7 },
    vsync: true, allowTearing: false, maxLatency: 1, showFps: true, showGraph: true, hudPosition: "tl",
    drawCursor: true, clipCursor: true, pauseWhenUnfocused: true, renderScale: 100,
  };
}

// Five ready-made test profiles (only the test1..test5 ids are written; the user's own profiles are
// never touched). Bump TEST_PRESETS to refresh them after a change.
const TEST_PRESETS = 3;
export function addTestProfiles(s, name) {
  if (!s || s.testPresets >= TEST_PRESETS) return false;
  s.profiles = s.profiles || {};
  const r = (enabled, tone, color, structure) => ({ enabled, tone, color, structure, temporal: 0.7 });
  const presets = {
    test1: { upscaler: "fsr", sharpness: 0.35, frameGen: "x2", flowQuality: "balanced", render: r(false, 1.0, 0.8, 1.0) },
    test2: { upscaler: "neural", sharpness: 0.2, frameGen: "x2", flowQuality: "quality", render: r(false, 1.0, 0.8, 1.0) },
    test3: { upscaler: "fsr", sharpness: 0.3, frameGen: "x2", flowQuality: "balanced", render: r(true, 1.0, 0.8, 1.0) },
    test4: { upscaler: "nis", sharpness: 0.5, frameGen: "x3", flowQuality: "performance", render: r(false, 1.0, 0.8, 1.0) },
    test5: { upscaler: "neural", sharpness: 0.25, frameGen: "adaptive", targetFps: 0, flowQuality: "quality", render: r(true, 1.5, 0.9, 1.5) },
  };
  Object.entries(presets).forEach(([id, o], i) => {
    const old = s.profiles[id];
    s.profiles[id] = { ...defaultProfile(name(i + 1)), ...o, match: old?.match || "" };
  });
  s.testPresets = TEST_PRESETS;
  return true;
}

export function defaultSettings() {
  return {
    version: 1, language: "", intro: true, reducedMotion: false, accent: "moon", minimizeToTray: true, closeToTray: false,
    startWithWindows: false, startMinimized: false, hotkey: { ctrl: true, alt: true, shift: false, key: "S" }, scaleDelay: 5,
    activeProfile: "default", profiles: { default: defaultProfile("Default") }, lastSession: {}, firstRun: true,
  };
}

export function primaryMonitor() {
  const m = state.system?.monitors || [];
  return m.find((x) => x.primary) || m[0] || { width: 1920, height: 1080, refresh: 60, maxRefresh: 60 };
}

export function mainGpu() {
  const g = state.system?.gpus || [];
  return g.find((x) => !x.integrated) || g[0] || null;
}
