// Advisor: reads the hardware description and produces ranked, actionable suggestions.
import { call } from "./bridge.js";
import { t } from "./i18n.js";
import { state, profile, setProfile, primaryMonitor, mainGpu } from "./store.js";

export function gpuTier(g) {
  if (!g) return "entry";
  if (g.integrated) return "entry";
  const n = (g.name || "").toUpperCase();
  const num = (re) => {
    const m = n.match(re);
    return m ? parseInt(m[1], 10) : 0;
  };
  const laptop = /LAPTOP|MOBILE|MAX-Q/.test(n);
  let tier = "mid";
  if (g.vendor === "NVIDIA") {
    const rtx = num(/RTX\s*(\d{4})/);
    if (rtx) {
      const series = Math.floor(rtx / 1000), model = rtx % 100;
      const order = ["entry", "mid", "high", "enthusiast"];
      let i = model >= 80 ? 3 : model >= 70 ? 2 : model >= 60 ? 1 : 0;
      if (series <= 2) i = Math.max(0, i - 1);  // Turing: one class lower
      tier = order[i];
    } else if (/GTX\s*16|GTX\s*10/.test(n)) tier = "entry";
    else if (/MX\s*\d/.test(n)) tier = "entry";
  } else if (g.vendor === "AMD") {
    const rx = num(/RX\s*(\d{4})/);
    if (rx) {
      const model = rx % 1000;
      if (model >= 900) tier = "enthusiast";
      else if (model >= 700) tier = "high";
      else if (model >= 600) tier = "mid";
      else tier = "entry";
    } else tier = g.vramMB >= 8000 ? "mid" : "entry";
  } else if (g.vendor === "Intel") {
    tier = /ARC/.test(n) ? (/B5|A7/.test(n) ? "mid" : "entry") : "entry";
  } else {
    tier = g.vramMB >= 12000 ? "high" : g.vramMB >= 6000 ? "mid" : "entry";
  }
  if (laptop && tier === "enthusiast") tier = "high";
  else if (laptop && tier === "high") tier = "mid";
  return tier;
}

const tierRank = { entry: 0, mid: 1, high: 2, enthusiast: 3 };

// The setup the advisor would choose for this machine.
export function recommendedPlan() {
  const g = mainGpu();
  const tier = gpuTier(g);
  const mon = primaryMonitor();
  const r = tierRank[tier];
  const neural = state.neuralBundled && r >= 2;
  return {
    tier,
    upscaler: neural ? "neural" : "edge",
    flowQuality: r === 0 ? "performance" : r >= 2 ? "quality" : "balanced",
    frameGen: mon.refresh >= 90 ? "adaptive" : "x2",
    sharpness: 0.35,
    refresh: mon.refresh,
  };
}

export function applyPlan(p = profile()) {
  const plan = recommendedPlan();
  p.upscaler = plan.upscaler;
  p.flowQuality = plan.flowQuality;
  p.sharpness = plan.sharpness;
  p.vsync = true;
  return plan;
}

const open = (uri) => () => call("openExternal", { url: uri });

