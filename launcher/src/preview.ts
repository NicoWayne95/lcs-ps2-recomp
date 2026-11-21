// Sample answers for the design preview in a plain browser (no Tauri backend). Never used inside the launcher.
const sample: Record<string, unknown> = {
  get_config: { language: "en", windowed: false, game_dir: "C:\\Games\\LCS Recomp\\game", launcher_version: "0.1.0" },
  get_status: {
    game_dir: "C:\\Games\\LCS Recomp\\game", runtime: true, iso: true,
    complete: true, running: false, installed_version: "0.1.0", kit_version: "0.1.0", backup_version: null, missing: [],
  },
  get_settings: [
    { key: "SCREEN RESOLUTION", value: 2, choices: 5 }, { key: "WIDE SCREEN", value: 1, choices: 2 },
    { key: "FRAME SYNC", value: 1, choices: 2 }, { key: "FRAME LIMITER", value: 1, choices: 4 },
    { key: "MSAA", value: 2, choices: 4 }, { key: "TEXTURE FILTERING", value: 4, choices: 5 },
    { key: "TRAILS", value: 1, choices: 2 }, { key: "MOUSE SENSITIVITY", value: 3, choices: 10 },
    { key: "INVERT MOUSE", value: 0, choices: 2 }, { key: "RAW INPUT", value: 1, choices: 2 },
  ],
  check_updates: {
    state: "available", latest_tag: "v0.1.1", latest_name: "LCS Recomp 0.1.1", notes: "- Police cars use the right paint again\n- Free aim follows the mouse more closely\n- Faster rebuilds after an update", published_at: "2026-11-20T12:00:00Z",
    installed: "0.1.0", detail: null, release_version: "0.1.1", download_mb: 41.2, min_launcher: "0.1.0", launcher_setup: false,
  },
  prepare_info: { kit: "C:\\Games\\LCS Recomp\\Source Code", target: "C:\\Games\\LCS Recomp\\game", running: false, downloads_mb: 258 },
  start_prepare: null,
  get_library: (() => {
    const now = Math.floor(Date.now() / 1000);
    return {
      play_seconds: 51720, sessions: 9, last_played: now - 86400 + 3600, built_at: now - 2 * 86400,
      activity: [
        { t: now - 86400 + 3600, kind: "played", detail: "6420" },
        { t: now - 2 * 86400, kind: "updated", detail: "0.1.0" },
        { t: now - 2 * 86400 - 600, kind: "backup", detail: "saves-20261115-180200" },
        { t: now - 3 * 86400, kind: "crashed", detail: "0xC0000005" },
      ],
      saves: [
        { name: "BASLUS-21423LCS0", modified: now - 86400 + 3600, bytes: 58368 },
        { name: "BASLUS-21423LCS1", modified: now - 5 * 86400, bytes: 58368 },
      ],
      backups: [{ name: "savegame-20261115-180200", created: now - 2 * 86400 - 600, entries: 2 }],
      save_folder: { path: "C:\\Games\\LCS Recomp\\game\\Savegame", custom: false, reachable: true },
    };
  })(),
  backup_saves: "saves-20261117-231500",
  pick_iso:"D:\\ISOs\\Grand Theft Auto - Liberty City Stories (USA).iso",
  validate_iso: {
    path: "", size: 4010016768, size_ok: true, iso9660: true, boot_file: "SLUS_214.23", exe_found: true,
    sha256: "9e45271f8835f6b77213c8a7ff3bd1e4307fffa9c3f4434cf8498a5a7b59c4fe", hash_ok: true, supported: true, error: null,
  },
};

export function previewInvoke(cmd: string, _args?: Record<string, unknown>): Promise<unknown> {
  if (cmd in sample) return new Promise((r) => setTimeout(() => r(structuredClone(sample[cmd])), cmd === "validate_iso" ? 600 : 60));
  return Promise.resolve(null);
}
