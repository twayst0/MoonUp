// Startup sequence. The logo starts exactly where and how the native splash drew it (168 px,
// centred), shrinks into a liquid-glass tile that forms around it, the wordmark comes into focus
// letter by letter, and on exit the logo glides into the title bar while the app fades in.
// Everything is transform/opacity/filter on a handful of elements, so it stays smooth in WebView2.
// About 2.8 s, skippable with a click or any key.
import { t } from "./i18n.js";
import { isNative } from "./bridge.js";

const css = (name, fallback) => getComputedStyle(document.documentElement).getPropertyValue(name).trim() || fallback;
const supportsLinear = window.CSS?.supports?.("transition-timing-function", "linear(0, 1)");
const SPRING = supportsLinear ? css("--spring", "cubic-bezier(0.2, 0.9, 0.25, 1)") : "cubic-bezier(0.2, 0.9, 0.25, 1)";
const BOUNCY = supportsLinear ? css("--spring-bouncy", "cubic-bezier(0.3, 1.3, 0.4, 1)") : "cubic-bezier(0.3, 1.3, 0.4, 1)";
const OUT = "cubic-bezier(0.16, 1, 0.3, 1)";
const SMOOTH = "cubic-bezier(0.45, 0, 0.2, 1)";

const SPLASH_PX = 168; // matches the native splash (168 dip)
const LOGO_PX = 128;   // logo size inside the tile

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function playIntro() {
  const intro = document.getElementById("intro");
  const veil = document.getElementById("introVeil");
  const stage = document.getElementById("introStage");
  const tile = document.getElementById("introTile");
  const sheen = document.getElementById("introSheen");
  const logo = document.getElementById("introLogo");
  const word = document.getElementById("introWord");
  const tag = document.getElementById("introTag");
  const app = document.getElementById("app");
  const tbLogo = document.getElementById("tbLogo");

  document.getElementById("introSkip").textContent = t("intro.skip");
  tag.textContent = t("intro.tag");
  word.innerHTML = "MoonUp".split("").map((c) => `<span>${c}</span>`).join("");
  const letters = [...word.children];

  // The tile starts centred on screen (where the splash logo is) and moves up once the
  // wordmark arrives, so the whole lock-up ends up centred.
  const sr = stage.getBoundingClientRect();
  const tr = tile.getBoundingClientRect();
  const lift = Math.round(sr.top + sr.height / 2 - (tr.top + tr.height / 2));
  const startScale = SPLASH_PX / LOGO_PX;

  const running = [];
  const anim = (el, frames, opts) => {
    const a = el.animate(frames, { fill: "both", ...opts });
    running.push(a);
    return a;
  };

  // First frame: identical to the native splash.
  stage.style.transform = `translate(-50%, calc(-50% + ${lift}px))`;
  logo.style.transform = `scale(${startScale})`;
  if (!isNative) anim(logo, [{ opacity: 0 }, { opacity: 1 }], { duration: 260, easing: OUT });

  let leaving = false;
  let resolveDone;
  const done = new Promise((r) => (resolveDone = r));

  const leave = async (fast) => {
    if (leaving) return;
    leaving = true;
    intro.classList.add("leaving");
    for (const a of running) {
      if (a.playState === "running" || a.playState === "pending") a.finish();
    }
    const d = fast ? 420 : 760;

    // Reveal the app underneath.
    document.body.classList.remove("booting");
    app.classList.add("enter");
    tbLogo.style.opacity = "0";

    // Fly the logo into the title bar (FLIP).
    const from = logo.getBoundingClientRect();
    const to = tbLogo.getBoundingClientRect();
    const dx = to.left + to.width / 2 - (from.left + from.width / 2);
    const dy = to.top + to.height / 2 - (from.top + from.height / 2);
    const s = to.width / from.width;
    const base = getComputedStyle(logo).transform;
    const baseT = base === "none" ? "" : base;
    logo.animate(
      [{ transform: `${baseT}` }, { transform: `translate(${dx}px, ${dy}px) ${baseT} scale(${s})` }],
      { duration: d, easing: SMOOTH, fill: "forwards" }
    );
    logo.animate([{ filter: "drop-shadow(0 8px 24px rgba(30, 80, 255, 0.45))" }, { filter: "drop-shadow(0 2px 6px rgba(40, 90, 255, 0.35))" }], { duration: d, fill: "forwards" });

    tile.animate(
      [{ opacity: 1, transform: "scale(1)" }, { opacity: 0, transform: "scale(0.86)" }],
      { duration: d * 0.6, easing: SMOOTH, fill: "forwards" }
    );
    const textOut = [{ opacity: 1, transform: "none", filter: "blur(0)" }, { opacity: 0, transform: "translateY(-8px)", filter: "blur(6px)" }];
    word.animate(textOut, { duration: d * 0.55, easing: SMOOTH, fill: "forwards" });
    tag.animate(textOut, { duration: d * 0.45, easing: SMOOTH, fill: "forwards" });
    veil.animate([{ opacity: getComputedStyle(veil).opacity }, { opacity: 0 }], { duration: d * 0.5, fill: "forwards" });

    await sleep(d);
    tbLogo.style.opacity = "";
    tbLogo.animate([{ opacity: 0.4 }, { opacity: 1 }], { duration: 160 });
    intro.remove();
    setTimeout(() => app.classList.remove("enter"), 200);
    resolveDone();
  };

  intro.addEventListener("click", () => leave(true));
  document.addEventListener("keydown", () => leave(true), { once: true });

  await Promise.race([document.fonts.ready, sleep(400)]).catch(() => {});
  if (leaving) return done;
  setTimeout(() => !leaving && intro.classList.add("show-skip"), 900);

  // 1. The room lights up: the veil lifts off the ambient field.
  anim(veil, [{ opacity: 1 }, { opacity: 0 }], { duration: 1400, delay: 100, easing: SMOOTH });

  // 2. The logo settles into the glass tile that forms around it.
  anim(logo, [{ transform: `scale(${startScale})` }, { transform: "scale(1)" }], { duration: 1000, delay: 120, easing: BOUNCY });
  anim(tile, [
    { opacity: 0, transform: "scale(0.7)", filter: "blur(10px)" },
    { opacity: 1, transform: "scale(1)", filter: "blur(0)" },
  ], { duration: 1000, delay: 160, easing: BOUNCY });

  // 3. A highlight passes across the glass.
  anim(sheen, [
    { transform: "translateX(-160%) skewX(-16deg)" },
    { transform: "translateX(300%) skewX(-16deg)" },
  ], { duration: 1100, delay: 650, easing: SMOOTH });

  // 4. The lock-up rises to make room, and the wordmark focuses in.
  anim(stage, [
    { transform: `translate(-50%, calc(-50% + ${lift}px))` },
    { transform: "translate(-50%, -50%)" },
  ], { duration: 1100, delay: 820, easing: SPRING });
  letters.forEach((el, i) =>
    anim(el, [
      { opacity: 0, transform: "translateY(0.32em) scale(0.96)", filter: "blur(10px)" },
      { opacity: 1, transform: "none", filter: "blur(0)" },
    ], { duration: 820, delay: 940 + i * 48, easing: OUT })
  );
  anim(tag, [
    { opacity: 0, transform: "translateY(6px)", letterSpacing: "0.12em" },
    { opacity: 1, transform: "none", letterSpacing: "0.01em" },
  ], { duration: 900, delay: 1420, easing: OUT });

  setTimeout(() => leave(false), 2780);
  return done;
}
