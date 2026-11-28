//! The release check: GitHub's latest release of the project (public metadata only, no token), its signed manifest
//! (update.rs) downloaded and verified right away, so the page only offers what can be installed. Release notes are
//! shown as text, never run. Installing (the kit update, the launcher's installer) starts only on the player's click.

use serde::Serialize;
use std::sync::atomic::AtomicBool;
use std::sync::Arc;
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager, State};

use crate::update::{self, Release, Source};
use crate::AppState;

const LATEST: &str = "https://api.github.com/repos/NicoWayne95/lcs-ps2-recomp/releases/latest";
const DOWNLOADS: &str = "https://github.com/NicoWayne95/lcs-ps2-recomp/releases/download/";

/// The last verified release (what "Install update" installs).
pub struct Checked {
    pub release: Release,
    pub source: Source,
}

#[derive(Serialize)]
pub struct UpdateInfo {
    /// offline | no_release | up_to_date | available | unknown_installed | launcher_required | no_manifest |
    /// bad_signature | error
    state: String,
    latest_tag: Option<String>,
    latest_name: Option<String>,
    notes: Option<String>,
    published_at: Option<String>,
    installed: Option<String>,
    detail: Option<String>,
    /// the release's game version (from the signed manifest)
    release_version: Option<String>,
    download_mb: Option<f64>,
    /// launcher_required: the launcher version the release needs, and whether its installer is in the release
    min_launcher: Option<String>,
    launcher_setup: bool,
}

fn version_of(tag: &str) -> Option<semver::Version> {
    semver::Version::parse(tag.trim().trim_start_matches(['v', 'V'])).ok()
}

fn tag_ok(tag: &str) -> bool {
    !tag.is_empty() && tag.len() < 64 && tag.chars().all(|c| c.is_ascii_alphanumeric() || "._-".contains(c))
}

async fn get_small(client: &reqwest::Client, url: &str) -> Result<Vec<u8>, String> {
    let resp = client.get(url).send().await.map_err(|e| e.to_string())?;
    if !resp.status().is_success() {
        return Err(format!("HTTP {}", resp.status().as_u16()));
    }
    if resp.content_length().unwrap_or(0) > update::MAX_MANIFEST {
        return Err("too large".into());
    }
    let b = resp.bytes().await.map_err(|e| e.to_string())?;
    if b.len() as u64 > update::MAX_MANIFEST {
        return Err("too large".into());
    }
    Ok(b.to_vec())
}

/// Compares a verified release with the installed game and this launcher (shared with the headless test).
pub fn state_for(rel: &Release, installed: Option<&str>, launcher: &semver::Version) -> &'static str {
    if &rel.min_launcher > launcher {
        return "launcher_required";
    }
    match installed.and_then(version_of) {
        Some(have) if rel.version > have => "available",
        Some(_) => "up_to_date",
        None => "unknown_installed",
    }
}

#[tauri::command]
pub async fn check_updates(app: AppHandle, state: State<'_, AppState>) -> Result<UpdateInfo, String> {
    let dir = state.config.lock().unwrap().resolve_game_dir();
    let installed = dir.and_then(|d| {
        let text = std::fs::read_to_string(d.join("version.json")).ok()?;
        let v: serde_json::Value = serde_json::from_str(&text).ok()?;
        v.get("version")?.as_str().map(str::to_owned)
    });
    let mut info = UpdateInfo {
        state: "error".into(),
        latest_tag: None,
        latest_name: None,
        notes: None,
        published_at: None,
        installed: installed.clone(),
        detail: None,
        release_version: None,
        download_mb: None,
        min_launcher: None,
        launcher_setup: false,
    };
    *state.checked.lock().unwrap() = None;
    let client = reqwest::Client::builder()
        .user_agent(format!("LCS-Recomp-Launcher/{}", app.package_info().version))
        .timeout(Duration::from_secs(10))
        .https_only(true)
        .build()
        .map_err(|e| e.to_string())?;
    let resp = match client.get(LATEST).header("Accept", "application/vnd.github+json").send().await {
        Ok(r) => r,
        Err(e) => {
            info.state = "offline".into();
            info.detail = Some(e.to_string());
            return Ok(info);
        }
    };
    if resp.status().as_u16() == 404 {
        info.state = "no_release".into();
        return Ok(info);
    }
    if !resp.status().is_success() {
        info.detail = Some(format!("HTTP {}", resp.status().as_u16()));
        return Ok(info);
    }
    let json: serde_json::Value = match resp.json().await {
        Ok(j) => j,
        Err(e) => {
            info.detail = Some(e.to_string());
            return Ok(info);
        }
    };
    let tag = json.get("tag_name").and_then(|v| v.as_str()).map(str::to_owned);
    let Some(tag) = tag.filter(|t| tag_ok(t)) else {
        info.detail = Some("release without a usable tag".into());
        return Ok(info);
    };
    info.latest_name = json.get("name").and_then(|v| v.as_str()).map(|s| s.chars().take(120).collect());
    info.notes = json.get("body").and_then(|v| v.as_str()).map(|s| s.chars().take(8000).collect());
    info.published_at = json.get("published_at").and_then(|v| v.as_str()).map(str::to_owned);
    info.latest_tag = Some(tag.clone());

    // the signed manifest: only a release that has one can be installed from here
    let names: Vec<&str> = json
        .get("assets")
        .and_then(|a| a.as_array())
        .map(|a| a.iter().filter_map(|x| x.get("name").and_then(|n| n.as_str())).collect())
        .unwrap_or_default();
    if !names.contains(&update::MANIFEST) || !names.contains(&update::SIGNATURE) {
        info.state = "no_manifest".into();
        return Ok(info);
    }
    let base = format!("{}{}/", DOWNLOADS, tag);
    let fetched = async {
        let m = get_small(&client, &format!("{}{}", base, update::MANIFEST)).await?;
        let s = get_small(&client, &format!("{}{}", base, update::SIGNATURE)).await?;
        Ok::<_, String>((m, String::from_utf8_lossy(&s).to_string()))
    }
    .await;
    let (bytes, sig) = match fetched {
        Ok(x) => x,
        Err(e) => {
            info.detail = Some(format!("manifest: {}", e));
            return Ok(info);
        }
    };
    let rel = match update::verify(&bytes, &sig) {
        Ok(r) => r,
        Err(e) => {
            info.state = match e.as_str() {
                "bad_signature" => "bad_signature",
                "manifest_format" => "launcher_required",
                _ => "error",
            }
            .into();
            info.detail = Some(e);
            return Ok(info);
        }
    };
    let own = semver::Version::parse(&app.package_info().version.to_string()).unwrap_or(semver::Version::new(0, 0, 0));
    info.state = state_for(&rel, installed.as_deref(), &own).into();
    info.release_version = Some(rel.version.to_string());
    info.download_mb = Some(rel.kit.size as f64 / 1e6);
    info.min_launcher = Some(rel.min_launcher.to_string());
    info.launcher_setup = rel.launcher.as_ref().is_some_and(|(v, _)| v >= &rel.min_launcher && v > &own);
    *state.checked.lock().unwrap() = Some(Checked { release: rel, source: Source::Http(base) });
    Ok(info)
}

