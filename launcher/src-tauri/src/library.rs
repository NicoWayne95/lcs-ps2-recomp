//! What the Home page shows about the player's game, all from the game folder and nothing invented:
//! - play time, last played, an activity list: launcher_stats.json, written by the launcher itself (a game's run as
//!   the launcher saw it start and end, an update, a build, a backup)
//! - the savegames: LCSrecomp's memory card folder - Savegame in the game folder, or one the player chose
//!   (launcher.json save_dir, e.g. on an external drive; LCSrecomp gets it as --save-dir)
//! - savegame backups: copies into backups\savegame-<time>\ (made and restored here; restoring first backs up
//!   what is there, so nothing is lost)

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};
use tauri::State;

use crate::AppState;

const STATS: &str = "launcher_stats.json";
const MAX_ACTIVITY: usize = 40;
const MAX_BACKUPS: usize = 10;

pub fn now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

fn mtime(p: &Path) -> u64 {
    fs::metadata(p).and_then(|m| m.modified()).ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_secs()).unwrap_or(0)
}

#[derive(Clone, Serialize, Deserialize)]
pub struct Activity {
    /// unix seconds
    pub t: u64,
    /// played | crashed | updated | built | backup | restored
    pub kind: String,
    /// played: seconds; updated / built: the version; backup / restored: the backup's name; crashed: the exit code
    pub detail: String,
}

#[derive(Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Stats {
    pub play_seconds: u64,
    pub sessions: u64,
    pub last_played: Option<u64>,
    pub activity: Vec<Activity>,
}

