//! The player's ISO: format (ISO 9660), SYSTEM.CNF's boot file, the executable on the disc, size and SHA-256 against
//! the one supported disc. Read only; nothing is copied or sent anywhere here.

use serde::Serialize;
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use tauri::{AppHandle, Emitter};
use tauri_plugin_dialog::DialogExt;

/// Grand Theft Auto: Liberty City Stories (USA), SLUS-21423 - the dump the recompiled code was made from
/// (measured 2026-11-17; MD5 2d0cd1f12841de894a9a668f7c8fd5d3, SHA-1 f690b24e4f6f7cfcb642f9faaa1e05d2ba6a3a3a).
pub const SUPPORTED_SIZE: u64 = 4_010_016_768;
pub const SUPPORTED_SHA256: &str = "9e45271f8835f6b77213c8a7ff3bd1e4307fffa9c3f4434cf8498a5a7b59c4fe";
pub const SUPPORTED_BOOT: &str = "SLUS_214.23";
const SECTOR: u64 = 2048;

#[derive(Serialize, Default)]
pub struct IsoReport {
    path: String,
    size: u64,
    size_ok: bool,
    iso9660: bool,
    boot_file: Option<String>,
    exe_found: bool,
    sha256: Option<String>,
    hash_ok: bool,
    supported: bool,
    error: Option<String>,
}

#[derive(Clone, Serialize)]
struct Progress {
    stage: &'static str,
    percent: u32,
}

struct Entry {
    name: String,
    lba: u32,
    size: u32,
}

fn read_at(f: &mut File, offset: u64, len: usize) -> std::io::Result<Vec<u8>> {
    let mut buf = vec![0u8; len];
    f.seek(SeekFrom::Start(offset))?;
    f.read_exact(&mut buf)?;
    Ok(buf)
}

