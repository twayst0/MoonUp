// Profiles: per-game settings linked to an executable.
import { call } from "../bridge.js";
import { icon } from "../icons.js";
import { t } from "../i18n.js";
import { state, saveNow, defaultProfile, notify } from "../store.js";
import { modal, confirmDialog, promptDialog, toast, esc } from "../ui.js";

function summary(p) {
  const parts = [];
  parts.push(p.scaleMode === "off" ? t("mode.off") : t(`upscaler.${p.upscaler}.name`).replace("MoonUp ", ""));
  parts.push(t(`fg.${p.frameGen}`));
  if (p.vision?.enabled) parts.push(t("nav.vision"));
  parts.push(p.capture === "dda" ? "DXGI" : "WGC");
  return parts.join(" · ");
}

function rows() {
  const s = state.settings;
  return Object.entries(s.profiles)
    .map(([id, p]) => {
      const active = s.activeProfile === id;
      return `<div class="prof ${active ? "on" : ""}" data-id="${esc(id)}">
        <span class="radio"></span>
        <div class="pn"><b>${esc(p.name)}</b><span>${p.match ? esc(t("profiles.matched", { exe: p.match })) : esc(t("profiles.matchNone"))}</span></div>
        <div class="ps mono">${esc(summary(p))}</div>
        <div class="pa">
          <button class="btn ghost sm" data-act="link" title="${esc(t("profiles.link"))}">${icon("link")}</button>
          <button class="btn ghost sm" data-act="rename" title="${esc(t("profiles.rename"))}">${icon("edit")}</button>
          <button class="btn ghost sm" data-act="dup" title="${esc(t("profiles.duplicate"))}">${icon("copy")}</button>
          <button class="btn ghost sm" data-act="export" title="${esc(t("profiles.export"))}">${icon("external")}</button>
          <button class="btn ghost sm" data-act="del" title="${esc(t("profiles.delete"))}" ${Object.keys(s.profiles).length < 2 ? "disabled" : ""}>${icon("trash")}</button>
        </div>
      </div>`;
    })
    .join("");
}

function newId() {
  return "p" + Date.now().toString(36);
}

function pickWindow() {
  return new Promise((resolve) => {
    let chosen = null;
    modal({
      title: esc(t("profiles.pickGame")),
      width: 560,
      body: `<div class="win-list" id="pkList"><div class="empty">…</div></div>`,
      onMount: async (m, close) => {
        const list = (await call("listWindows").catch(() => [])) || [];
        const box = m.querySelector("#pkList");
        box.innerHTML = list.length
          ? list.map((w, i) => `<div class="win-item" data-i="${i}">${w.icon ? `<img src="${w.icon}" alt="">` : `<span class="ph"></span>`}<div><b>${esc(w.title)}</b><span>${esc(w.exe)}</span></div><span class="mono faint">${w.width}×${w.height}</span></div>`).join("")
          : `<div class="empty">${esc(t("common.none"))}</div>`;
        box.addEventListener("click", (e) => {
          const it = e.target.closest(".win-item");
          if (!it) return;
          chosen = list[+it.dataset.i];
          close();
        });
      },
      onClose: () => resolve(chosen),
    });
  });
}

export function render(root) {
  root.innerHTML = `
  <div class="page-head"><div><h1>${esc(t("profiles.title"))}</h1><p>${esc(t("profiles.sub"))}</p></div>
    <div class="side"><button class="btn" id="pImport">${esc(t("profiles.import"))}</button><button class="btn primary" id="pNew">${icon("plus")}${esc(t("profiles.new"))}</button></div></div>
  <div class="profs" id="profs">${rows()}</div>`;

  const redraw = () => {
    root.querySelector("#profs").innerHTML = rows();
    notify("profile-external");
  };
  const s = state.settings;

  root.querySelector("#profs").addEventListener("click", async (e) => {
    const row = e.target.closest(".prof");
    if (!row) return;
    const id = row.dataset.id;
    const p = s.profiles[id];
    const act = e.target.closest("[data-act]")?.dataset.act;
    if (!act) {
      s.activeProfile = id;
      saveNow();
      return redraw();
    }
    if (act === "rename") {
      const name = await promptDialog(t("profiles.namePrompt"), p.name);
      if (name && name.trim()) p.name = name.trim();
    } else if (act === "dup") {
      const nid = newId();
      s.profiles[nid] = { ...JSON.parse(JSON.stringify(p)), name: t("profile.copy", { name: p.name }), match: "" };
    } else if (act === "del") {
      if (!(await confirmDialog(t("profiles.deleteConfirm", { name: p.name }), { danger: true, okLabel: t("profiles.delete") }))) return;
      delete s.profiles[id];
      if (s.activeProfile === id) s.activeProfile = Object.keys(s.profiles)[0];
    } else if (act === "link") {
      if (p.match) p.match = "";
      else {
        const w = await pickWindow();
        if (w) {
          p.match = w.exe;
          if (p.name === t("profile.default") || p.name === "Default" || /^Profile|^Profil/.test(p.name)) p.name = w.title.slice(0, 40);
        }
      }
    } else if (act === "export") {
      try {
        await navigator.clipboard.writeText(JSON.stringify({ moonupProfile: 1, ...p }, null, 2));
        toast(t("profiles.copied"), "ok");
      } catch {
        await promptDialog(t("profiles.export"), JSON.stringify({ moonupProfile: 1, ...p }, null, 2), { multiline: true });
      }
      return;
    }
    saveNow();
    redraw();
  });

  root.querySelector("#pNew").addEventListener("click", async () => {
    const name = await promptDialog(t("profiles.namePrompt"), "");
    if (!name || !name.trim()) return;
    const nid = newId();
    s.profiles[nid] = { ...defaultProfile(name.trim()) };
    saveNow();
    redraw();
  });

  root.querySelector("#pImport").addEventListener("click", async () => {
    const text = await promptDialog(t("profiles.importPrompt"), "", { multiline: true });
    if (!text) return;
    try {
      const p = JSON.parse(text);
      if (!p.moonupProfile) throw new Error();
      delete p.moonupProfile;
      s.profiles[newId()] = { ...defaultProfile(p.name || "Imported"), ...p };
      saveNow();
      redraw();
    } catch {
      toast(t("profiles.invalid"), "bad");
    }
  });
}