export function analyze() {
  const sys = state.system || {};
  const p = profile();
  const g = mainGpu();
  const mon = primaryMonitor();
  const plan = recommendedPlan();
  const gpus = sys.gpus || [];
  const recs = [];
  const add = (r) => recs.push(r);

  if (mon.refresh >= 90) {
    add({
      id: "adaptive", sev: "tip", weight: 90, title: t("rec.adaptive.t"), body: t("rec.adaptive.b", { hz: mon.refresh }),
      applied: p.frameGen === "adaptive", apply: () => setProfile("frameGen", "adaptive"),
    });
  }
  if (plan.upscaler === "neural") {
    add({
      id: "neural", sev: "tip", weight: 70, title: t("rec.neural.t"), body: t("rec.neural.b", { gpu: g?.name || "GPU" }),
      applied: p.upscaler === "neural", apply: () => setProfile("upscaler", "neural"),
    });
  } else {
    add({
      id: "edge", sev: "tip", weight: 60, title: t("rec.edge.t"), body: t("rec.edge.b"),
      applied: p.upscaler === "edge" || p.upscaler === "neural", apply: () => setProfile("upscaler", "edge"),
    });
  }
  if (plan.flowQuality === "performance") {
    add({
      id: "flowPerf", sev: "tip", weight: 65, title: t("rec.flowPerf.t"), body: t("rec.flowPerf.b"),
      applied: p.flowQuality === "performance", apply: () => setProfile("flowQuality", "performance"),
    });
  } else if (plan.flowQuality === "quality") {
    add({
      id: "flowQuality", sev: "tip", weight: 40, title: t("rec.flowQuality.t"), body: t("rec.flowQuality.b"),
      applied: p.flowQuality === "quality", apply: () => setProfile("flowQuality", "quality"),
    });
  }
  if (tierRank[plan.tier] >= 1) {
    add({
      id: "render", sev: "tip", weight: 75, title: t("rec.render.t"), body: t("rec.render.b"),
      applied: !!p.render?.enabled, apply: () => setProfile("render.enabled", true),
    });
  }
  if (gpus.some((x) => x.integrated) && gpus.some((x) => !x.integrated)) {
    add({ id: "hybrid", sev: "warn", weight: 95, title: t("rec.hybrid.t"), body: t("rec.hybrid.b"), action: t("rec.hybrid.a"), run: open("ms-settings:display-advancedgraphics") });
  }
  if (sys.battery?.onBattery) add({ id: "battery", sev: "warn", weight: 85, title: t("rec.battery.t"), body: t("rec.battery.b") });
  if (sys.os && sys.os.build >= 19041 && sys.hags === false)
    add({ id: "hags", sev: "tip", weight: 45, title: t("rec.hags.t"), body: t("rec.hags.b"), action: t("rec.hags.a"), run: open("ms-settings:display-advancedgraphics") });
  if (sys.gameMode === false)
    add({ id: "gameMode", sev: "tip", weight: 35, title: t("rec.gameMode.t"), body: t("rec.gameMode.b"), action: t("rec.gameMode.a"), run: open("ms-settings:gaming-gamemode") });
  if (mon.maxRefresh > mon.refresh + 1)
    add({ id: "refresh", sev: "warn", weight: 88, title: t("rec.refresh.t", { max: mon.maxRefresh }), body: t("rec.refresh.b", { hz: mon.refresh }), action: t("rec.refresh.a"), run: open("ms-settings:display-advanced") });
  if (sys.wgc === false) add({ id: "wgc", sev: "warn", weight: 99, title: t("rec.wgc.t"), body: t("rec.wgc.b") });
  if (g && !g.integrated && g.vramMB > 0 && g.vramMB < 4000)
    add({ id: "vram", sev: "info", weight: 30, title: t("rec.vram.t"), body: t("rec.vram.b") });
  const lastFps = state.settings?.lastSession?.baseFps;
  if (lastFps && lastFps < 35) add({ id: "lowfps", sev: "warn", weight: 80, title: t("rec.lowfps.t"), body: t("rec.lowfps.b", { fps: Math.round(lastFps) }) });
  if (p.scaleMode !== "off" && p.sharpness < 0.05)
    add({ id: "sharp", sev: "tip", weight: 25, title: t("rec.sharp.t"), body: t("rec.sharp.b"), applied: false, apply: () => setProfile("sharpness", 0.35) });
  if (p.frameGen !== "off" && !p.vsync)
    add({ id: "vsync", sev: "tip", weight: 50, title: t("rec.vsync.t"), body: t("rec.vsync.b"), applied: false, apply: () => setProfile("vsync", true) });
  add({ id: "borderless", sev: "info", weight: 20, title: t("rec.borderless.t"), body: t("rec.borderless.b") });
  if (g && (g.vendor === "NVIDIA" || g.vendor === "AMD" || g.vendor === "Intel"))
    add({ id: "dlss", sev: "info", weight: 10, title: t("rec.dlss.t"), body: t("rec.dlss.b") });

  recs.sort((a, b) => (!!a.applied === !!b.applied ? b.weight - a.weight : a.applied ? 1 : -1));
  return { tier: plan.tier, plan, recs };
}
