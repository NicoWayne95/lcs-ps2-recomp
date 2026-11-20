//! Installing a release: its signed manifest (release-tool/: manifest.json + manifest.json.sig, Ed25519, the public
//! key below) -> the kit zip into <game>\_update\<version>, checked (size, SHA-256, every file against the manifest)
//! -> unpacked into kit.new next to the kit -> kit becomes kit.old, kit.new becomes kit -> the game is rebuilt in the
//! existing build tree (prepare.rs: only what changed) -> kit.old and the download are removed. Any failure before
//! the new build is in puts kit.old back; the installed game is replaced only after a successful build (prepare's
//! install stage, which keeps the previous version in _backup\). saves\, lcs.ini and the ISO are never touched.
//!
//! Where a release comes from: GitHub (updates.rs: the project's releases, HTTPS only) or a local folder (the
//! headless test: `--update-from <folder> <game folder>`). Nothing in a manifest is run; the launcher's own update is
//! the Inno Setup installer the manifest names (its SHA-256 checked), started only on the player's click.

use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use serde::Serialize;
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Instant;

use crate::prepare;

/// The release signing key's public half (release-tool: `lcs-release pubkey`; the private half stays with the
/// maintainer in %USERPROFILE%\.lcs-release).
pub const PUBLIC_KEY_HEX: &str = "3827212aa6258c89df020c17d0d53c08571a21f9ecd56e9a58202cbd4c7025e4";
pub const MANIFEST: &str = "manifest.json";
pub const SIGNATURE: &str = "manifest.json.sig";
pub const MAX_MANIFEST: u64 = 16 << 20;
const MAX_KIT: u64 = 2 << 30;
const MAX_SETUP: u64 = 512 << 20;

/// Set while an update unpacks or swaps the kit (find_kit's recovery must not touch kit.new then).
static UPDATING: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Serialize)]
pub struct Asset {
    pub file: String,
    pub size: u64,
    pub sha256: String,
}

#[derive(Clone)]
pub struct Release {
    pub version: semver::Version,
    pub min_launcher: semver::Version,
    pub kit: Asset,
    pub launcher: Option<(semver::Version, Asset)>,
    files: Value,
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

fn unhex(s: &str) -> Option<Vec<u8>> {
    let s = s.trim();
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok()).collect()
}

pub fn safe_name(s: &str) -> bool {
    !s.is_empty() && s.len() < 128 && !s.starts_with('.') && s.chars().all(|c| c.is_ascii_alphanumeric() || "._-".contains(c))
}

fn asset(v: &Value, max: u64) -> Result<Asset, String> {
    let file = v.get("file").and_then(|x| x.as_str()).unwrap_or_default();
    let size = v.get("size").and_then(|x| x.as_u64()).unwrap_or(0);
    let sha = v.get("sha256").and_then(|x| x.as_str()).unwrap_or_default();
    if !safe_name(file) || size == 0 || size > max || sha.len() != 64 || !sha.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err(format!("manifest_invalid: asset {}", file));
    }
    Ok(Asset { file: file.into(), size, sha256: sha.to_ascii_lowercase() })
}

/// The signature first (over the exact bytes), then the manifest's content. Errors: "bad_signature",
/// "manifest_invalid: ...", "manifest_format" (a newer format than this launcher reads).
pub fn verify(bytes: &[u8], sig_text: &str) -> Result<Release, String> {
    let key: [u8; 32] = unhex(PUBLIC_KEY_HEX).and_then(|k| k.try_into().ok()).ok_or("bad public key")?;
    let key = VerifyingKey::from_bytes(&key).map_err(|e| e.to_string())?;
    let sig: [u8; 64] = unhex(sig_text).and_then(|s| s.try_into().ok()).ok_or("bad_signature")?;
    key.verify(bytes, &Signature::from_bytes(&sig)).map_err(|_| "bad_signature".to_string())?;
    let m: Value = serde_json::from_slice(bytes).map_err(|e| format!("manifest_invalid: {}", e))?;
    if m.get("format").and_then(|f| f.as_u64()) != Some(1) {
        return Err("manifest_format".into());
    }
    let ver = |k: &str| {
        m.get(k).and_then(|v| v.as_str()).and_then(|v| semver::Version::parse(v).ok()).ok_or(format!("manifest_invalid: {}", k))
    };
    let files = m.get("files").cloned().unwrap_or(Value::Null);
    let list = files.as_array().ok_or("manifest_invalid: files")?;
    for f in list {
        let p = f.get("path").and_then(|p| p.as_str()).unwrap_or_default();
        let sha = f.get("sha256").and_then(|p| p.as_str()).unwrap_or_default();
        if !prepare::safe_rel(p) || p.eq_ignore_ascii_case("kit.json") || sha.len() != 64 || f.get("size").and_then(|s| s.as_u64()).is_none() {
            return Err(format!("manifest_invalid: file {}", p));
        }
    }
    let launcher = match m.get("launcher") {
        Some(l) => {
            let v = l.get("version").and_then(|v| v.as_str()).and_then(|v| semver::Version::parse(v).ok()).ok_or("manifest_invalid: launcher")?;
            Some((v, asset(l, MAX_SETUP)?))
        }
        None => None,
    };
    Ok(Release { version: ver("version")?, min_launcher: ver("min_launcher")?, kit: asset(m.get("kit").unwrap_or(&Value::Null), MAX_KIT)?, launcher, files })
}