#[derive(Clone, Serialize)]
struct Done {
    ok: bool,
    error: Option<String>,
}

/// Installs the checked release (update.rs apply) in the background: `update-progress`, then `update-done`.
#[tauri::command]
pub fn install_update(app: AppHandle, state: State<AppState>) -> Result<(), String> {
    let (rel, src) = {
        let c = state.checked.lock().unwrap();
        let c = c.as_ref().ok_or("not_checked")?;
        (c.release.clone(), c.source.clone())
    };
    let own = semver::Version::parse(&app.package_info().version.to_string()).unwrap_or(semver::Version::new(0, 0, 0));
    if rel.min_launcher > own {
        return Err("launcher_required".into());
    }
    let mut slot = state.preparing.lock().unwrap();
    if slot.is_some() {
        return Err("already_preparing".into());
    }
    if state.game.lock().unwrap().pid.is_some() {
        return Err("game_running".into());
    }
    let kit = crate::prepare::find_kit().ok_or("kit_missing")?;
    let game = state.config.lock().unwrap().resolve_game_dir().ok_or("no_game_dir")?;
    let cancel = Arc::new(AtomicBool::new(false));
    *slot = Some(cancel.clone());
    let a = app.clone();
    std::thread::spawn(move || {
        let emit = move |p: update::UpdateProgress| {
            let _ = a.emit("update-progress", p);
        };
        let result = update::apply(&kit, &game, &rel, &src, &emit, cancel);
        crate::game::log_line(&game, &format!("update to {}: {:?}", rel.version, result));
        if result.is_ok() {
            crate::library::record(&game, "updated", &rel.version.to_string());
        }
        *app.state::<AppState>().preparing.lock().unwrap() = None;
        let _ = app.emit("update-done", Done { ok: result.is_ok(), error: result.err() });
    });
    Ok(())
}

/// The launcher's own update: its installer from the checked release (SHA-256 from the signed manifest), started;
/// the launcher closes.
#[tauri::command]
pub async fn install_launcher(app: AppHandle, state: State<'_, AppState>) -> Result<(), String> {
    let (asset, src) = {
        let c = state.checked.lock().unwrap();
        let c = c.as_ref().ok_or("not_checked")?;
        let (_, a) = c.release.launcher.clone().ok_or("no_launcher_setup")?;
        (a, c.source.clone())
    };
    if state.preparing.lock().unwrap().is_some() {
        return Err("already_preparing".into());
    }
    let game = state.config.lock().unwrap().resolve_game_dir().ok_or("no_game_dir")?;
    let setup = tauri::async_runtime::spawn_blocking(move || update::fetch_setup(&game, &src, &asset, &AtomicBool::new(false)))
        .await
        .map_err(|e| e.to_string())??;
    std::process::Command::new(&setup).spawn().map_err(|e| format!("start_failed: {}", e))?;
    app.exit(0);
    Ok(())
}