fn u32le(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

/// The root directory's entries (ISO 9660: the primary volume descriptor at sector 16, its root record at +156).
fn root_entries(f: &mut File) -> Result<Vec<Entry>, String> {
    let pvd = read_at(f, 16 * SECTOR, SECTOR as usize).map_err(|e| e.to_string())?;
    if pvd[0] != 1 || &pvd[1..6] != b"CD001" {
        return Err("not_iso9660".into());
    }
    let root = &pvd[156..156 + 34];
    let (lba, len) = (u32le(root, 2), u32le(root, 10).min(256 * 1024));
    let dir = read_at(f, lba as u64 * SECTOR, len as usize).map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < dir.len() {
        let rec = dir[i] as usize;
        if rec == 0 {
            // records do not cross sectors: on to the next one
            i = (i / SECTOR as usize + 1) * SECTOR as usize;
            continue;
        }
        if i + rec > dir.len() || rec < 34 {
            break;
        }
        let name_len = dir[i + 32] as usize;
        if 33 + name_len <= rec {
            let raw = &dir[i + 33..i + 33 + name_len];
            let name = String::from_utf8_lossy(raw).to_string();
            let name = name.split(';').next().unwrap_or("").to_string();
            out.push(Entry { name, lba: u32le(&dir[i..], 2), size: u32le(&dir[i..], 10) });
        }
        i += rec;
    }
    Ok(out)
}

/// `BOOT2 = cdrom0:\SLUS_214.23;1` -> `SLUS_214.23`
fn boot_file(cnf: &str) -> Option<String> {
    let line = cnf.lines().find(|l| l.trim_start().to_ascii_uppercase().starts_with("BOOT2"))?;
    let value = line.split('=').nth(1)?.trim();
    let file = value.rsplit(['\\', '/', ':']).next()?;
    Some(file.split(';').next()?.trim().to_string())
}

pub fn check_with(path: &str, progress: &dyn Fn(&'static str, u32)) -> IsoReport {
    let mut r = IsoReport { path: path.to_string(), ..Default::default() };
    let mut f = match File::open(path) {
        Ok(f) => f,
        Err(e) => {
            r.error = Some(format!("open_failed: {}", e));
            return r;
        }
    };
    r.size = f.metadata().map(|m| m.len()).unwrap_or(0);
    r.size_ok = r.size == SUPPORTED_SIZE;
    progress("format", 0);
    match root_entries(&mut f) {
        Ok(entries) => {
            r.iso9660 = true;
            if let Some(cnf) = entries.iter().find(|e| e.name.eq_ignore_ascii_case("SYSTEM.CNF")) {
                if let Ok(b) = read_at(&mut f, cnf.lba as u64 * SECTOR, cnf.size.min(4096) as usize) {
                    r.boot_file = boot_file(&String::from_utf8_lossy(&b));
                }
            }
            if let Some(boot) = &r.boot_file {
                r.exe_found = entries.iter().any(|e| e.name.eq_ignore_ascii_case(boot));
            }
        }
        Err(e) => {
            r.error = Some(e);
            return r;
        }
    }
    if r.boot_file.as_deref() != Some(SUPPORTED_BOOT) {
        r.error = Some("unsupported_version".into());
        return r;
    }
    // the hash only for a disc that can be the supported one (4 GB take a while)
    if !r.size_ok {
        r.error = Some("unsupported_dump".into());
        return r;
    }
    let mut hasher = Sha256::new();
    let mut buf = vec![0u8; 8 << 20];
    let mut done: u64 = 0;
    let mut last = u32::MAX;
    if f.seek(SeekFrom::Start(0)).is_err() {
        r.error = Some("read_failed".into());
        return r;
    }
    loop {
        let n = match f.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) => {
                r.error = Some(format!("read_failed: {}", e));
                return r;
            }
        };
        hasher.update(&buf[..n]);
        done += n as u64;
        let pct = (done * 100 / r.size.max(1)) as u32;
        if pct != last {
            last = pct;
            progress("hash", pct);
        }
    }
    let hex: String = hasher.finalize().iter().map(|b| format!("{:02x}", b)).collect();
    r.hash_ok = hex == SUPPORTED_SHA256;
    r.sha256 = Some(hex);
    r.supported = r.iso9660 && r.exe_found && r.size_ok && r.hash_ok;
    if !r.hash_ok {
        r.error = Some("hash_mismatch".into());
    }
    r
}

fn check(app: &AppHandle, path: &str) -> IsoReport {
    check_with(path, &|stage, percent| {
        let _ = app.emit("iso-progress", Progress { stage, percent });
    })
}

/// The file `name` in the ISO's root directory: (byte offset, size).
pub fn find_root_file(path: &str, name: &str) -> Result<(u64, u32), String> {
    let mut f = File::open(path).map_err(|e| e.to_string())?;
    let entries = root_entries(&mut f)?;
    let e = entries.iter().find(|e| e.name.eq_ignore_ascii_case(name)).ok_or(format!("{} not on the disc", name))?;
    Ok((e.lba as u64 * SECTOR, e.size))
}

#[tauri::command]
pub async fn pick_iso(app: AppHandle) -> Option<String> {
    app.dialog()
        .file()
        .add_filter("PS2 disc image", &["iso"])
        .blocking_pick_file()
        .and_then(|p| p.into_path().ok())
        .map(|p| p.display().to_string())
}

#[tauri::command]
pub async fn validate_iso(app: AppHandle, path: String) -> IsoReport {
    let a = app.clone();
    tauri::async_runtime::spawn_blocking(move || check(&a, &path)).await.unwrap_or_else(|e| IsoReport {
        error: Some(e.to_string()),
        ..Default::default()
    })
}

#[cfg(test)]
mod tests {
    use super::boot_file;
    #[test]
    fn boot_line() {
        assert_eq!(boot_file("BOOT2 = cdrom0:\\SLUS_214.23;1\nVER = 1.00\n").as_deref(), Some("SLUS_214.23"));
    }
}

impl IsoReport {
    pub fn supported(&self) -> bool {
        self.supported
    }
    pub fn error_text(&self) -> String {
        self.error.clone().unwrap_or_default()
    }
}