/// Where the release's files are: a GitHub release's download folder (https, ending in "/") or a local folder.
#[derive(Clone)]
pub enum Source {
    Http(String),
    Dir(PathBuf),
}

#[derive(Clone, Serialize)]
pub struct UpdateProgress {
    /// download | unpack | swap | build | done
    pub phase: &'static str,
    /// the build's stage (prepare.rs STAGES) while phase is "build"
    pub stage: Option<&'static str>,
    pub percent: f32,
    pub detail: String,
}

fn sha256_file(p: &Path) -> Result<String, String> {
    let mut f = File::open(p).map_err(|e| format!("{}: {}", p.display(), e))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(hex(&h.finalize()))
}

fn matches(p: &Path, a: &Asset) -> Result<bool, String> {
    Ok(fs::metadata(p).map(|m| m.len()).unwrap_or(0) == a.size && sha256_file(p)? == a.sha256)
}

/// One asset into `dir` (kept there if it is already complete), checked against its size and SHA-256.
pub fn fetch(src: &Source, a: &Asset, dir: &Path, cancel: &AtomicBool, progress: &dyn Fn(u64, f64)) -> Result<PathBuf, String> {
    fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    let dest = dir.join(&a.file);
    if matches(&dest, a)? {
        return Ok(dest);
    }
    let part = dir.join(format!("{}.part", a.file));
    let mut out = File::create(&part).map_err(|e| e.to_string())?;
    let mut reader: Box<dyn Read> = match src {
        Source::Dir(d) => Box::new(File::open(d.join(&a.file)).map_err(|e| format!("download_failed: {}: {}", a.file, e))?),
        Source::Http(base) => {
            let client = reqwest::blocking::Client::builder()
                .user_agent("LCS-Recomp-Launcher")
                .https_only(true)
                .timeout(None)
                .build()
                .map_err(|e| e.to_string())?;
            let resp = client.get(format!("{}{}", base, a.file)).send().map_err(|e| format!("download_failed: {}: {}", a.file, e))?;
            if !resp.status().is_success() {
                return Err(format!("download_failed: {}: HTTP {}", a.file, resp.status().as_u16()));
            }
            Box::new(resp)
        }
    };
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    let mut done = 0u64;
    let started = Instant::now();
    loop {
        if cancel.load(Ordering::Relaxed) {
            drop(out);
            let _ = fs::remove_file(&part);
            return Err("cancelled".into());
        }
        let n = reader.read(&mut buf).map_err(|e| format!("download_failed: {}: {}", a.file, e))?;
        if n == 0 {
            break;
        }
        done += n as u64;
        if done > a.size {
            drop(out);
            let _ = fs::remove_file(&part);
            return Err(format!("checksum_failed: {} is larger than the manifest says", a.file));
        }
        h.update(&buf[..n]);
        out.write_all(&buf[..n]).map_err(|e| e.to_string())?;
        progress(done, done as f64 / 1e6 / started.elapsed().as_secs_f64().max(0.1));
    }
    drop(out);
    if done != a.size || hex(&h.finalize()) != a.sha256 {
        let _ = fs::remove_file(&part);
        return Err(format!("checksum_failed: {}", a.file));
    }
    let _ = fs::remove_file(&dest);
    fs::rename(&part, &dest).map_err(|e| e.to_string())?;
    Ok(dest)
}

