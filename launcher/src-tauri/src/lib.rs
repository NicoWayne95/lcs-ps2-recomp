//! LCS Recomp Launcher backend: the game folder, the runtime's start and exit, the ISO check, the game's preparation
//! (prepare.rs: the ISO -> a built LCSrecomp.exe on this PC), the runtime's own settings file (lcs.ini) and the GitHub
//! release check. The launcher never changes the runtime; it starts LCSrecomp.exe the way a double click does.
//!
//! Headless (tests, output in the console):
//!   lcs-recomp-launcher.exe --prepare <iso> <game folder>            the preparation
//!   lcs-recomp-launcher.exe --update-from <release folder> <game folder>   a release from a folder (update.rs)
//! For other launchers and shortcuts:
//!   lcs-recomp-launcher.exe --play [game flags]      starts the built game directly (flags go to LCSrecomp.exe)
//!   lcs-recomp-launcher.exe --game-dir <folder> ...  uses that game folder for this run
//! Everything the launcher writes stays in its own folder: launcher.json, webview\, game\.

mod config;
mod game;
mod iso;
mod library;
mod prepare;
mod settings;
mod update;
mod updates;

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Emitter, Manager, State};

pub struct AppState {
    pub config: Mutex<config::Config>,
    pub game: Mutex<game::Running>,
    pub preparing: Mutex<Option<Arc<AtomicBool>>>,
    pub checked: Mutex<Option<updates::Checked>>,
}

/// The game folder to prepare into: the one in use, else `game\` next to the launcher (the installer makes it).
fn target_game_dir(state: &AppState) -> Option<PathBuf> {
    state.config.lock().unwrap().resolve_game_dir().or_else(|| {
        let exe = std::env::current_exe().ok()?;
        Some(exe.parent()?.join("game"))
    })
}

#[derive(Clone, serde::Serialize)]
struct Done {
    ok: bool,
    error: Option<String>,
}

#[tauri::command]
fn start_prepare(app: AppHandle, state: State<AppState>, iso_path: String) -> Result<(), String> {
    let mut slot = state.preparing.lock().unwrap();
    if slot.is_some() {
        return Err("already_preparing".into());
    }
    if state.game.lock().unwrap().pid.is_some() {
        return Err("game_running".into());
    }
    let kit = prepare::find_kit().ok_or("kit_missing")?;
    let game = target_game_dir(&state).ok_or("no_game_dir")?;
    std::fs::create_dir_all(&game).map_err(|e| e.to_string())?;
    let cancel = Arc::new(AtomicBool::new(false));
    *slot = Some(cancel.clone());
    let a = app.clone();
    std::thread::spawn(move || {
        let emit = move |p: prepare::Progress| {
            let _ = a.emit("prepare-progress", p);
        };
        let ctx = prepare::Ctx::new(kit, game.clone(), PathBuf::from(iso_path), &emit, cancel);
        let result = prepare::prepare(&ctx);
        if let Err(e) = &result {
            ctx.log(&format!("== failed: {} ==", e));
        }
        let st = app.state::<AppState>();
        *st.preparing.lock().unwrap() = None;
        {
            let mut cfg = st.config.lock().unwrap();
            if result.is_ok() {
                let v = std::fs::read_to_string(game.join("version.json")).ok()
                    .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
                    .and_then(|v| v.get("version")?.as_str().map(str::to_owned))
                    .unwrap_or_default();
                library::record(&game, "built", &v);
            }
            if result.is_ok() && cfg.game_dir.is_none() {
                cfg.game_dir = Some(game.display().to_string());
                let _ = cfg.save();
            }
        }
        let _ = app.emit("prepare-done", Done { ok: result.is_ok(), error: result.err() });
    });
    Ok(())
}

#[tauri::command]
fn cancel_prepare(state: State<AppState>) {
    if let Some(c) = state.preparing.lock().unwrap().as_ref() {
        c.store(true, Ordering::Relaxed);
    }
}

