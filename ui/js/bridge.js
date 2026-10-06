// Native bridge: request/response over WebView2 messages plus an event bus.
// Outside WebView2 (plain browser preview) a mock backend keeps the UI fully usable.

const listeners = new Map();
const pending = new Map();
let seq = 1;

const native = typeof window !== "undefined" && window.chrome && window.chrome.webview;

function dispatch(msg) {
  if (msg && typeof msg.id === "number" && pending.has(msg.id)) {
    const p = pending.get(msg.id);
    pending.delete(msg.id);
    msg.ok ? p.resolve(msg.data) : p.reject(new Error(msg.error || "error"));
    return;
  }
  if (msg && msg.event) {
    (listeners.get(msg.event) || []).forEach((fn) => fn(msg));
    (listeners.get("*") || []).forEach((fn) => fn(msg));
  }
}

if (native) {
  window.chrome.webview.addEventListener("message", (e) => dispatch(e.data));
}

export function call(cmd, args = {}) {
  const id = seq++;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    const msg = JSON.stringify({ id, cmd, args });
    if (native) window.chrome.webview.postMessage(msg);
    else mock(id, cmd, args);
  });
}

// Fire-and-forget (no reply expected, used for window dragging where latency matters).
export function send(cmd, args = {}) {
  const msg = JSON.stringify({ id: null, cmd, args });
  if (native) window.chrome.webview.postMessage(msg);
}

export function on(event, fn) {
  if (!listeners.has(event)) listeners.set(event, []);
  listeners.get(event).push(fn);
  return () => listeners.set(event, listeners.get(event).filter((f) => f !== fn));
}

export const isNative = !!native;

// ------------------------------------------------------------------------------------- mock
let mockSettings = null;
let mockTimer = null;

function mockSystem() {
  return {
    gpus: [
      { index: 0, name: "NVIDIA GeForce RTX 4060 Laptop GPU", vendor: "NVIDIA", vendorId: 4318, vramMB: 8188, integrated: false },
      { index: 1, name: "Intel(R) UHD Graphics", vendor: "Intel", vendorId: 32902, vramMB: 128, integrated: true },
    ],
    monitors: [{ name: "Generic PnP Monitor", width: 1920, height: 1080, refresh: 144, maxRefresh: 144, primary: true }],
    cpu: "13th Gen Intel(R) Core(TM) i7-13650HX",
    cpuThreads: 20,
    ramMB: 16084,
    os: { major: 10, minor: 0, build: 22631, windows11: true },
    wgc: true,
    captureExclusion: true,
    hags: true,
    gameMode: true,
    battery: { present: true, onBattery: false, percent: 87 },
    version: "1.0.0",
  };
}

function reply(id, data, ok = true) {
  setTimeout(() => dispatch(ok ? { id, ok: true, data } : { id, ok: false, error: data }), 30);
}

function mock(id, cmd, args) {
  switch (cmd) {
    case "init":
      return reply(id, {
        settings: mockSettings,
        system: mockSystem(),
        version: "1.0.0",
        locale: navigator.language || "en-US",
        neuralBundled: true,
        engine: { running: false, target: "" },
        maximized: false,
      });
    case "saveSettings":
      mockSettings = args.settings;
      return reply(id, null);
    case "listWindows":
      return reply(id, [
        { hwnd: "1001", title: "Hollow Knight", exe: "hollow_knight.exe", width: 1280, height: 720, icon: "" },
        { hwnd: "1002", title: "Elden Ring", exe: "eldenring.exe", width: 1600, height: 900, icon: "" },
        { hwnd: "1003", title: "Stardew Valley", exe: "Stardew Valley.exe", width: 960, height: 540, icon: "" },
      ]);
    case "scale": {
      reply(id, null);
      let n = args.hwnd ? 0 : args.delay ?? 5;
      const tick = () => {
        dispatch({ event: "countdown", remaining: n });
        if (n <= 0) return startMockEngine(args.hwnd ? "Elden Ring" : "Hollow Knight");
        n--;
        setTimeout(tick, 1000);
      };
      if (args.hwnd) startMockEngine("Elden Ring");
      else tick();
      return;
    }
    case "stop":
      clearInterval(mockTimer);
      dispatch({ event: "engine", state: "stopped", reason: "user" });
      return reply(id, null);
    case "systemInfo":
      return reply(id, mockSystem());
    case "upgraphLibrary":
      return setTimeout(() => reply(id, [
        { launcher: "Steam", id: "1", name: "Cyberpunk 2077", dir: "D:\\Steam\\steamapps\\common\\Cyberpunk 2077", poster: "", installed: true },
        { launcher: "Steam", id: "2", name: "Chameleon", dir: "D:\\Steam\\steamapps\\common\\Chameleon", poster: "", installed: false },
        { launcher: "Epic Games", id: "3", name: "Alan Wake 2", dir: "E:\\Epic\\AlanWake2", poster: "", installed: false },
        { launcher: "GOG", id: "4", name: "The Witcher 3", dir: "E:\\GOG\\The Witcher 3", poster: "", installed: false },
      ]), 300);
    case "upgraphScan":
      return setTimeout(() => reply(id, {
        dir: args.dir, name: "Chameleon", antiCheat: false, unreal: true, reshadeCached: "",
        candidates: [{ path: args.dir + "\\PenguinHotel\\Binaries\\Win64\\PenguinHotel-Win64-Shipping.exe", rel: "PenguinHotel\\Binaries\\Win64\\PenguinHotel-Win64-Shipping.exe", name: "PenguinHotel-Win64-Shipping.exe", bits: 64, api: "dxgi", apiLabel: "DirectX 12", via: "imports" }],
        exe: { path: args.dir + "\\PenguinHotel\\Binaries\\Win64\\PenguinHotel-Win64-Shipping.exe", rel: "PenguinHotel\\Binaries\\Win64\\PenguinHotel-Win64-Shipping.exe", name: "PenguinHotel-Win64-Shipping.exe", bits: 64, api: "dxgi", apiLabel: "DirectX 12", via: "imports" },
        upscalers: [{ kind: "DLSS", file: "nvngx_dlss.dll", version: "3.7.10.0" }, { kind: "FSR", file: "amd_fidelityfx_dx12.dll", version: "1.1.2" }],
        status: { installed: false, hooks: [] },
        gpus: [{ name: "NVIDIA GeForce RTX 4070", vendor: 4318, tier: "rtx", driver: "616.64" }], dlss5Custom: "",
      }), 300);
    default:
      return reply(id, null);
  }
}

function startMockEngine(title) {
  dispatch({ event: "scaling", title, exe: "game.exe", profile: "default" });
  dispatch({ event: "engine", state: "running", capture: "Windows Graphics Capture", gpu: "NVIDIA GeForce RTX 4060 Laptop GPU", refresh: 144 });
  let t = 0;
  clearInterval(mockTimer);
  mockTimer = setInterval(() => {
    t++;
    const base = 58 + Math.sin(t / 3) * 3;
    const ft = Array.from({ length: 60 }, (_, i) => 6.9 + Math.sin((t * 7 + i) / 5) * 0.6 + (i % 17 === 0 ? 3 : 0));
    dispatch({
      event: "stats", baseFps: +base.toFixed(1), outFps: 143.6, gpuMs: 1.84, delayMs: 17.4,
      src: [1280, 720], out: [1920, 1080], upscaler: "edge", neural: true, capture: "Windows Graphics Capture", refresh: 144, frameTimes: ft,
    });
  }, 500);
}

export function setMockSettings(s) {
  mockSettings = s;
}
