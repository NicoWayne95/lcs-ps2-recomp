import "@fontsource/inter/400.css";
import "@fontsource/inter/600.css";
import "@fontsource/inter/700.css";
import "@fontsource/bebas-neue/400.css";
import "./styles.css";
import { invoke as tauriInvoke } from "@tauri-apps/api/core";
import { listen as tauriListen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { t, setLang, lang, hasKey, type Lang, type Key } from "./i18n";
import { previewInvoke } from "./preview";

// In a plain browser (the design preview: `npm run dev`) there is no Tauri backend: sample data instead.
const inTauri = "__TAURI_INTERNALS__" in window;
const invoke = <T = unknown>(cmd: string, args?: Record<string, unknown>): Promise<T> =>
  inTauri ? tauriInvoke<T>(cmd, args) : (previewInvoke(cmd, args) as Promise<T>);
const listen = <T>(event: string, cb: (e: { payload: T }) => void) =>
  inTauri ? tauriListen<T>(event, cb) : Promise.resolve(() => {});

// ---- backend types (src-tauri/src) ----
interface ConfigView { language: Lang; windowed: boolean; game_dir: string | null; launcher_version: string; }
interface Status { game_dir: string | null; runtime: boolean; iso: boolean; complete: boolean; running: boolean; installed_version: string | null; kit_version: string | null; backup_version: string | null; missing: string[]; }
interface Setting { key: string; value: number | null; choices: number; }
interface IsoReport { path: string; size: number; size_ok: boolean; iso9660: boolean; boot_file: string | null; exe_found: boolean; sha256: string | null; hash_ok: boolean; supported: boolean; error: string | null; }
interface UpdateInfo { state: string; latest_tag: string | null; latest_name: string | null; notes: string | null; published_at: string | null; installed: string | null; detail: string | null; release_version?: string | null; download_mb?: number | null; min_launcher?: string | null; launcher_setup?: boolean; }
interface UpdProgress { phase: string; stage: string | null; percent: number; detail: string; }
interface PrepProgress { stage: string; stage_index: number; stage_count: number; percent: number; detail: string; }
interface PrepInfo { kit: string | null; target: string | null; running: boolean; downloads_mb: number | null; }
interface Exited { code: number | null; seconds: number; log_tail: string[]; }
interface Activity { t: number; kind: string; detail: string; }
interface SaveEntry { name: string; modified: number; bytes: number; }
interface Backup { name: string; created: number; entries: number; }
interface SaveFolder { path: string; custom: boolean; reachable: boolean; }
interface Library { play_seconds: number; sessions: number; last_played: number | null; activity: Activity[]; saves: SaveEntry[]; backups: Backup[]; built_at: number | null; save_folder: SaveFolder; }
type Done = { ok: boolean; error: string | null };

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const STAGE_KEYS = ["check_iso", "place_iso", "tools", "elf", "recompiler", "recompile", "postprocess", "configure", "build", "install"];
const UPDATE_PHASES = ["download", "unpack", "swap"];

let config: ConfigView;
let status: Status;
let library: Library | null = null;
let settings: Setting[] = [];
let update: UpdateInfo | null = null;
let checking = false;
let isoPath = "";
let isoReport: IsoReport | null = null;
let isoBusy = false;
let isoPct = 0;
let exitInfo: Exited | null = null;
let startError: string | null = null;
let prepInfo: PrepInfo | null = null;
let updResult: string | null = null;
let prepResult: Done | null = null;

// the task: a preparation or an update, shown in the content area (hideable)
type TaskKind = "prepare" | "update";
let task: { kind: TaskKind; running: boolean; percent: number; index: number; detail: string; result: Done | null; version: string } | null = null;
let taskHidden = false;
const preparing = () => task?.running === true && task.kind === "prepare";
const updating = () => task?.running === true && task.kind === "update";
const busy = () => task?.running === true;

// ---- small helpers ----
function el<K extends keyof HTMLElementTagNameMap>(tag: K, cls?: string, text?: string): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}
function icon(name: string, cls = "") {
  const i = el("img", cls);
  i.src = `/media/icons/png/${name}.png`;
  i.alt = "";
  return i;
}
function toast(text: string, kind: "" | "ok" | "err" = "") {
  const tst = el("div", "toast " + kind, text);
  $("toasts").append(tst);
  setTimeout(() => tst.classList.add("out"), 3800);
  setTimeout(() => tst.remove(), 4300);
}
const locale = () => (lang() === "de" ? "de-DE" : "en-GB");
function ago(unix: number): string {
  const s = Math.round(unix - Date.now() / 1000);
  const rtf = new Intl.RelativeTimeFormat(locale(), { numeric: "auto" });
  const a = Math.abs(s);
  if (a < 60) return rtf.format(Math.round(s), "second");
  if (a < 3600) return rtf.format(Math.round(s / 60), "minute");
  if (a < 86400) return rtf.format(Math.round(s / 3600), "hour");
  if (a < 86400 * 30) return rtf.format(Math.round(s / 86400), "day");
  return new Date(unix * 1000).toLocaleDateString(locale(), { day: "numeric", month: "short", year: "numeric" });
}
function when(unix: number): string {
  return new Date(unix * 1000).toLocaleString(locale(), { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit" });
}
function duration(sec: number): string {
  const h = Math.floor(sec / 3600);
  const m = Math.floor(sec / 60) % 60;
  if (h > 0) return `${h} h ${m} min`;
  if (m > 0) return `${m} min`;
  return `${sec} s`;
}
function rolling(target: HTMLElement) {
  let last = "";
  return (value: string) => {
    if (value === last) return;
    target.replaceChildren(...[...value].map((ch, i) => el("span", "digit" + (last[i] !== ch ? " roll" : ""), ch)));
    last = value;
  };
}

// ---- texts ----
function applyTexts() {
  document.documentElement.lang = lang();
  document.querySelectorAll<HTMLElement>("[data-i18n]").forEach((e) => {
    const k = e.dataset.i18n!;
    if (hasKey(k)) e.textContent = t(k);
  });
  document.querySelectorAll<HTMLElement>("[data-i18n-title]").forEach((e) => {
    const k = e.dataset.i18nTitle!;
    if (hasKey(k)) e.title = t(k);
  });
  document.querySelectorAll<HTMLButtonElement>(".lang button").forEach((b) => b.classList.toggle("on", b.dataset.lang === lang()));
  renderAll();
}
async function switchLang(l: Lang) {
  setLang(l);
  await invoke("set_language", { language: lang() }).catch(() => {});
  applyTexts();
}

// ---- title bar and navigation ----
if (inTauri) {
  const win = getCurrentWindow();
  $("tb-min").onclick = () => win.minimize();
  $("tb-max").onclick = () => win.toggleMaximize();
  $("tb-close").onclick = () => win.close();
}
document.querySelectorAll<HTMLButtonElement>(".lang button").forEach((b) => b.addEventListener("click", () => switchLang(b.dataset.lang as Lang)));
function showPage(name: string) {
  taskHidden = true; // going somewhere else hides a running task (Home shows its progress)
  document.querySelectorAll<HTMLButtonElement>(".nav[data-page]").forEach((n) => n.classList.toggle("active", n.dataset.page === name));
  document.querySelectorAll<HTMLElement>(".page").forEach((p) => p.classList.toggle("active", p.id === "page-" + name));
  if (name === "settings") loadSettings();
  if (name === "install") refreshStatus();
  if (name === "saves" || name === "home") loadLibrary();
  renderTask();
}
document.querySelectorAll<HTMLButtonElement>(".nav[data-page]").forEach((n) => n.addEventListener("click", () => showPage(n.dataset.page!)));
document.addEventListener("click", (e) => {
  const target = e.target as HTMLElement;
  const open = target.closest<HTMLElement>("[data-open]");
  if (open) invoke("open_folder", { kind: open.dataset.open }).catch((er) => toast(String(er), "err"));
  const go = target.closest<HTMLElement>("[data-goto]");
  if (go) showPage(go.dataset.goto!);
});

// ---- home ----
function renderHome() {
  if (!status) return;
  const play = $<HTMLButtonElement>("play");
  play.disabled = !status.complete || status.running || busy();
  play.classList.toggle("playing", status.running);
  if (!status.running) $("play-label").textContent = t("home.play");

  const pill = $("home-pill");
  let title: string, detail: string, kind = "";
  const builtWhen = library?.built_at ? ago(library.built_at) : "";
  if (busy() && task) {
    kind = "run";
    title = updating() ? t("status.updating", { pct: Math.round(task.percent) }) : t("status.preparing", { pct: Math.round(task.percent) });
    detail = taskStep();
  } else if (status.running) {
    kind = "run";
    title = t("status.running");
    detail = t("status.runningDetail");
  } else if (status.complete) {
    kind = update?.state === "available" ? "warn" : exitInfo && exitInfo.code !== 0 ? "err" : "";
    title = update?.state === "available" && update.release_version ? t("version.available", { v: update.release_version }) : t("status.ready");
    detail = status.installed_version ? t("home.readyDetail", { v: status.installed_version, when: builtWhen }) : t("home.localDetail", { when: builtWhen });
  } else if (!status.game_dir) {
    kind = "warn";
    title = t("status.noFolder");
    detail = t("status.noFolderDetail");
  } else {
    kind = "warn";
    title = t("status.required");
    detail = t("status.requiredDetail", { missing: status.missing.join(", ") });
  }
  pill.className = "pill " + kind;
  $("home-status-title").textContent = title;
  $("home-status-detail").textContent = detail;
  renderRailTask();

  // the exit card
  const card = $("exit-card");
  if (startError) {
    card.hidden = false;
    $("exit-title").textContent = t("exit.startFailed");
    $("exit-detail").textContent = startError;
    $("exit-log").textContent = "";
  } else if (exitInfo && exitInfo.code !== 0) {
    card.hidden = false;
    $("exit-title").textContent = t("exit.title", { code: exitInfo.code ?? "?" });
    $("exit-detail").textContent = t("exit.detail", { s: exitInfo.seconds });
    $("exit-log").textContent = exitInfo.log_tail.join("\n");
  } else card.hidden = true;

  // your game
  const kv = $("home-game");
  const rows: [string, string, string?][] = [
    [t("home.playTime"), library ? duration(library.play_seconds) : "–"],
    [t("home.lastPlayed"), library?.last_played ? ago(library.last_played) : t("home.never")],
    [t("home.sessions"), String(library?.sessions ?? 0)],
    [t("home.version"), status.installed_version ?? t("upd.local")],
    [t("home.built"), builtWhen || "–"],
    [t("home.iso"), status.iso ? "SLUS_214.23 · " + t("home.isoOk") : t("home.isoMissing"), status.iso ? "ok" : "bad"],
  ];
  kv.replaceChildren(...rows.flatMap(([k, v, c]) => [el("span", "", k), el("b", c ?? "", v)]));

  // saves (the newest three)
  const sv = $("home-saves");
  const saves = library?.saves ?? [];
  if (!saves.length) sv.replaceChildren(el("p", "empty", t("home.noSaves")));
  else {
    sv.replaceChildren(...saves.slice(0, 3).map((s, i) => saveRow(s, i)));
    if (saves.length > 3) {
      const more = el("button", "more", t("home.moreSaves", { n: saves.length - 3 }));
      more.dataset.goto = "saves";
      sv.append(more);
    }
  }

  renderQuick();
  renderHomeUpdate();

  // activity
  const act = $("home-activity");
  const list = library?.activity ?? [];
  if (!list.length) act.replaceChildren(el("p", "empty", t("home.noActivity")));
  else act.replaceChildren(...list.slice(0, 6).map((a) => { const d = el("div"); d.append(el("time", "", when(a.t)), el("span", "", activityText(a))); return d; }));
}
function saveRow(s: SaveEntry, i: number) {
  const r = el("div", "save");
  const info = el("div", "grow");
  info.append(el("b", "", s.name), el("small", "", `${when(s.modified)} · ${Math.max(1, Math.round(s.bytes / 1024))} KB`));
  r.append(el("div", "n", String(i + 1)), info);
  return r;
}
function activityText(a: Activity): string {
  switch (a.kind) {
    case "played": return t("act.played", { d: duration(Number(a.detail) || 0) });
    case "crashed": return t("act.crashed", { c: a.detail });
    case "updated": return t("act.updated", { v: a.detail });
    case "built": return t("act.built", { v: a.detail });
    case "backup": return t("act.backup");
    case "restored": return t("act.restored");
    case "save_dir": return t("act.save_dir");
    default: return a.kind;
  }
}
// quick settings: the launcher's window mode and three of the game's own (lcs.ini)
function renderQuick() {
  const box = $("home-quick");
  const rows: HTMLElement[] = [];
  const sw = (on: boolean, disabled: boolean, onClick: () => void) => {
    const b = el("button", "switch" + (on ? " on" : ""));
    b.disabled = disabled;
    b.onclick = onClick;
    return b;
  };
  const locked = status?.running ?? false;
  const win = el("div", "q");
  win.append(el("span", "", t("opt.windowed")), sw(config?.windowed ?? false, false, () => setWindowed(!config.windowed)));
  rows.push(win);
  for (const key of ["FRAME SYNC", "FRAME LIMITER", "RAW INPUT"]) {
    const s = settings.find((x) => x.key === key);
    if (!s) continue;
    const q = el("div", "q");
    q.append(el("span", "", t(("set." + key) as Key)));
    if (s.choices === 2) q.append(sw(s.value === 1, locked, () => save(s, s.value === 1 ? 0 : 1)));
    else {
      const chips = el("div", "chips");
      ["30", "60", "120", "∞"].slice(0, s.choices).forEach((label, i) => {
        const b = el("button", s.value === i ? "on" : "", label);
        b.disabled = locked;
        b.onclick = () => save(s, i);
        chips.append(b);
      });
      q.append(chips);
    }
    rows.push(q);
  }
  box.replaceChildren(...rows);
}
function renderHomeUpdate() {
  const st = checking ? "checking" : update?.state ?? null;
  const img = $<HTMLImageElement>("home-upd-icon");
  const notes = $("home-upd-notes");
  const acts = $("home-upd-actions");
  notes.replaceChildren();
  acts.replaceChildren();
  let title = t("version.notChecked"), detail = "";
  img.src = "/media/icons/png/cloud_cyan_64.png";
  if (st === "checking") { title = t("upd.checking"); img.src = "/media/status/downloading_64.png"; }
  else if (st === "available") {
    img.src = "/media/status/update_available_64.png";
    title = t("version.available", { v: update!.release_version ?? "" });
    detail = t("upd.availableDetail", { v: update!.release_version ?? "", mb: Math.max(1, Math.round(update!.download_mb ?? 0)) });
    (update!.notes ?? "").split(/\r?\n/).map((l) => l.replace(/^[-*•]\s*/, "").trim()).filter(Boolean).slice(0, 3).forEach((l) => notes.append(el("li", "", l)));
    const go = el("button", "btn primary", t("updates.install", { v: update!.release_version ?? "", mb: Math.max(1, Math.round(update!.download_mb ?? 0)) }));
    go.disabled = busy() || status?.running || !status?.iso;
    go.onclick = startUpdate;
    acts.append(go);
  } else if (st === "up_to_date") { img.src = "/media/status/ready_64.png"; title = t("upd.up_to_date"); detail = status?.installed_version ? t("version.title", { v: status.installed_version }) : ""; }
  else if (st === "no_release") { title = t("upd.no_release"); }
  else if (st === "offline") { img.src = "/media/status/warning_64.png"; title = t("upd.offline"); }
  else if (st && hasKey("upd." + st)) { title = t(("upd." + st) as Key); img.src = st === "bad_signature" || st === "error" ? "/media/status/error_64.png" : "/media/status/warning_64.png"; }
  if (status?.backup_version !== null && status?.backup_version !== undefined && st !== "available") {
    notes.append(el("li", "", t("updates.restoreText", { v: status.backup_version || t("updates.previous") })));
  }
  $("home-version-title").textContent = title;
  $("home-version-detail").textContent = detail;
  $<HTMLButtonElement>("home-check").disabled = checking;
}
$("play").addEventListener("click", async () => {
  startError = null;
  exitInfo = null;
  try {
    await invoke("launch_game");
  } catch (e) {
    const err = String(e);
    startError = err.startsWith("save_dir_missing: ") ? t("saves.folderMissing", { p: err.slice("save_dir_missing: ".length) }) : err;
  }
  await refreshStatus();
});
$("exit-dismiss").addEventListener("click", () => { startError = null; exitInfo = null; renderHome(); });
$("home-backup").addEventListener("click", () => backupSaves());
// the build / update in the rail (the user's ask 2026-11-18): a ring with the percentage while it runs, green or red
// when it ended until its result is closed; a click opens the progress
function renderRailTask() {
  const b = $<HTMLButtonElement>("rail-task");
  b.hidden = !task;
  if (!task) return;
  const pct = Math.floor(task.percent);
  const failed = !!task.result && !task.result.ok;
  b.classList.toggle("done", !!task.result?.ok);
  b.classList.toggle("fail", failed);
  $("rail-task-arc").style.strokeDashoffset = String(119.4 * (1 - (failed ? 1 : task.percent / 100)));
  $("rail-task-pct").textContent = task.result ? (task.result.ok ? "✓" : "!") : pct + "%";
  const title = task.kind === "update" ? t("task.updateTitle", { v: task.version }) : t("task.prepareTitle");
  $("rail-task-tip").textContent = task.running ? `${title} · ${pct}% · ${taskStep()}` : title;
}
$("rail-task").addEventListener("click", () => { taskHidden = false; renderTask(); });

// ---- installation ----
function li(ok: boolean | null, text: string) {
  return el("li", ok === null ? "wait" : ok ? "ok" : "bad", text);
}
function renderInstall() {
  $<HTMLInputElement>("iso-path").value = isoPath || "";
  $<HTMLInputElement>("iso-path").placeholder = t("install.noIso");
  $<HTMLButtonElement>("iso-check").disabled = !isoPath || isoBusy;
  $<HTMLButtonElement>("iso-browse").disabled = isoBusy;
  const list = $("iso-checks");
  list.replaceChildren();
  $("iso-progress").hidden = !isoBusy;
  $("iso-bar").style.width = isoPct + "%";
  $("iso-pct").textContent = isoPct + "%";
  if (isoBusy && !isoReport) list.append(li(null, t("check.hashing")));
  const r = isoReport;
  if (r) {
    list.append(li(r.iso9660, t("check.format")));
    if (r.iso9660) list.append(li(r.boot_file === "SLUS_214.23", t("check.boot", { file: r.boot_file ?? "?" })));
    if (r.boot_file) list.append(li(r.exe_found, t("check.exe")));
    if (r.boot_file === "SLUS_214.23") list.append(li(r.size_ok, t("check.size")));
    if (r.sha256) list.append(li(r.hash_ok, t("check.hash")));
    if (r.supported) list.append(li(true, t("check.supported")));
    if (r.error) {
      const code = r.error.split(":")[0];
      const key = ("iso." + code) as Key;
      list.append(li(false, hasKey(key) ? t(key) : t("iso.failed", { e: r.error })));
    }
  }
  if (status) {
    $<HTMLInputElement>("game-dir").value = status.game_dir ?? "";
    $("folder-checks").replaceChildren(li(status.runtime, t("folder.runtime")), li(status.iso, t("folder.iso")));
  }
  const lead = document.querySelector<HTMLElement>('[data-i18n="install.prepareLead"]');
  if (lead) lead.textContent = t("install.prepareLead", { mb: prepInfo?.downloads_mb ?? 260 });
  const start = $<HTMLButtonElement>("prep-start");
  start.disabled = busy() || !isoReport?.supported || !prepInfo?.kit || (status?.running ?? false);
  start.title = isoReport?.supported ? "" : t("install.needIso");
  $("prep-show").hidden = !preparing();
  const res = $("prep-result");
  if (prepResult && !preparing()) {
    res.hidden = false;
    const e = prepResult.error ?? "";
    res.textContent = prepResult.ok ? t("install.prepDone") : e === "cancelled" ? t("install.prepCancelled") : t("install.prepFailed", { e });
  } else res.hidden = true;
}
$("iso-browse").addEventListener("click", async () => {
  const p = await invoke<string | null>("pick_iso");
  if (p) {
    isoPath = p;
    isoReport = null;
    renderInstall();
  }
});
$("iso-check").addEventListener("click", async () => {
  isoBusy = true;
  isoReport = null;
  isoPct = 0;
  renderInstall();
  isoReport = await invoke<IsoReport>("validate_iso", { path: isoPath });
  isoBusy = false;
  renderInstall();
});
$("pick-game-dir").addEventListener("click", async () => {
  try {
    await invoke("pick_game_dir");
  } catch (e) {
    toast(String(e), "err");
  }
  config = await invoke<ConfigView>("get_config");
  await refreshStatus();
});
async function startPrepare(path: string) {
  prepResult = null;
  try {
    await invoke("start_prepare", { isoPath: path });
    task = { kind: "prepare", running: true, percent: 0, index: 0, detail: "", result: null, version: status?.kit_version ?? "" };
    taskHidden = false;
  } catch (e) {
    prepResult = { ok: false, error: String(e) };
    toast(t("install.prepFailed", { e: String(e) }), "err");
  }
  renderAll();
}
$("prep-start").addEventListener("click", () => startPrepare(isoPath));
$("prep-show").addEventListener("click", () => { taskHidden = false; renderTask(); });

// ---- the task view ----
function stageKeys(kind: TaskKind) {
  return kind === "update" ? [...UPDATE_PHASES.map((p) => "phase." + p), ...STAGE_KEYS.map((s) => "stage." + s)] : STAGE_KEYS.map((s) => "stage." + s);
}
function taskStep(): string {
  if (!task) return "";
  const keys = stageKeys(task.kind);
  const k = keys[Math.min(task.index, keys.length - 1)] as Key;
  return t(k) + (task.detail ? " · " + task.detail : "");
}
const rollPct = rolling($("task-pct"));
function renderTask() {
  renderRailTask();
  const view = $("task");
  view.hidden = !task || taskHidden;
  if (!task) return;
  $("task-title").textContent = task.kind === "update" ? t("task.updateTitle", { v: task.version }) : t("task.prepareTitle");
  $("task-sub").textContent = task.kind === "update" ? t("task.updateSub") : t("task.prepareSub");
  rollPct(String(Math.floor(task.percent)));
  $("task-arc").style.strokeDashoffset = String(553 * (1 - task.percent / 100));
  $("task-detail").textContent = task.running ? task.detail : "";
  $<HTMLButtonElement>("task-cancel").hidden = !task.running;
  const keys = stageKeys(task.kind);
  const failed = task.result && !task.result.ok;
  $("task-stages").replaceChildren(...keys.map((k, i) => {
    const done = task!.result?.ok || i < task!.index;
    const cls = failed && i === task!.index ? "fail" : done ? "done" : i === task!.index && task!.running ? "now" : "";
    const item = el("li", cls);
    item.append(el("i", "", cls === "done" ? "✓" : cls === "fail" ? "✕" : ""), el("span", "", t(k as Key)));
    return item;
  }));
  const res = $("task-result");
  res.hidden = !task.result;
  if (task.result) {
    const e = task.result.error ?? "";
    const cancelled = e === "cancelled";
    res.className = "task-result" + (task.result.ok || cancelled ? "" : " err");
    $<HTMLImageElement>("task-result-icon").src = task.result.ok ? "/media/status/ready_64.png" : cancelled ? "/media/status/warning_64.png" : "/media/status/error_64.png";
    $("task-result-title").textContent = task.result.ok ? t("task.okTitle") : cancelled ? t("task.cancelledTitle") : t("task.failTitle");
    $("task-result-text").textContent = task.kind === "update"
      ? task.result.ok ? t("updates.done", { v: task.version }) : cancelled ? t("updates.cancelled") : t("updates.failed", { e })
      : task.result.ok ? t("install.prepDone") : cancelled ? t("install.prepCancelled") : t("install.prepFailed", { e });
  }
}
$("task-hide").addEventListener("click", () => { taskHidden = true; renderTask(); });
$("task-cancel").addEventListener("click", () => invoke("cancel_prepare"));
$("task-close").addEventListener("click", () => { if (task && !task.running) task = null; renderAll(); });
listen<PrepProgress>("prepare-progress", (e) => {
  if (!task || task.kind !== "prepare") task = { kind: "prepare", running: true, percent: 0, index: 0, detail: "", result: null, version: "" };
  task.running = true;
  task.percent = e.payload.percent;
  task.index = e.payload.stage_index;
  task.detail = e.payload.detail;
  renderTask();
  renderHome();
});
listen<Done>("prepare-done", async (e) => {
  if (task) { task.running = false; task.result = e.payload; if (e.payload.ok) task.percent = 100; }
  prepResult = e.payload;
  config = await invoke<ConfigView>("get_config");
  await refreshStatus();
  loadLibrary();
});
listen<UpdProgress>("update-progress", (e) => {
  if (!task || task.kind !== "update") task = { kind: "update", running: true, percent: 0, index: 0, detail: "", result: null, version: update?.release_version ?? "" };
  const p = e.payload;
  task.running = true;
  task.percent = p.percent;
  task.index = p.phase === "build" && p.stage ? UPDATE_PHASES.length + STAGE_KEYS.indexOf(p.stage) : Math.max(0, UPDATE_PHASES.indexOf(p.phase));
  task.detail = p.detail;
  renderTask();
  renderHome();
});
listen<Done>("update-done", async (e) => {
  if (task) { task.running = false; task.result = e.payload; if (e.payload.ok) task.percent = 100; }
  await refreshStatus();
  loadLibrary();
  if (e.payload.ok) checkUpdates();
});

// ---- saves ----
async function loadLibrary() {
  library = await invoke<Library>("get_library").catch(() => null);
  renderHome();
  renderSaves();
}
async function backupSaves() {
  try {
    const name = await invoke<string>("backup_saves");
    toast(t("saves.made", { n: name }), "ok");
  } catch (e) {
    toast(t("saves.failed", { e: String(e) }), "err");
  }
  loadLibrary();
}
function renderSaves() {
  const saves = library?.saves ?? [];
  const list = $("saves-list");
  list.replaceChildren(...(saves.length ? saves.map((s, i) => saveRow(s, i)) : [el("p", "empty", t("home.noSaves"))]));
  const backups = library?.backups ?? [];
  const bl = $("backups-list");
  bl.replaceChildren(...(backups.length ? backups.map((b) => {
    const row = el("div", "list-row");
    const info = el("div", "grow");
    info.append(el("b", "", when(b.created)), el("small", "", `${b.name} · ${t("saves.entries", { n: b.entries })}`));
    const btn = el("button", "btn", t("saves.restore"));
    btn.disabled = status?.running ?? false;
    let armed = false;
    btn.onclick = async () => {
      if (!armed) {
        armed = true;
        btn.textContent = t("saves.confirm");
        btn.classList.add("danger");
        setTimeout(() => { armed = false; btn.textContent = t("saves.restore"); btn.classList.remove("danger"); }, 4000);
        return;
      }
      try {
        await invoke("restore_saves", { name: b.name });
        toast(t("saves.restoredMsg"), "ok");
      } catch (e) {
        toast(t("saves.failed", { e: String(e) }), "err");
      }
      loadLibrary();
    };
    row.append(info, btn);
    return row;
  }) : [el("p", "empty", t("saves.noBackups"))]));
  $<HTMLButtonElement>("saves-backup").disabled = status?.running ?? false;
  const folder = library?.save_folder;
  $<HTMLInputElement>("save-dir").value = folder?.path ?? "";
  $<HTMLButtonElement>("save-dir-pick").disabled = status?.running ?? false;
  $<HTMLButtonElement>("save-dir-reset").disabled = (status?.running ?? false) || !folder?.custom;
  const missing = $("save-dir-missing");
  missing.hidden = !folder || folder.reachable;
  missing.textContent = folder ? t("saves.folderMissing", { p: folder.path }) : "";
}
async function setSaveDir(path: string | null) {
  try {
    const p = await invoke<string | null>("set_save_dir", { path, copy: $<HTMLInputElement>("save-dir-copy").checked });
    if (p) toast(t("saves.folderSet", { p }), "ok");
  } catch (e) {
    toast(t("saves.failed", { e: String(e) }), "err");
  }
  loadLibrary();
}
$("save-dir-pick").addEventListener("click", () => setSaveDir("pick"));
$("save-dir-reset").addEventListener("click", () => setSaveDir(null));
$("saves-backup").addEventListener("click", () => backupSaves());

// ---- settings (the game's lcs.ini, values as the pause menu's indexes) ----
const onOff = () => [t("opt.off"), t("opt.on")];
const choicesFor: Record<string, () => string[]> = {
  "SCREEN RESOLUTION": () => [t("opt.res0"), "1920 × 1080", "2560 × 1440", "3840 × 2160", t("opt.res4")],
  "FRAME LIMITER": () => ["30 FPS", "60 FPS", "120 FPS", t("opt.uncapped")],
  MSAA: () => [t("opt.off"), "2×", "4×", "8×"],
  "TEXTURE FILTERING": () => [t("opt.off"), "2×", "4×", "8×", "16×"],
};
const groups: Record<string, string[]> = {
  "set-display": ["SCREEN RESOLUTION", "WIDE SCREEN", "FRAME SYNC", "FRAME LIMITER"],
  "set-graphics": ["MSAA", "TEXTURE FILTERING", "TRAILS"],
  "set-input": ["MOUSE SENSITIVITY", "INVERT MOUSE", "RAW INPUT"],
};
function rowFor(s: Setting): HTMLElement {
  const row = el("div", "row");
  const k = ("set." + s.key) as Key;
  const label = el("label", "", hasKey(k) ? t(k) : s.key);
  row.append(label);
  const locked = status?.running ?? false;
  if (s.choices === 2) {
    const sw = el("button", "switch" + (s.value === 1 ? " on" : ""));
    sw.disabled = locked;
    sw.setAttribute("aria-label", label.textContent!);
    sw.onclick = () => save(s, s.value === 1 ? 0 : 1);
    row.append(sw);
  } else if (s.key === "MOUSE SENSITIVITY") {
    const r = el("input");
    r.type = "range";
    r.min = "0";
    r.max = String(s.choices - 1);
    r.step = "1";
    r.value = String(s.value ?? 3);
    r.disabled = locked;
    r.onchange = () => save(s, Number(r.value));
    row.append(r);
  } else {
    const sel = el("select");
    sel.disabled = locked;
    const names = (choicesFor[s.key] ?? (() => onOff()))();
    if (s.value === null) sel.append(new Option(t("settings.default"), "", true, true));
    names.forEach((n, i) => sel.append(new Option(n, String(i), false, s.value === i)));
    sel.onchange = () => sel.value !== "" && save(s, Number(sel.value));
    row.append(sel);
  }
  return row;
}
async function save(s: Setting, value: number) {
  try {
    await invoke("set_setting", { key: s.key, value });
    s.value = value;
  } catch (e) {
    toast(String(e), "err");
  }
  renderSettings();
  renderQuick();
}
async function setWindowed(on: boolean) {
  config.windowed = on;
  await invoke("set_windowed", { windowed: on }).catch(() => {});
  renderSettings();
  renderQuick();
}
function renderSettings() {
  $("settings-running").hidden = !status?.running;
  for (const [id, keys] of Object.entries(groups)) {
    $(id).replaceChildren(...keys.map((k) => settings.find((s) => s.key === k)).filter((s): s is Setting => !!s).map(rowFor));
  }
  if (!config) return;
  const wrow = el("div", "row");
  const wsel = el("select");
  wsel.append(new Option(t("opt.fullscreen"), "0", false, !config.windowed), new Option(t("opt.windowed"), "1", false, config.windowed));
  wsel.onchange = () => setWindowed(wsel.value === "1");
  wrow.append(el("label", "", t("set.windowed")), wsel);
  const lrow = el("div", "row");
  const lsel = el("select");
  lsel.append(new Option("English", "en", false, lang() === "en"), new Option("Deutsch", "de", false, lang() === "de"));
  lsel.onchange = () => switchLang(lsel.value as Lang);
  lrow.append(el("label", "", t("set.language")), lsel);
  $("set-launcher").replaceChildren(wrow, lrow);
}
async function loadSettings() {
  settings = await invoke<Setting[]>("get_settings").catch(() => []);
  renderSettings();
  renderQuick();
}

// ---- updates ----
function renderUpdates() {
  const st = checking ? "checking" : update?.state ?? null;
  const img = $<HTMLImageElement>("upd-icon");
  const map: Record<string, [string, Key]> = {
    checking: ["/media/status/downloading_64.png", "upd.checking"],
    up_to_date: ["/media/status/ready_64.png", "upd.up_to_date"],
    available: ["/media/status/update_available_64.png", "upd.available"],
    no_release: ["/media/icons/png/cloud_cyan_64.png", "upd.no_release"],
    unknown_installed: ["/media/status/update_available_64.png", "upd.unknown_installed"],
    offline: ["/media/status/warning_64.png", "upd.offline"],
    launcher_required: ["/media/status/update_available_64.png", "upd.launcher_required"],
    no_manifest: ["/media/status/warning_64.png", "upd.no_manifest"],
    bad_signature: ["/media/status/error_64.png", "upd.bad_signature"],
    error: ["/media/status/error_64.png", "upd.error"],
  };
  if (st && map[st]) {
    img.src = map[st][0];
    $("upd-state").textContent = t(map[st][1]);
  } else {
    img.src = "/media/icons/png/cloud_cyan_64.png";
    $("upd-state").textContent = t("version.notChecked");
  }
  const v = update?.release_version ?? update?.latest_tag ?? "";
  const mb = Math.max(1, Math.round(update?.download_mb ?? 0));
  const details: Record<string, string> = {
    unknown_installed: t("upd.unknown_installedDetail"),
    available: t("upd.availableDetail", { v, mb }),
    launcher_required: update?.launcher_setup
      ? t("upd.launcher_requiredDetail", { v, min: update?.min_launcher ?? "?" })
      : t("upd.launcher_requiredNoSetup", { v, min: update?.min_launcher ?? "?" }),
    no_manifest: t("upd.no_manifestDetail"),
    bad_signature: t("upd.bad_signatureDetail"),
  };
  $("upd-detail").textContent = (st && details[st]) || (update?.detail ?? "");
  const blocked = busy() || (status?.running ?? false);
  const canInstall = (st === "available" || st === "unknown_installed") && !!update?.release_version && !!status?.iso;
  const install = $<HTMLButtonElement>("upd-install");
  install.hidden = !canInstall;
  install.disabled = blocked;
  $("upd-install-label").textContent = t("updates.install", { v, mb });
  const launcher = $<HTMLButtonElement>("upd-launcher");
  launcher.hidden = !(st === "launcher_required" && update?.launcher_setup);
  launcher.disabled = blocked;
  $("upd-result").hidden = !updResult;
  $("upd-result").textContent = updResult ?? "";
  const kitV = status?.kit_version ?? null;
  const gameV = status?.installed_version ?? null;
  const rebuild = !!kitV && !!status?.iso && kitV !== gameV;
  const backupV = status?.backup_version;
  const restore = backupV !== null && backupV !== undefined;
  $("upd-local-panel").hidden = !rebuild && !restore;
  const lines: string[] = [];
  if (rebuild) lines.push(t("updates.rebuildText", { kit: kitV!, game: gameV ?? t("upd.local") }));
  if (restore) lines.push(t("updates.restoreText", { v: backupV || t("updates.previous") }));
  $("upd-local-text").textContent = lines.join(" ");
  const rb = $<HTMLButtonElement>("upd-rebuild");
  rb.hidden = !rebuild;
  rb.disabled = blocked;
  $("upd-rebuild-label").textContent = t("updates.rebuild", { v: kitV ?? "" });
  const rs = $<HTMLButtonElement>("upd-restore");
  rs.hidden = !restore;
  rs.disabled = blocked;
  $("upd-restore-label").textContent = t("updates.restore", { v: backupV || t("updates.previous") });
  $("upd-installed").textContent = status?.installed_version ?? t("upd.local");
  $("upd-latest").textContent = update?.latest_tag ? `${update.latest_tag}${update.published_at ? " · " + update.published_at.slice(0, 10) : ""}` : t("upd.none");
  $<HTMLButtonElement>("upd-check").disabled = checking;
  $("nav-update-badge").hidden = update?.state !== "available" && update?.state !== "launcher_required";
  const notes = update?.notes?.trim();
  $("upd-notes-panel").hidden = !notes;
  $("upd-notes").textContent = notes ?? "";
}
async function checkUpdates() {
  checking = true;
  renderUpdates();
  renderHomeUpdate();
  try {
    update = await invoke<UpdateInfo>("check_updates");
  } catch (e) {
    update = { state: "error", detail: String(e), latest_tag: null, latest_name: null, notes: null, published_at: null, installed: null };
  }
  checking = false;
  renderAll();
}
async function startUpdate() {
  updResult = null;
  try {
    await invoke("install_update");
    task = { kind: "update", running: true, percent: 0, index: 0, detail: "", result: null, version: update?.release_version ?? "" };
    taskHidden = false;
  } catch (e) {
    updResult = t("updates.failed", { e: String(e) });
    toast(updResult, "err");
  }
  renderAll();
}
$("upd-install").addEventListener("click", startUpdate);
$("upd-launcher").addEventListener("click", async () => {
  updResult = null;
  try {
    await invoke("install_launcher");
  } catch (e) {
    updResult = t("updates.failed", { e: String(e) });
    renderUpdates();
  }
});
$("upd-rebuild").addEventListener("click", () => {
  if (status?.game_dir) startPrepare(status.game_dir + "\\gamedata\\LCS.iso");
});
$("upd-restore").addEventListener("click", async () => {
  try {
    await invoke("restore_previous");
    updResult = t("updates.restored");
  } catch (e) {
    updResult = t("updates.restoreFailed", { e: String(e) });
  }
  await refreshStatus();
  checkUpdates();
});
$("upd-check").addEventListener("click", checkUpdates);
$("home-check").addEventListener("click", checkUpdates);
$("upd-releases").addEventListener("click", () => invoke("open_link", { kind: "releases" }));
$("about-repo").addEventListener("click", () => invoke("open_link", { kind: "repo" }));

// ---- the command palette (Ctrl+K) ----
interface Cmd { icon: string; label: string; hint?: string; run: () => void; }
function commands(): Cmd[] {
  const pages: [string, string, Key][] = [["home", "home_white_64", "nav.home"], ["install", "installation_white_64", "nav.install"], ["saves", "save_white_64", "nav.saves"],
    ["settings", "settings_white_64", "nav.settings"], ["updates", "update_white_64", "nav.updates"], ["about", "info_white_64", "nav.about"]];
  const list: Cmd[] = [];
  if (status?.complete && !status.running && !busy()) list.push({ icon: "play_white_64", label: t("home.play"), run: () => $("play").click() });
  list.push({ icon: "archive_white_64", label: t("saves.backup"), run: () => backupSaves() });
  list.push({ icon: "refresh_white_64", label: t("updates.check"), run: () => checkUpdates() });
  list.push({ icon: "folder_white_64", label: t("pal.openGame"), run: () => invoke("open_folder", { kind: "game" }) });
  list.push({ icon: "save_white_64", label: t("pal.openSaves"), run: () => invoke("open_folder", { kind: "saves" }) });
  list.push({ icon: "logs_white_64", label: t("pal.openLogs"), run: () => invoke("open_folder", { kind: "logs" }) });
  list.push({ icon: "monitor_white_64", label: t("pal.toggle", { name: t("opt.windowed") }), hint: config?.windowed ? t("opt.on") : t("opt.off"), run: () => setWindowed(!config.windowed) });
  if (!status?.running) {
    for (const s of settings.filter((x) => x.choices === 2)) {
      const k = ("set." + s.key) as Key;
      list.push({ icon: "settings_white_64", label: t("pal.toggle", { name: hasKey(k) ? t(k) : s.key }), hint: s.value === 1 ? t("opt.on") : t("opt.off"), run: () => save(s, s.value === 1 ? 0 : 1) });
    }
  }
  for (const s of settings.filter((x) => x.choices > 2)) {
    const k = ("set." + s.key) as Key;
    list.push({ icon: "settings_white_64", label: hasKey(k) ? t(k) : s.key, hint: t("nav.settings"), run: () => showPage("settings") });
  }
  list.push({ icon: "external-link_white_64", label: t("pal.language", { l: lang() === "en" ? "Deutsch" : "English" }), run: () => switchLang(lang() === "en" ? "de" : "en") });
  for (const [page, ic, key] of pages) list.push({ icon: ic, label: t("pal.goto", { page: t(key) }), run: () => showPage(page) });
  return list;
}
let palSel = 0;
let palList: Cmd[] = [];
function renderPalette() {
  const q = $<HTMLInputElement>("palette-input").value.trim().toLowerCase();
  palList = commands().filter((c) => c.label.toLowerCase().includes(q));
  palSel = Math.min(palSel, Math.max(0, palList.length - 1));
  const box = $("palette-items");
  if (!palList.length) { box.replaceChildren(el("div", "pal-item empty", t("pal.none"))); return; }
  box.replaceChildren(...palList.map((c, i) => {
    const b = el("button", "pal-item" + (i === palSel ? " sel" : ""));
    b.append(icon(c.icon), el("span", "", c.label));
    if (c.hint) b.append(el("small", "", c.hint));
    b.onclick = () => runPalette(i);
    b.onmousemove = () => { if (palSel !== i) { palSel = i; renderPalette(); } };
    return b;
  }));
  box.querySelector(".sel")?.scrollIntoView({ block: "nearest" });
}
function openPalette(on: boolean) {
  $("palette").hidden = !on;
  $("palette-dim").hidden = !on;
  if (on) {
    const input = $<HTMLInputElement>("palette-input");
    input.placeholder = t("pal.placeholder");
    input.value = "";
    palSel = 0;
    renderPalette();
    input.focus();
  }
}
function runPalette(i: number) {
  const c = palList[i];
  openPalette(false);
  c?.run();
}
$("open-palette").addEventListener("click", () => openPalette(true));
$("palette-dim").addEventListener("click", () => openPalette(false));
$("palette-input").addEventListener("input", () => { palSel = 0; renderPalette(); });
addEventListener("keydown", (e) => {
  const open = !$("palette").hidden;
  if (e.ctrlKey && e.key.toLowerCase() === "k") { e.preventDefault(); openPalette(!open); return; }
  if (!open) { if (e.key === "Escape" && task && !taskHidden) { taskHidden = true; renderTask(); } return; }
  if (e.key === "Escape") openPalette(false);
  else if (e.key === "ArrowDown") { e.preventDefault(); palSel = Math.min(palList.length - 1, palSel + 1); renderPalette(); }
  else if (e.key === "ArrowUp") { e.preventDefault(); palSel = Math.max(0, palSel - 1); renderPalette(); }
  else if (e.key === "Enter") { e.preventDefault(); runPalette(palSel); }
});

// ---- state ----
function renderAll() {
  renderHome();
  renderInstall();
  renderSaves();
  renderUpdates();
  renderTask();
  if ($("page-settings").classList.contains("active")) renderSettings();
  $("tb-version").textContent = config ? "· Beta · Launcher " + config.launcher_version : "";
  $("about-version").textContent = config?.launcher_version ?? "";
}
async function refreshStatus() {
  status = await invoke<Status>("get_status");
  renderAll();
}
listen<number>("game-started", () => refreshStatus());
listen<Exited>("game-exited", (e) => {
  exitInfo = e.payload;
  refreshStatus();
  loadLibrary();
});
listen<{ stage: string; percent: number }>("iso-progress", (e) => {
  isoPct = e.payload.percent;
  $("iso-bar").style.width = isoPct + "%";
  $("iso-pct").textContent = isoPct + "%";
});
// the running game's clock on the Play button (once a second, only while it runs)
let startedAt = 0;
setInterval(() => {
  if (!status?.running) { startedAt = 0; return; }
  if (!startedAt) startedAt = Date.now();
  const s = Math.floor((Date.now() - startedAt) / 1000);
  const hms = [Math.floor(s / 3600), Math.floor(s / 60) % 60, s % 60].map((x) => String(x).padStart(2, "0")).join(":");
  $("play-label").textContent = t("home.playing") + " · " + hms;
}, 1000);

(async () => {
  config = await invoke<ConfigView>("get_config");
  setLang(config.language);
  status = await invoke<Status>("get_status");
  prepInfo = await invoke<PrepInfo>("prepare_info").catch(() => null);
  if (prepInfo?.running) task = { kind: "prepare", running: true, percent: 0, index: 0, detail: "", result: null, version: "" };
  taskHidden = true;
  applyTexts();
  // the first start (nothing built yet): straight to the Installation page, where the game is prepared
  if (!status.complete && !prepInfo?.running) showPage("install");
  loadSettings();
  loadLibrary();
  checkUpdates(); // the release check at start (read only; nothing is downloaded without the player)
})();