#[tauri::command]
fn prepare_info(state: State<AppState>) -> serde_json::Value {
    serde_json::json!({
        "kit": prepare::find_kit().map(|p| p.display().to_string()),
        "target": target_game_dir(&state).map(|p| p.display().to_string()),
        "running": state.preparing.lock().unwrap().is_some(),
        "downloads_mb": prepare::find_kit()
            .and_then(|k| prepare::tools_of(&k).ok())
            .map(|t| t.iter().map(|t| t.size).sum::<u64>() / 1_000_000),
    })
}

/// Puts the previous version (_backup\) back, swapping it with the installed one.
#[tauri::command]
fn restore_previous(state: State<AppState>) -> Result<(), String> {
    if state.preparing.lock().unwrap().is_some() {
        return Err("already_preparing".into());
    }
    if state.game.lock().unwrap().pid.is_some() {
        return Err("game_running".into());
    }
    let game = state.config.lock().unwrap().resolve_game_dir().ok_or("no_game_dir")?;
    let r = prepare::restore_backup(&game);
    game::log_line(&game, &format!("restore previous version: {:?}", r));
    r
}

fn headless_update(release: &str, game: &str) -> i32 {
    let Some(kit) = prepare::find_kit() else {
        eprintln!("no kit next to the launcher");
        return 2;
    };
    let rel = match update::read_dir_release(std::path::Path::new(release)) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("release refused: {}", e);
            return 3;
        }
    };
    let own = semver::Version::parse(env!("CARGO_PKG_VERSION")).unwrap();
    let installed = std::fs::read_to_string(std::path::Path::new(game).join("version.json"))
        .ok()
        .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|v| v.get("version")?.as_str().map(str::to_owned));
    let st = updates::state_for(&rel, installed.as_deref(), &own);
    println!("release {} (launcher >= {}), installed {:?}: {}", rel.version, rel.min_launcher, installed, st);
    if st == "launcher_required" {
        return 4;
    }
    let last = Mutex::new((String::new(), -1i32));
    let show = |p: update::UpdateProgress| {
        let mut l = last.lock().unwrap();
        let key = format!("{}{:?}", p.phase, p.stage);
        let pct = p.percent as i32;
        if l.0 != key || l.1 != pct {
            *l = (key, pct);
            println!("[{:>3}%] {} {} {}", pct, p.phase, p.stage.unwrap_or(""), p.detail);
        }
    };
    let src = update::Source::Dir(PathBuf::from(release));
    match update::apply(&kit, std::path::Path::new(game), &rel, &src, &show, Arc::new(AtomicBool::new(false))) {
        Ok(()) => {
            println!("updated to {}", rel.version);
            0
        }
        Err(e) => {
            eprintln!("update failed: {}", e);
            1
        }
    }
}

fn headless_prepare(iso: &str, game: &str) -> i32 {
    let Some(kit) = prepare::find_kit() else {
        eprintln!("no kit next to the launcher");
        return 2;
    };
    let last = Mutex::new((usize::MAX, -1i32));
    let show = |p: prepare::Progress| {
        let mut l = last.lock().unwrap();
        let pct = p.percent as i32;
        if l.0 != p.stage_index || l.1 != pct {
            *l = (p.stage_index, pct);
            println!("[{:>3}%] {} {}", pct, p.stage, p.detail);
        }
    };
    let ctx = prepare::Ctx::new(kit, PathBuf::from(game), PathBuf::from(iso), &show, Arc::new(AtomicBool::new(false)));
    match prepare::prepare(&ctx) {
        Ok(()) => {
            println!("prepared: {}", game);
            0
        }
        Err(e) => {
            ctx.log(&format!("== failed: {} ==", e));
            eprintln!("failed: {}", e);
            1
        }
    }
}