fn load(game: &Path) -> Stats {
    fs::read_to_string(game.join(STATS)).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

fn store(game: &Path, s: &Stats) {
    let tmp = game.join(format!("{}.tmp", STATS));
    if let Ok(text) = serde_json::to_string_pretty(s) {
        if fs::write(&tmp, text).is_ok() {
            let _ = fs::rename(&tmp, game.join(STATS));
        }
    }
}

/// One line of activity (newest first, the list kept short).
pub fn record(game: &Path, kind: &str, detail: &str) {
    let mut s = load(game);
    s.activity.insert(0, Activity { t: now(), kind: kind.into(), detail: detail.into() });
    s.activity.truncate(MAX_ACTIVITY);
    store(game, &s);
}

/// A run of the game ended (game.rs): its time counts, a non-zero exit is noted as such.
pub fn record_run(game: &Path, seconds: u64, code: Option<i32>) {
    let mut s = load(game);
    s.play_seconds += seconds;
    s.sessions += 1;
    s.last_played = Some(now());
    let (kind, detail) = match code {
        Some(c) if c != 0 => ("crashed", format!("{:#X}", c as u32)),
        _ => ("played", seconds.to_string()),
    };
    s.activity.insert(0, Activity { t: now(), kind: kind.into(), detail });
    s.activity.truncate(MAX_ACTIVITY);
    store(game, &s);
}

#[derive(Serialize)]
pub struct SaveEntry {
    name: String,
    modified: u64,
    bytes: u64,
}

#[derive(Serialize)]
pub struct Backup {
    name: String,
    created: u64,
    entries: usize,
}

#[derive(Serialize)]
pub struct Library {
    play_seconds: u64,
    sessions: u64,
    last_played: Option<u64>,
    activity: Vec<Activity>,
    saves: Vec<SaveEntry>,
    backups: Vec<Backup>,
    /// LCSrecomp.exe's time (when this build was installed)
    built_at: Option<u64>,
    save_folder: SaveFolder,
}

fn dir_size(p: &Path) -> u64 {
    if p.is_file() {
        return fs::metadata(p).map(|m| m.len()).unwrap_or(0);
    }
    fs::read_dir(p).map(|r| r.flatten().map(|e| dir_size(&e.path())).sum()).unwrap_or(0)
}

/// The savegame folder's default: Savegame in the game folder (English folder names).
pub const DEFAULT_SAVE_DIR: &str = "Savegame";

/// An older saves\mc0 (before 2026-11-18) moved to Savegame once (LCSrecomp does the same at its start).
pub fn migrate_default(game: &Path) {
    let new = game.join(DEFAULT_SAVE_DIR);
    let old = game.join("saves").join("mc0");
    if !new.exists() && old.is_dir() && fs::rename(&old, &new).is_ok() {
        let _ = fs::remove_dir(game.join("saves")); // only when empty
    }
}

/// The savegame folder in use: the one the player chose (launcher.json save_dir), else Savegame in the game folder.
pub fn save_dir(cfg: &crate::config::Config, game: &Path) -> PathBuf {
    match &cfg.save_dir {
        Some(d) if !d.is_empty() => PathBuf::from(d),
        _ => game.join(DEFAULT_SAVE_DIR),
    }
}

/// Before the game starts: a chosen folder must be reachable (an external drive unplugged: no empty new memory card
/// behind the player's back). None: start with this folder.
pub fn check_save_dir(cfg: &crate::config::Config, game: &Path) -> Result<Option<PathBuf>, String> {
    migrate_default(game);
    match &cfg.save_dir {
        Some(d) if !d.is_empty() => {
            let p = PathBuf::from(d);
            if p.is_dir() {
                Ok(Some(p))
            } else {
                Err(format!("save_dir_missing: {}", p.display()))
            }
        }
        _ => Ok(None),
    }
}

fn backups_dir(game: &Path) -> PathBuf {
    game.join("backups")
}

fn list_saves(dir: &Path) -> Vec<SaveEntry> {
    let mut v: Vec<SaveEntry> = fs::read_dir(dir)
        .map(|r| {
            r.flatten()
                .map(|e| SaveEntry { name: e.file_name().to_string_lossy().to_string(), modified: mtime(&e.path()), bytes: dir_size(&e.path()) })
                .collect()
        })
        .unwrap_or_default();
    v.sort_by(|a, b| b.modified.cmp(&a.modified));
    v
}

/// A backup's savegames: the folder itself (savegame-*), or its mc0 (saves-*, made before 2026-11-18).
fn backup_content(dir: &Path) -> PathBuf {
    if dir.join("mc0").is_dir() {
        dir.join("mc0")
    } else {
        dir.to_path_buf()
    }
}

fn is_backup_name(n: &str) -> bool {
    (n.starts_with("savegame-") || n.starts_with("saves-")) && crate::update::safe_name(n)
}

fn list_backups(game: &Path) -> Vec<Backup> {
    let mut v: Vec<Backup> = fs::read_dir(backups_dir(game))
        .map(|r| {
            r.flatten()
                .filter(|e| is_backup_name(&e.file_name().to_string_lossy()) && e.path().is_dir())
                .map(|e| Backup {
                    name: e.file_name().to_string_lossy().to_string(),
                    created: mtime(&e.path()),
                    entries: fs::read_dir(backup_content(&e.path())).map(|r| r.count()).unwrap_or(0),
                })
                .collect()
        })
        .unwrap_or_default();
    v.sort_by(|a, b| b.created.cmp(&a.created));
    v
}


fn current_save_dir(state: &State<AppState>) -> Result<(PathBuf, PathBuf, bool), String> {
    let cfg = state.config.lock().unwrap();
    let game = cfg.resolve_game_dir().ok_or("no_game_dir")?;
    migrate_default(&game);
    let custom = cfg.save_dir.as_deref().is_some_and(|d| !d.is_empty());
    Ok((save_dir(&cfg, &game), game, custom))
}

#[derive(Serialize)]
pub struct SaveFolder {
    path: String,
    custom: bool,
    reachable: bool,
}

#[tauri::command]
pub fn get_library(state: State<AppState>) -> Result<Library, String> {
    let (saves, game, custom) = current_save_dir(&state)?;
    let s = load(&game);
    let exe = game.join(crate::config::RUNTIME_EXE);
    Ok(Library {
        play_seconds: s.play_seconds,
        sessions: s.sessions,
        last_played: s.last_played,
        activity: s.activity,
        saves: list_saves(&saves),
        backups: list_backups(&game),
        built_at: exe.is_file().then(|| mtime(&exe)),
        save_folder: SaveFolder { path: saves.display().to_string(), custom, reachable: saves.is_dir() || !custom },
    })
}

fn copy_tree(from: &Path, to: &Path) -> Result<usize, String> {
    fs::create_dir_all(to).map_err(|e| e.to_string())?;
    let mut n = 0;
    for e in fs::read_dir(from).map_err(|e| e.to_string())? {
        let e = e.map_err(|e| e.to_string())?;
        let dest = to.join(e.file_name());
        if e.file_type().map_err(|e| e.to_string())?.is_dir() {
            n += copy_tree(&e.path(), &dest)?;
        } else {
            fs::copy(e.path(), &dest).map_err(|er| format!("{}: {}", e.path().display(), er))?;
            n += 1;
        }
    }
    Ok(n)
}

/// A local "YYYYMMDD-HHMMSS"-like name from unix time (UTC; only for ordering and display).
fn stamp(t: u64) -> String {
    let days = t / 86400;
    let (h, m, s) = ((t % 86400) / 3600, (t % 3600) / 60, t % 60);
    // civil date from days since 1970-01-01 (Howard Hinnant's algorithm)
    let z = days as i64 + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let mo = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + if mo <= 2 { 1 } else { 0 };
    format!("{:04}{:02}{:02}-{:02}{:02}{:02}", y, mo, d, h, m, s)
}

fn is_empty_dir(p: &Path) -> bool {
    fs::read_dir(p).map(|mut r| r.next().is_none()).unwrap_or(true)
}

fn make_backup(game: &Path, saves: &Path) -> Result<String, String> {
    if !saves.is_dir() || is_empty_dir(saves) {
        return Err("no_saves".into());
    }
    let mut name = format!("savegame-{}", stamp(now()));
    let mut i = 1;
    while backups_dir(game).join(&name).exists() {
        i += 1;
        name = format!("savegame-{}-{}", stamp(now()), i);
    }
    copy_tree(saves, &backups_dir(game).join(&name))?;
    // keep the newest few
    for b in list_backups(game).iter().skip(MAX_BACKUPS) {
        let _ = fs::remove_dir_all(backups_dir(game).join(&b.name));
    }
    Ok(name)
}

#[tauri::command]
pub fn backup_saves(state: State<AppState>) -> Result<String, String> {
    if state.game.lock().unwrap().pid.is_some() {
        return Err("game_running".into());
    }
    let (saves, game, _) = current_save_dir(&state)?;
    let name = make_backup(&game, &saves)?;
    record(&game, "backup", &name);
    Ok(name)
}

fn clear_dir(p: &Path) -> Result<(), String> {
    for e in fs::read_dir(p).map_err(|e| e.to_string())?.flatten() {
        let path = e.path();
        if path.is_dir() { fs::remove_dir_all(&path) } else { fs::remove_file(&path) }.map_err(|er| format!("{}: {}", path.display(), er))?;
    }
    Ok(())
}

/// Puts a backup's savegames back; what was there is backed up first (and put back if the copy fails).
#[tauri::command]
pub fn restore_saves(state: State<AppState>, name: String) -> Result<(), String> {
    if state.game.lock().unwrap().pid.is_some() {
        return Err("game_running".into());
    }
    if !is_backup_name(&name) {
        return Err("bad_name".into());
    }
    let (saves, game, custom) = current_save_dir(&state)?;
    if custom && !saves.is_dir() {
        return Err(format!("save_dir_missing: {}", saves.display()));
    }
    let from = backup_content(&backups_dir(&game).join(&name));
    if !from.is_dir() {
        return Err("no_backup".into());
    }
    let safety = if saves.is_dir() && !is_empty_dir(&saves) { Some(make_backup(&game, &saves)?) } else { None };
    fs::create_dir_all(&saves).map_err(|e| e.to_string())?;
    let result = clear_dir(&saves).and_then(|_| copy_tree(&from, &saves));
    if let Err(e) = result {
        if let Some(s) = &safety {
            let _ = clear_dir(&saves);
            let _ = copy_tree(&backups_dir(&game).join(s), &saves);
        }
        return Err(e);
    }
    record(&game, "restored", &name);
    Ok(())
}

/// Another savegame folder (an external drive, a synced folder) or back to the default (`path` None). With `copy`
/// the savegames there now are copied over (nothing in the new folder is overwritten: a clash stops it).
#[tauri::command]
pub async fn set_save_dir(app: tauri::AppHandle, path: Option<String>, copy: bool) -> Result<Option<String>, String> {
    use tauri::Manager;
    use tauri_plugin_dialog::DialogExt;
    let state = app.state::<AppState>();
    if state.game.lock().unwrap().pid.is_some() {
        return Err("game_running".into());
    }
    let (from, game, _) = current_save_dir(&state)?;
    let target: Option<PathBuf> = match path.as_deref() {
        Some("pick") => match app.dialog().file().blocking_pick_folder() {
            Some(p) => Some(p.into_path().map_err(|e| e.to_string())?),
            None => return Ok(None), // the dialog was closed
        },
        Some(p) => Some(PathBuf::from(p)),
        None => None,
    };
    let to = target.clone().unwrap_or_else(|| game.join(DEFAULT_SAVE_DIR));
    if to == from {
        return Ok(Some(to.display().to_string()));
    }
    if to.starts_with(game.join("backups")) {
        return Err("bad_folder".into());
    }
    fs::create_dir_all(&to).map_err(|e| e.to_string())?;
    if copy && from.is_dir() {
        for e in fs::read_dir(&from).map_err(|e| e.to_string())?.flatten() {
            if to.join(e.file_name()).exists() {
                return Err(format!("target_not_empty: {}", e.file_name().to_string_lossy()));
            }
        }
        copy_tree(&from, &to)?;
    }
    {
        let mut cfg = state.config.lock().unwrap();
        cfg.save_dir = target.map(|p| p.display().to_string());
        cfg.save()?;
    }
    record(&game, "save_dir", &to.display().to_string());
    Ok(Some(to.display().to_string()))
}

#[cfg(test)]
mod tests {
    #[test]
    fn stamp_is_a_date() {
        assert_eq!(super::stamp(0), "19700101-000000");
        assert_eq!(super::stamp(1_795_000_000), "20261118-110640");
    }
}
