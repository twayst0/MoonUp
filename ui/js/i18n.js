// Localisation: dictionaries, interpolation and language detection.
import en from "./i18n/en.js";
import tr from "./i18n/tr.js";

export const LANGS = [
  { id: "tr", name: "Türkçe" },
  { id: "en", name: "English" },
  { id: "de", name: "Deutsch" },
  { id: "es", name: "Español" },
  { id: "fr", name: "Français" },
  { id: "it", name: "Italiano" },
  { id: "pt", name: "Português" },
  { id: "ru", name: "Русский" },
  { id: "ja", name: "日本語" },
  { id: "zh", name: "简体中文" },
];

const dicts = { en, tr };
let current = "en";
let dict = en;

export async function loadLanguage(id) {
  if (!LANGS.find((l) => l.id === id)) id = "en";
  if (!dicts[id]) {
    try {
      dicts[id] = (await import(`./i18n/${id}.js`)).default;
    } catch (e) {
      console.warn("missing language", id, e);
      id = "en";
    }
  }
  current = id;
  dict = dicts[id];
  document.documentElement.lang = id;
  return id;
}

export function detectLanguage(locale) {
  const l = (locale || "en").toLowerCase().slice(0, 2);
  return LANGS.find((x) => x.id === l) ? l : "en";
}

export function lang() {
  return current;
}

export function t(key, vars) {
  let s = dict[key] ?? en[key] ?? key;
  if (vars) s = s.replace(/\{(\w+)\}/g, (_, k) => (vars[k] ?? `{${k}}`));
  return s;
}

// Number formatting with the active locale (e.g. "0,35" in Turkish).
export function fmt(n, digits = 0) {
  try {
    return new Intl.NumberFormat(current, { minimumFractionDigits: digits, maximumFractionDigits: digits }).format(n);
  } catch {
    return Number(n).toFixed(digits);
  }
}