/// `--play`: starts the built game the way the Play button does (no window of the launcher's), then ends. The game's
/// own flags after it go to LCSrecomp.exe (`--windowed`, `--fullscreen`, ...).
fn play_direct(game_args: &[String]) -> i32 {
    let saved: Option<config::Config> = config::exe_dir()
        .and_then(|d| std::fs::read_to_string(d.join("launcher.json")).ok())
        .and_then(|s| serde_json::from_str(&s).ok());
    let cfg = saved.unwrap_or_default();
    let Some(dir) = cfg.resolve_game_dir() else {
        eprintln!("no game folder: prepare the game in the launcher first");
        return 2;
    };
    let st = game::status_of(Some(dir.clone()), false);
    if !st.complete {
        eprintln!("the game folder {} is missing: {}", dir.display(), st.missing.join(", "));
        return 2;
    }
    let save_dir = match library::check_save_dir(&cfg, &dir) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("{}", e);
            return 2;
        }
    };
    let mut cmd = std::process::Command::new(dir.join(config::RUNTIME_EXE));
    cmd.current_dir(&dir).args(game_args);
    if let Some(s) = save_dir.filter(|_| !game_args.iter().any(|a| a == "--save-dir")) {
        cmd.arg("--save-dir").arg(s);
    }
    if !game_args.iter().any(|a| a == "--windowed" || a == "--fullscreen") {
        cmd.arg(if cfg.windowed { "--windowed" } else { "--fullscreen" });
    }
    match cmd.spawn() {
        Ok(_) => {
            game::log_line(&dir, "started by --play");
            0
        }
        Err(e) => {
            eprintln!("start failed: {}", e);
            1
        }
    }
}

pub fn run() {
    let mut args: Vec<String> = std::env::args().collect();
    if args.len() == 4 && args[1] == "--prepare" {
        std::process::exit(headless_prepare(&args[2], &args[3]));
    }
    if args.len() == 4 && args[1] == "--update-from" {
        std::process::exit(headless_update(&args[2], &args[3]));
    }
    // --game-dir <folder>: this run uses that game folder (the saved choice stays as it is)
    if let Some(i) = args.iter().position(|a| a == "--game-dir") {
        let Some(dir) = args.get(i + 1).cloned() else {
            eprintln!("--game-dir needs a folder");
            std::process::exit(2);
        };
        let dir = std::path::absolute(&dir).unwrap_or_else(|_| PathBuf::from(&dir));
        let _ = config::GAME_DIR_OVERRIDE.set(dir);
        args.drain(i..i + 2);
    }
    if let Some(i) = args.iter().position(|a| a == "--play") {
        std::process::exit(play_direct(&args[i + 1..]));
    }
    if args.len() > 1 {
        eprintln!("usage: lcs-recomp-launcher.exe [--game-dir <folder>] [--play [game flags]] | --prepare <iso> <game folder> | --update-from <release folder> <game folder>");
        std::process::exit(2);
    }
    // the WebView's data (cache, its own settings) next to the launcher, not in AppData
    if let Some(dir) = config::exe_dir() {
        std::env::set_var("WEBVIEW2_USER_DATA_FOLDER", dir.join("webview"));
    }
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            let cfg = config::Config::load(app.handle());
            app.manage(AppState { config: Mutex::new(cfg), game: Mutex::new(game::Running::default()), preparing: Mutex::new(None), checked: Mutex::new(None) });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            config::get_config,
            config::set_language,
            config::set_windowed,
            config::pick_game_dir,
            game::get_status,
            game::launch_game,
            game::open_folder,
            game::open_link,
            iso::pick_iso,
            iso::validate_iso,
            settings::get_settings,
            settings::set_setting,
            updates::check_updates,
            updates::install_update,
            updates::install_launcher,
            restore_previous,
            library::get_library,
            library::backup_saves,
            library::restore_saves,
            library::set_save_dir,
            start_prepare,
            cancel_prepare,
            prepare_info,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the LCS Recomp Launcher");
}