/// Next to the kit while an update is between "kit -> kit.old" and its successful build.

/// An update interrupted (the launcher closed, the PC went off): while <kit>.pending is there the new kit never got
/// its build, so the previous kit goes back - the installed game is still the previous version. Without the marker
/// the update was done and only its clean-up was cut short: kit.old goes.
pub fn recover_kit(app_dir: &Path) {
    if UPDATING.load(Ordering::Relaxed) {
        return;
    }
    let k = prepare::KIT_DIR;
    let (kit, old, new, pending) =
        (app_dir.join(k), app_dir.join(format!("{k}.old")), app_dir.join(format!("{k}.new")), app_dir.join(format!("{k}.pending")));
    if old.join("kit.json").is_file() {
        if pending.is_file() || !kit.join("kit.json").is_file() {
            let _ = fs::remove_dir_all(&kit);
            if fs::rename(&old, &kit).is_ok() {
                let _ = fs::remove_file(&pending);
            }
        } else {
            let _ = fs::remove_dir_all(&old);
        }
    } else if pending.is_file() && kit.join("kit.json").is_file() {
        let _ = fs::remove_file(&pending);
    }
    if new.exists() {
        let _ = fs::remove_dir_all(&new);
    }
}

struct Updating;
impl Updating {
    fn start() -> Result<Updating, String> {
        if UPDATING.swap(true, Ordering::SeqCst) {
            return Err("already_updating".into());
        }
        Ok(Updating)
    }
}
impl Drop for Updating {
    fn drop(&mut self) {
        UPDATING.store(false, Ordering::SeqCst);
    }
}

/// The unpacked kit must be exactly the manifest's list (plus kit.json, which must carry the same list and version).
fn check_unpacked(dir: &Path, rel: &Release) -> Result<(), String> {
    let kit_json: Value = serde_json::from_str(&fs::read_to_string(dir.join("kit.json")).map_err(|_| "kit_invalid: no kit.json")?)
        .map_err(|e| format!("kit_invalid: kit.json: {}", e))?;
    if kit_json.get("version").and_then(|v| v.as_str()) != Some(rel.version.to_string().as_str()) || kit_json.get("files") != Some(&rel.files) {
        return Err("kit_invalid: kit.json differs from the manifest".into());
    }
    let mut listed = std::collections::HashMap::new();
    for f in rel.files.as_array().unwrap() {
        listed.insert(f["path"].as_str().unwrap().to_string(), f);
    }
    fn walk(d: &Path, base: &Path, out: &mut Vec<(String, PathBuf)>) -> Result<(), String> {
        for e in fs::read_dir(d).map_err(|e| e.to_string())? {
            let e = e.map_err(|e| e.to_string())?;
            if e.file_type().map_err(|e| e.to_string())?.is_dir() {
                walk(&e.path(), base, out)?;
            } else {
                out.push((e.path().strip_prefix(base).unwrap().to_string_lossy().replace('\\', "/"), e.path()));
            }
        }
        Ok(())
    }
    let mut found = Vec::new();
    walk(dir, dir, &mut found)?;
    let mut seen = 0usize;
    for (rel_path, p) in found {
        if rel_path == "kit.json" {
            continue;
        }
        let f = listed.get(&rel_path).ok_or(format!("kit_invalid: {} is not in the manifest", rel_path))?;
        let size = fs::metadata(&p).map(|m| m.len()).unwrap_or(u64::MAX);
        if Some(size) != f["size"].as_u64() || Some(sha256_file(&p)?.as_str()) != f["sha256"].as_str() {
            return Err(format!("kit_invalid: {} differs from the manifest", rel_path));
        }
        seen += 1;
    }
    if seen != listed.len() {
        return Err(format!("kit_invalid: {} of {} files", seen, listed.len()));
    }
    Ok(())
}

