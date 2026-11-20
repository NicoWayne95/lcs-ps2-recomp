//! The game folder's state and the runtime's start / exit.
//!
//! What the runtime needs (LCSrecomp.exe's release start, src/main.cpp releaseStart / mountIso, 2026-11-17):
//! LCSrecomp.exe with its DLLs and the player's ISO as gamedata\LCS.iso - the only game data. The runtime maps the ISO
//! as the disc and takes the ELF and the IOP modules out of it into cache\ itself. It writes Savegame\ (or the --save-dir it gets),
//! logs\last_run.log and lcs.ini. It is started with no arguments and the game folder as its working folder.

use serde::Serialize;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Instant;
use tauri::{AppHandle, Emitter, Manager, State};

use crate::config::RUNTIME_EXE;
use crate::AppState;

#[derive(Default)]
pub struct Running {
    pub pid: Option<u32>,
}

#[derive(Serialize)]
pub struct Status {
    game_dir: Option<String>,
    runtime: bool,
    iso: bool,
    pub(crate) complete: bool,
    running: bool,
    installed_version: Option<String>,
    /// the kit's version (what a build makes now) and the previous version kept in _backup\
    kit_version: Option<String>,
    backup_version: Option<String>,
    pub(crate) missing: Vec<String>,
}

fn kit_version() -> Option<String> {
    let kit = crate::prepare::find_kit()?;
    let v: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(kit.join("kit.json")).ok()?).ok()?;
    v.get("version")?.as_str().map(str::to_owned)
}

fn installed_version(dir: &Path) -> Option<String> {
    // a release writes version.json next to the runtime; a local build has none (shown as such, never invented)
    let text = std::fs::read_to_string(dir.join("version.json")).ok()?;
    let v: serde_json::Value = serde_json::from_str(&text).ok()?;
    v.get("version")?.as_str().map(str::to_owned)
}

pub fn status_of(dir: Option<PathBuf>, running: bool) -> Status {
    let Some(dir) = dir else {
        return Status {
            game_dir: None,
            runtime: false,
            iso: false,
            complete: false,
            running,
            installed_version: None,
            kit_version: kit_version(),
            backup_version: None,
            missing: vec!["game folder".into()],
        };
    };
    let runtime = dir.join(RUNTIME_EXE).is_file();
    let iso = dir.join("gamedata").join("LCS.iso").is_file();
    let mut missing = Vec::new();
    if !runtime {
        missing.push(RUNTIME_EXE.to_string());
    }
    if !iso {
        missing.push("gamedata\\LCS.iso".into());
    }
    Status {
        installed_version: installed_version(&dir),
        kit_version: kit_version(),
        backup_version: crate::prepare::backup_version(&dir),
        game_dir: Some(dir.display().to_string()),
        complete: missing.is_empty(),
        runtime,
        iso,
        running,
        missing,
    }
}

pub fn log_line(dir: &Path, line: &str) {
    let logs = dir.join("logs");
    let _ = std::fs::create_dir_all(&logs);
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(logs.join("launcher.log")) {
        let secs = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let _ = writeln!(f, "[{}] {}", secs, line);
    }
}

#[tauri::command]
pub fn get_status(state: State<AppState>) -> Status {
    let dir = state.config.lock().unwrap().resolve_game_dir();
    let running = state.game.lock().unwrap().pid.is_some();
    status_of(dir, running)
}

#[derive(Clone, Serialize)]
struct Exited {
    code: Option<i32>,
    seconds: u64,
    log_tail: Vec<String>,
}

fn log_tail(dir: &Path, n: usize) -> Vec<String> {
    let text = std::fs::read_to_string(dir.join("logs").join("last_run.log")).unwrap_or_default();
    let lines: Vec<&str> = text.lines().collect();
    lines[lines.len().saturating_sub(n)..].iter().map(|s| s.to_string()).collect()
}

/// Starts LCSrecomp.exe from the game folder (no arguments), and reports its exit as the `game-exited` event.
#[tauri::command]
pub fn launch_game(app: AppHandle, state: State<AppState>) -> Result<u32, String> {
    let (dir, windowed, cfg) = {
        let cfg = state.config.lock().unwrap();
        (cfg.resolve_game_dir(), cfg.windowed, cfg.clone())
    };
    if state.game.lock().unwrap().pid.is_some() {
        return Err("already_running".into());
    }
    let st = status_of(dir.clone(), false);
    let dir = dir.ok_or("no_game_dir")?;
    if !st.complete {
        return Err(format!("incomplete: {}", st.missing.join(", ")));
    }
    let save_dir = crate::library::check_save_dir(&cfg, &dir)?;
    let mut cmd = Command::new(dir.join(RUNTIME_EXE));
    cmd.current_dir(&dir).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
    if let Some(s) = &save_dir {
        cmd.arg("--save-dir").arg(s);
    }
    // LCS_FULLSCREEN is the runtime's own switch (src/main.cpp): 0 windowed, 1 fullscreen (its default)
    cmd.env("LCS_FULLSCREEN", if windowed { "0" } else { "1" });
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    let mut child = cmd.spawn().map_err(|e| {
        log_line(&dir, &format!("start failed: {}", e));
        format!("start_failed: {}", e)
    })?;
    let pid = child.id();
    state.game.lock().unwrap().pid = Some(pid);
    log_line(&dir, &format!("started {} (pid {}, windowed {})", RUNTIME_EXE, pid, windowed));
    let _ = app.emit("game-started", pid);
    let started = Instant::now();
    std::thread::spawn(move || {
        let code = child.wait().ok().and_then(|s| s.code());
        let seconds = started.elapsed().as_secs();
        log_line(&dir, &format!("exited with {:?} after {} s", code, seconds));
        crate::library::record_run(&dir, seconds, code);
        app.state::<AppState>().game.lock().unwrap().pid = None;
        let tail = if code == Some(0) { Vec::new() } else { log_tail(&dir, 25) };
        let _ = app.emit("game-exited", Exited { code, seconds, log_tail: tail });
    });
    Ok(pid)
}

fn explorer(path: &Path) -> Result<(), String> {
    Command::new("explorer.exe").arg(path).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// Opens the game folder, its saves or its logs in Explorer.
#[tauri::command]
pub fn open_folder(state: State<AppState>, kind: String) -> Result<(), String> {
    let dir = state.config.lock().unwrap().resolve_game_dir().ok_or("no_game_dir")?;
    let target = match kind.as_str() {
        "saves" => crate::library::save_dir(&state.config.lock().unwrap(), &dir),
        "logs" => dir.join("logs"),
        _ => dir,
    };
    std::fs::create_dir_all(&target).map_err(|e| e.to_string())?;
    explorer(&target)
}

pub const REPO_URL: &str = "https://github.com/NicoWayne95/lcs-ps2-recomp";

/// Opens one of the project's fixed pages in the default browser (no URL from outside is ever opened).
#[tauri::command]
pub fn open_link(kind: String) -> Result<(), String> {
    let url = match kind.as_str() {
        "releases" => format!("{}/releases", REPO_URL),
        _ => REPO_URL.to_string(),
    };
    Command::new("explorer.exe").arg(url).spawn().map(|_| ()).map_err(|e| e.to_string())
}
