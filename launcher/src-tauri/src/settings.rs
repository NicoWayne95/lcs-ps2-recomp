//! The runtime's own settings file, lcs.ini next to LCSrecomp.exe ("LABEL=index" a line; written by the pause menu,
//! read at the runtime's start - src/lcs/lcs_menu.cpp). Only settings the runtime really applies are offered here;
//! the indexes are the pause menu's. Lines the launcher doesn't know are kept as they are.

use serde::Serialize;
use std::path::PathBuf;
use tauri::State;

use crate::AppState;

/// (key in lcs.ini, number of choices) - the menu's rows that change the game (lcs_menu.cpp / main.cpp kRows).
/// Not offered: DRAW DISTANCE (not wired to the game yet), BETA POLICE CARS (no visible effect yet).
pub const KNOWN: [(&str, u32); 10] = [
    ("SCREEN RESOLUTION", 5), // Original 640x448, 1080p, 1440p, 4K, the window's size
    ("WIDE SCREEN", 2),
    ("FRAME SYNC", 2),        // VSync
    ("FRAME LIMITER", 4),     // 30, 60, 120, off
    ("MSAA", 4),              // off, 2x, 4x, 8x
    ("TEXTURE FILTERING", 5), // off, 2x, 4x, 8x, 16x
    ("TRAILS", 2),
    ("MOUSE SENSITIVITY", 10),
    ("INVERT MOUSE", 2),
    ("RAW INPUT", 2),
];

/// lcs.ini in the game folder - before the game is built too (the user's ask 2026-11-18: the graphics options set
/// first): then the folder the game will be prepared into (game\ next to the launcher), made when a value is saved.
fn ini_path(state: &State<AppState>) -> Result<PathBuf, String> {
    let dir = state
        .config
        .lock()
        .unwrap()
        .resolve_game_dir()
        .or_else(|| crate::config::exe_dir().map(|d| d.join("game")))
        .ok_or("no_game_dir")?;
    Ok(dir.join("lcs.ini"))
}

#[derive(Serialize)]
pub struct Setting {
    key: String,
    value: Option<u32>,
    choices: u32,
}

#[tauri::command]
pub fn get_settings(state: State<AppState>) -> Result<Vec<Setting>, String> {
    let text = std::fs::read_to_string(ini_path(&state)?).unwrap_or_default();
    Ok(KNOWN
        .iter()
        .map(|(k, n)| {
            let value = text
                .lines()
                .filter_map(|l| l.split_once('='))
                .find(|(key, _)| key.trim() == *k)
                .and_then(|(_, v)| v.trim().parse::<u32>().ok())
                .filter(|v| v < n);
            Setting { key: k.to_string(), value, choices: *n }
        })
        .collect())
}

#[tauri::command]
pub fn set_setting(state: State<AppState>, key: String, value: u32) -> Result<(), String> {
    if state.game.lock().unwrap().pid.is_some() {
        return Err("game_running".into()); // the running game owns lcs.ini (it writes it from its menu)
    }
    let Some((_, choices)) = KNOWN.iter().find(|(k, _)| *k == key) else {
        return Err("unknown_setting".into());
    };
    if value >= *choices {
        return Err("out_of_range".into());
    }
    let path = ini_path(&state)?;
    let text = std::fs::read_to_string(&path).unwrap_or_else(|_| "; LCS Recomp settings (pause menu)\n".into());
    let mut found = false;
    let mut lines: Vec<String> = text
        .lines()
        .map(|l| match l.split_once('=') {
            Some((k, _)) if k.trim() == key => {
                found = true;
                format!("{}={}", key, value)
            }
            _ => l.to_string(),
        })
        .collect();
    if !found {
        lines.push(format!("{}={}", key, value));
    }
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let tmp = path.with_extension("ini.tmp");
    std::fs::write(&tmp, lines.join("\n") + "\n").map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, &path).map_err(|e| e.to_string())
}