/// Download, check, swap the kit, rebuild. `kit` is the kit in use (its folder: prepare::KIT_DIR, "Source Code").
pub fn apply(
    kit: &Path,
    game: &Path,
    rel: &Release,
    src: &Source,
    progress: &(dyn Fn(UpdateProgress) + Sync),
    cancel: Arc<AtomicBool>,
) -> Result<(), String> {
    let guard = Updating::start()?;
    let iso = game.join("gamedata").join("LCS.iso");
    if !iso.is_file() {
        return Err("iso_missing".into());
    }
    let app_dir = kit.parent().ok_or("kit_missing")?.to_path_buf();
    let report = |phase, percent: f32, detail: String| progress(UpdateProgress { phase, stage: None, percent, detail });
    let build_cb = |p: prepare::Progress| {
        progress(UpdateProgress { phase: "build", stage: Some(p.stage), percent: 20.0 + p.percent * 0.8, detail: p.detail })
    };
    let ctx = prepare::Ctx::new(kit.to_path_buf(), game.to_path_buf(), iso, &build_cb, cancel.clone());
    ctx.log(&format!("== update to {} ==", rel.version));

    // 1. the kit zip
    let staging = game.join("_update").join(rel.version.to_string());
    let total = rel.kit.size as f64 / 1e6;
    let zip = fetch(src, &rel.kit, &staging, &cancel, &|done, mbps| {
        report("download", 15.0 * done as f32 / rel.kit.size as f32, format!("{:.0} / {:.0} MB · {:.1} MB/s", done as f64 / 1e6, total, mbps))
    })?;
    ctx.log(&format!("downloaded {} ({} bytes, SHA-256 ok)", rel.kit.file, rel.kit.size));

    // 2. unpacked next to the kit, every file checked
    report("unpack", 15.0, rel.kit.file.clone());
    let new = app_dir.join(format!("{}.new", prepare::KIT_DIR));
    let _ = fs::remove_dir_all(&new);
    let unpacked = prepare::unzip(&zip, &new, &cancel).and_then(|_| {
        report("unpack", 18.0, "SHA-256".into());
        check_unpacked(&new, rel)
    });
    if let Err(e) = unpacked {
        let _ = fs::remove_dir_all(&new);
        return Err(e);
    }

    // 3. the swap
    report("swap", 20.0, String::new());
    let old = app_dir.join(format!("{}.old", prepare::KIT_DIR));
    let pending = app_dir.join(format!("{}.pending", prepare::KIT_DIR));
    let _ = fs::remove_dir_all(&old);
    fs::write(&pending, rel.version.to_string()).map_err(|e| format!("swap_failed: {}", e))?;
    if let Err(e) = fs::rename(kit, &old) {
        let _ = fs::remove_file(&pending);
        return Err(format!("swap_failed: {}", e));
    }
    if let Err(e) = fs::rename(&new, kit) {
        let _ = fs::rename(&old, kit);
        let _ = fs::remove_dir_all(&new);
        let _ = fs::remove_file(&pending);
        return Err(format!("swap_failed: {}", e));
    }
    ctx.log("kit swapped (previous kit in kit.old until the build succeeds)");

    // 4. the build (the installed game is replaced only at its end)
    let built = prepare::prepare(&ctx);
    if let Err(e) = built {
        ctx.log(&format!("== update failed: {} - the previous kit is put back ==", e));
        let _ = fs::remove_dir_all(kit);
        fs::rename(&old, kit).map_err(|e2| format!("{} (and the previous kit could not be put back: {})", e, e2))?;
        let _ = fs::remove_file(&pending);
        return Err(e);
    }
    let _ = fs::remove_file(&pending);
    let _ = fs::remove_dir_all(&old);
    let _ = fs::remove_dir_all(game.join("_update"));
    ctx.log(&format!("== updated to {} ==", rel.version));
    drop(guard);
    report("done", 100.0, String::new());
    Ok(())
}

/// The launcher's own update: the Inno Setup installer the manifest names, into the game folder's _update\launcher
/// (nothing outside the launcher's folder), checked, started; the launcher closes so the installer can replace it.
pub fn fetch_setup(game: &Path, src: &Source, a: &Asset, cancel: &AtomicBool) -> Result<PathBuf, String> {
    let dir = game.join("_update").join("launcher");
    fetch(src, a, &dir, cancel, &|_, _| {})
}

/// A release folder on disk (manifest.json, manifest.json.sig, the assets): verified like a download.
pub fn read_dir_release(dir: &Path) -> Result<Release, String> {
    let m = dir.join(MANIFEST);
    if fs::metadata(&m).map(|x| x.len()).unwrap_or(0) > MAX_MANIFEST {
        return Err("manifest_invalid: too large".into());
    }
    let bytes = fs::read(&m).map_err(|e| format!("{}: {}", m.display(), e))?;
    let sig = fs::read_to_string(dir.join(SIGNATURE)).map_err(|e| format!("{}: {}", SIGNATURE, e))?;
    verify(&bytes, &sig)
}
