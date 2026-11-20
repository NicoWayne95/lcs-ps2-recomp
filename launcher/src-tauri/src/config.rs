//! The launcher's own preferences (launcher.json in the app's config folder): the game folder, the language and
//! windowed mode. The game's settings are the runtime's lcs.ini (settings.rs), not here.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use tauri::{AppHandle, Manager, State};
use tauri_plugin_dialog::DialogExt;

use crate::AppState;

pub const RUNTIME_EXE: &str = "LCSrecomp.exe";

#[derive(Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Config {
    pub game_dir: Option<String>,
    pub language: String,
    pub windowed: bool,
    /// the savegame folder the player chose (None: Savegame in the game folder)
    pub save_dir: Option<String>,
    #[serde(skip)]
    pub file: Option<PathBuf>,
}

impl Default for Config {
    fn default() -> Self {
        Config { game_dir: None, language: "en".into(), windowed: false, save_dir: None, file: None }
    }
}

/// The launcher's own folder.
pub fn exe_dir() -> Option<PathBuf> {
    std::env::current_exe().ok()?.parent().map(Path::to_path_buf)
}

/// A game folder for this run only (`--game-dir`), over the saved one.
pub static GAME_DIR_OVERRIDE: std::sync::OnceLock<PathBuf> = std::sync::OnceLock::new();

impl Config {
    /// launcher.json next to the launcher (everything in its own folder, nothing in AppData). One from an older
    /// launcher in the app's config folder is taken over once and removed there.
    pub fn load(app: &AppHandle) -> Config {
        let file = exe_dir().map(|d| d.join("launcher.json"));
        let read = |f: &Path| std::fs::read_to_string(f).ok().and_then(|s| serde_json::from_str::<Config>(&s).ok());
        let mut cfg = file.as_deref().and_then(read);
        let old_dir = app.path().app_config_dir().ok();
        let old = old_dir.as_ref().map(|d| d.join("launcher.json")).filter(|f| f.is_file());
        if let Some(old) = old {
            if cfg.is_none() {
                cfg = read(&old);
            }
            let _ = std::fs::remove_file(&old);
            if let Some(d) = &old_dir {
                let _ = std::fs::remove_dir(d); // only when empty
            }
        }
        let migrated = cfg.is_some() && file.as_ref().is_some_and(|f| !f.is_file());
        let mut cfg = cfg.unwrap_or_default();
        cfg.file = file;
        if migrated {
            let _ = cfg.save();
        }
        if cfg.language != "de" {
            cfg.language = "en".into();
        }
        cfg
    }

    pub fn save(&self) -> Result<(), String> {
        let file = self.file.as_ref().ok_or("no config folder")?;
        if let Some(dir) = file.parent() {
            std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
        }
        let text = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        std::fs::write(file, text).map_err(|e| e.to_string())
    }

    /// The game folder: the one chosen, else next to the launcher (`game\` below it, or its own folder), else the
    /// nearest folder above it that holds the runtime (a development checkout).
    pub fn resolve_game_dir(&self) -> Option<PathBuf> {
        if let Some(d) = GAME_DIR_OVERRIDE.get() {
            return Some(d.clone());
        }
        if let Some(d) = &self.game_dir {
            let p = PathBuf::from(d);
            if p.is_dir() {
                return Some(p);
            }
        }
        let exe = std::env::current_exe().ok()?;
        let here = exe.parent()?;
        for candidate in [here.join("game"), here.to_path_buf()] {
            if candidate.join(RUNTIME_EXE).is_file() {
                return Some(candidate);
            }
        }
        let mut up: Option<&Path> = here.parent();
        for _ in 0..6 {
            let dir = up?;
            if dir.join(RUNTIME_EXE).is_file() {
                return Some(dir.to_path_buf());
            }
            up = dir.parent();
        }
        None
    }
}

#[derive(Serialize)]
pub struct ConfigView {
    language: String,
    windowed: bool,
    game_dir: Option<String>,
    launcher_version: String,
}

#[tauri::command]
pub fn get_config(app: AppHandle, state: State<AppState>) -> ConfigView {
    let cfg = state.config.lock().unwrap();
    ConfigView {
        language: cfg.language.clone(),
        windowed: cfg.windowed,
        game_dir: cfg.resolve_game_dir().map(|p| p.display().to_string()),
        launcher_version: app.package_info().version.to_string(),
    }
}

#[tauri::command]
pub fn set_language(state: State<AppState>, language: String) -> Result<(), String> {
    let mut cfg = state.config.lock().unwrap();
    cfg.language = if language == "de" { "de".into() } else { "en".into() };
    cfg.save()
}

#[tauri::command]
pub fn set_windowed(state: State<AppState>, windowed: bool) -> Result<(), String> {
    let mut cfg = state.config.lock().unwrap();
    cfg.windowed = windowed;
    cfg.save()
}

/// The folder picker for an existing game folder (one that holds LCSrecomp.exe).
#[tauri::command]
pub async fn pick_game_dir(app: AppHandle) -> Result<Option<String>, String> {
    let Some(picked) = app.dialog().file().blocking_pick_folder() else {
        return Ok(None);
    };
    let path = picked.into_path().map_err(|e| e.to_string())?;
    if !path.join(RUNTIME_EXE).is_file() {
        return Err(format!("{} not found in {}", RUNTIME_EXE, path.display()));
    }
    let state = app.state::<AppState>();
    let mut cfg = state.config.lock().unwrap();
    cfg.game_dir = Some(path.display().to_string());
    cfg.save()?;
    Ok(cfg.game_dir.clone())
}
