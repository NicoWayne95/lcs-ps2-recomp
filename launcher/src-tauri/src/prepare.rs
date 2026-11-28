//! Game preparation: the player's ISO -> a playable LCSrecomp.exe, on this PC, with the existing pipeline
//! (lcs-recomp-runtime docs/REPRODUCIBLE_BUILD_2026-11-17.md): check the ISO, put it in place, fetch the build tools (pinned
//! SHA-256), take the ELF out of the ISO, build the recompiler (ps2xRecomp at d8648c3, from the kit), recompile, the
//! post-processing (tools/lcs_postprocess.py), the VU1 microcode (tools/vu1_recomp.py), CMake + Ninja with llvm-mingw,
//! then LCSrecomp.exe and its DLLs into the game folder. Nothing is sent anywhere; the game's code is made here.
//!
//! Folders: <game>\gamedata\LCS.iso (the ISO), <game>\_build\toolchain (the tools, kept), <game>\_build\work (the
//! sources, generated code and build tree, kept so a later update rebuilds only what changed), <game>\logs\build.log.

use serde::Serialize;
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use crate::iso;

#[derive(Clone, serde::Deserialize)]
pub struct Tool {
    pub name: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
    /// the folder under toolchain\ it ends up in; `inner` true: the zip has that folder at its top
    pub folder: String,
    pub inner: bool,
}

struct ToolPin {
    name: &'static str,
    url: &'static str,
    sha256: &'static str,
    size: u64,
    folder: &'static str,
    inner: bool,
}

/// The build tools a kit without config\tools.json gets (official downloads, hashed 2026-11-17). A kit's own
/// tools.json (make_kit.py) wins, so a kit update can move the pins without a new launcher.
const BUILTIN_TOOLS: [ToolPin; 4] = [
    ToolPin {
        name: "llvm-mingw 20261020",
        url: "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip",
        sha256: "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd",
        size: 190_612_575,
        folder: "llvm-mingw-20261020-ucrt-x86_64",
        inner: true,
    },
    ToolPin {
        name: "CMake 4.4.5",
        url: "https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-windows-x86_64.zip",
        sha256: "326c826486d46a23cf9d5a1c9ee3a567d427bdd02c4a34071937a2a675cff004",
        size: 54_412_232,
        folder: "cmake-4.4.5-windows-x86_64",
        inner: true,
    },
    ToolPin {
        name: "Ninja 1.13.2",
        url: "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
        sha256: "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65",
        size: 291_570,
        folder: "ninja-1.13.2",
        inner: false,
    },
    ToolPin {
        name: "Python 3.14.7 (embeddable)",
        url: "https://www.python.org/ftp/python/3.14.7/python-3.14.7-embed-amd64.zip",
        sha256: "d297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15",
        size: 12_673_227,
        folder: "python-3.14.7",
        inner: false,
    },
];

/// The kit's build tools (config\tools.json), else the built-in pins. Four, in order: llvm-mingw, CMake, Ninja,
/// Python; only https downloads with a SHA-256 and a plain folder name are taken.
pub fn tools_of(kit: &Path) -> Result<Vec<Tool>, String> {
    let file = kit.join("config").join("tools.json");
    if !file.is_file() {
        return Ok(BUILTIN_TOOLS
            .iter()
            .map(|t| Tool { name: t.name.into(), url: t.url.into(), sha256: t.sha256.into(), size: t.size, folder: t.folder.into(), inner: t.inner })
            .collect());
    }
    #[derive(serde::Deserialize)]
    struct ToolsFile {
        tools: Vec<Tool>,
    }
    let f: ToolsFile = serde_json::from_str(&fs::read_to_string(&file).map_err(|e| e.to_string())?).map_err(|e| format!("tools.json: {}", e))?;
    if f.tools.len() != 4 {
        return Err("tools.json: four tools expected".into());
    }
    for t in &f.tools {
        let folder_ok = !t.folder.is_empty() && t.folder.chars().all(|c| c.is_ascii_alphanumeric() || "._-".contains(c));
        if !t.url.starts_with("https://") || t.sha256.len() != 64 || !folder_ok || t.size == 0 {
            return Err(format!("tools.json: {} not allowed", t.name));
        }
    }
    Ok(f.tools)
}

pub const STAGES: [&str; 10] = [
    "check_iso", "place_iso", "tools", "elf", "recompiler", "recompile", "postprocess", "configure", "build", "install",
];
/// each stage's share of the bar (%), the build being most of the time
const WEIGHTS: [f32; 10] = [6.0, 4.0, 12.0, 0.5, 4.0, 1.5, 3.0, 2.0, 65.0, 2.0];

#[derive(Clone, Serialize)]
pub struct Progress {
    pub stage: &'static str,
    pub stage_index: usize,
    pub stage_count: usize,
    /// the whole preparation, 0..100
    pub percent: f32,
    /// what is going on in the stage (a file, a count) - plain text for the UI
    pub detail: String,
}

pub struct Ctx<'a> {
    pub kit: PathBuf,
    pub game: PathBuf,
    pub iso_path: PathBuf,
    pub progress: &'a (dyn Fn(Progress) + Sync),
    pub cancel: Arc<AtomicBool>,
    tools: Mutex<Vec<Tool>>,
    log: Mutex<Option<File>>,
    stage: Mutex<usize>,
}

impl<'a> Ctx<'a> {
    pub fn new(kit: PathBuf, game: PathBuf, iso_path: PathBuf, progress: &'a (dyn Fn(Progress) + Sync), cancel: Arc<AtomicBool>) -> Self {
        let _ = fs::create_dir_all(game.join("logs"));
        let log = File::create(game.join("logs").join("build.log")).ok();
        Ctx { kit, game, iso_path, progress, cancel, tools: Mutex::new(Vec::new()), log: Mutex::new(log), stage: Mutex::new(0) }
    }
    fn work(&self) -> PathBuf {
        self.game.join("_build").join("work")
    }
    fn tool(&self, i: usize) -> Tool {
        self.tools.lock().unwrap()[i].clone()
    }
    fn toolchain(&self) -> PathBuf {
        self.game.join("_build").join("toolchain")
    }
    pub fn log(&self, line: &str) {
        if let Some(f) = self.log.lock().unwrap().as_mut() {
            let _ = writeln!(f, "{}", line);
        }
    }
    fn begin(&self, i: usize) {
        *self.stage.lock().unwrap() = i;
        self.log(&format!("== {} ==", STAGES[i]));
        self.report(0.0, String::new());
    }
    /// `frac` 0..1 within the current stage
    fn report(&self, frac: f32, detail: String) {
        let i = *self.stage.lock().unwrap();
        let before: f32 = WEIGHTS[..i].iter().sum();
        let total: f32 = WEIGHTS.iter().sum();
        let percent = (before + WEIGHTS[i] * frac.clamp(0.0, 1.0)) / total * 100.0;
        (self.progress)(Progress { stage: STAGES[i], stage_index: i, stage_count: STAGES.len(), percent, detail });
    }
    fn check_cancel(&self) -> Result<(), String> {
        if self.cancel.load(Ordering::Relaxed) {
            Err("cancelled".into())
        } else {
            Ok(())
        }
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{:02x}", b)).collect()
}

// ---- tools ----

fn tool_dir(c: &Ctx, t: &Tool) -> PathBuf {
    c.toolchain().join(&t.folder)
}

fn fetch_tool(c: &Ctx, t: &Tool, index: usize, count: usize) -> Result<(), String> {
    let dest = tool_dir(c, t);
    // a tool unpacked before counts when its marker names the same download
    if fs::read_to_string(dest.join(".ok")).map(|s| s.trim() == t.sha256).unwrap_or(false) {
        return Ok(());
    }
    fs::create_dir_all(c.toolchain()).map_err(|e| e.to_string())?;
    let zip_path = c.toolchain().join(format!("{}.zip", t.folder));
    let have = fs::metadata(&zip_path).map(|m| m.len()).unwrap_or(0) == t.size && sha256_file(&zip_path)? == t.sha256;
    if !have {
        c.log(&format!("download {}", t.url));
        let client = reqwest::blocking::Client::builder()
            .user_agent("LCS-Recomp-Launcher")
            .https_only(true)
            .timeout(None)
            .build()
            .map_err(|e| e.to_string())?;
        let mut resp = client.get(&t.url).send().map_err(|e| format!("download_failed: {}: {}", t.name, e))?;
        if !resp.status().is_success() {
            return Err(format!("download_failed: {}: HTTP {}", t.name, resp.status().as_u16()));
        }
        let part = zip_path.with_extension("zip.part");
        let mut out = File::create(&part).map_err(|e| e.to_string())?;
        let mut hasher = Sha256::new();
        let mut buf = vec![0u8; 1 << 20];
        let mut done: u64 = 0;
        let started = Instant::now();
        loop {
            c.check_cancel()?;
            let n = resp.read(&mut buf).map_err(|e| format!("download_failed: {}: {}", t.name, e))?;
            if n == 0 {
                break;
            }
            hasher.update(&buf[..n]);
            out.write_all(&buf[..n]).map_err(|e| e.to_string())?;
            done += n as u64;
            let mbps = done as f64 / 1e6 / started.elapsed().as_secs_f64().max(0.1);
            c.report(
                (index as f32 + done as f32 / t.size as f32 * 0.8) / count as f32,
                format!("{} · {:.0} / {:.0} MB · {:.1} MB/s", t.name, done as f64 / 1e6, t.size as f64 / 1e6, mbps),
            );
        }
        drop(out);
        if hex(&hasher.finalize()) != t.sha256 {
            let _ = fs::remove_file(&part);
            return Err(format!("checksum_failed: {}", t.name));
        }
        fs::rename(&part, &zip_path).map_err(|e| e.to_string())?;
    }
    c.report((index as f32 + 0.85) / count as f32, format!("{} · unpacking", t.name));
    let _ = fs::remove_dir_all(&dest); // an older unpack of the same folder
    let target = if t.inner { c.toolchain() } else { dest.clone() };
    unzip(&zip_path, &target, &c.cancel)?;
    if !dest.is_dir() {
        return Err(format!("unpack_failed: {} (no {})", t.name, t.folder));
    }
    fs::write(dest.join(".ok"), &t.sha256).map_err(|e| e.to_string())?;
    let _ = fs::remove_file(&zip_path);
    Ok(())
}

fn sha256_file(p: &Path) -> Result<String, String> {
    let mut f = match File::open(p) {
        Ok(f) => f,
        Err(_) => return Ok(String::new()),
    };
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

/// Unpacks a zip; entries that would land outside `dest` (absolute paths, "..") are refused.
pub fn unzip(zip_path: &Path, dest: &Path, cancel: &AtomicBool) -> Result<(), String> {
    let f = File::open(zip_path).map_err(|e| e.to_string())?;
    let mut z = zip::ZipArchive::new(f).map_err(|e| e.to_string())?;
    for i in 0..z.len() {
        if cancel.load(Ordering::Relaxed) {
            return Err("cancelled".into());
        }
        let mut entry = z.by_index(i).map_err(|e| e.to_string())?;
        let Some(rel) = entry.enclosed_name() else {
            return Err(format!("unsafe path in {}: {}", zip_path.display(), entry.name()));
        };
        let out = dest.join(rel);
        if entry.is_dir() {
            fs::create_dir_all(&out).map_err(|e| e.to_string())?;
            continue;
        }
        if let Some(parent) = out.parent() {
            fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }
        let mut w = File::create(&out).map_err(|e| e.to_string())?;
        std::io::copy(&mut entry, &mut w).map_err(|e| e.to_string())?;
    }
    Ok(())
}

// ---- processes ----

struct Env {
    path: String,
    cmake: PathBuf,
    ninja: PathBuf,
    python: PathBuf,
    llvm: PathBuf,
}

fn env_of(c: &Ctx) -> Env {
    let llvm = tool_dir(c, &c.tool(0)).join("bin");
    let cmake = tool_dir(c, &c.tool(1)).join("bin").join("cmake.exe");
    let ninja = tool_dir(c, &c.tool(2)).join("ninja.exe");
    let python = tool_dir(c, &c.tool(3)).join("python.exe");
    let sys_path = std::env::var("PATH").unwrap_or_default();
    let path = format!(
        "{};{};{};{};{}",
        llvm.display(),
        cmake.parent().unwrap().display(),
        ninja.parent().unwrap().display(),
        python.parent().unwrap().display(),
        sys_path
    );
    Env { path, cmake, ninja, python, llvm }
}

/// Runs a tool, its output into build.log; `on_line` sees each stdout line (Ninja's "[n/m]").
fn run(c: &Ctx, env: &Env, exe: &Path, args: &[String], cwd: &Path, on_line: &mut dyn FnMut(&str)) -> Result<(), String> {
    c.log(&format!("> {} {}", exe.display(), args.join(" ")));
    let mut cmd = Command::new(exe);
    cmd.args(args).current_dir(cwd).env("PATH", &env.path).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // no console window
    }
    let mut child = cmd.spawn().map_err(|e| format!("start_failed: {}: {}", exe.display(), e))?;
    let stderr = child.stderr.take().unwrap();
    let errs: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
    let errs2 = errs.clone();
    let t = std::thread::spawn(move || {
        for line in BufReader::new(stderr).lines().map_while(Result::ok) {
            errs2.lock().unwrap().push(line);
        }
    });
    let mut failures: Vec<String> = Vec::new();
    for line in BufReader::new(child.stdout.take().unwrap()).lines().map_while(Result::ok) {
        c.log(&line);
        // Ninja and the compiler report errors on stdout: keep the first few for the message
        if failures.len() < 4 && (line.contains("error:") || line.starts_with("FAILED") || line.contains("CMake Error")) {
            failures.push(line.chars().take(240).collect());
        }
        on_line(&line);
        if c.cancel.load(Ordering::Relaxed) {
            let _ = child.kill();
        }
    }
    let status = child.wait().map_err(|e| e.to_string())?;
    let _ = t.join();
    let errs = errs.lock().unwrap();
    for l in errs.iter() {
        c.log(l);
    }
    c.check_cancel()?;
    if !status.success() {
        let mut why = failures;
        why.extend(errs.iter().rev().take(4).rev().cloned());
        return Err(format!("{} failed ({:?}): {}", exe.file_name().unwrap().to_string_lossy(), status.code(), why.join(" | ")));
    }
    Ok(())
}

fn ninja_progress(line: &str) -> Option<(u32, u32)> {
    let rest = line.strip_prefix('[')?;
    let (n, rest) = rest.split_once('/')?;
    let (m, _) = rest.split_once(']')?;
    Some((n.trim().parse().ok()?, m.trim().parse().ok()?))
}

fn copy_dir(src: &Path, dst: &Path) -> Result<(), String> {
    fs::create_dir_all(dst).map_err(|e| e.to_string())?;
    for e in fs::read_dir(src).map_err(|e| e.to_string())? {
        let e = e.map_err(|e| e.to_string())?;
        let to = dst.join(e.file_name());
        if e.file_type().map_err(|e| e.to_string())?.is_dir() {
            copy_dir(&e.path(), &to)?;
        } else {
            let same = fs::metadata(&to).ok().zip(e.metadata().ok()).map(|(a, b)| a.len() == b.len() && a.modified().ok() >= b.modified().ok()).unwrap_or(false);
            if !same {
                fs::copy(e.path(), &to).map_err(|er| format!("{}: {}", e.path().display(), er))?;
            }
        }
    }
    Ok(())
}

fn s(p: &Path) -> String {
    p.display().to_string().replace('\\', "/")
}

// ---- the stages ----

pub fn prepare(c: &Ctx) -> Result<(), String> {
    if !c.kit.join("kit.json").is_file() {
        return Err(format!("kit_missing: {}", c.kit.display()));
    }
    let work = c.work();
    fs::create_dir_all(&work).map_err(|e| e.to_string())?;
    let tools = tools_of(&c.kit)?;
    let tools_there = fs::read_to_string(tool_dir(c, &tools[0]).join(".ok")).map(|s| s.trim() == tools[0].sha256).unwrap_or(false);
    *c.tools.lock().unwrap() = tools;

    // free space first: the tools (~1.2 GB with their zips), the build (~1.2 GB), and the ISO when it can't be a hard
    // link (another drive) - with a margin; a folder that has the tools already needs less
    let same_drive = drive_of(&c.iso_path) == drive_of(&c.game);
    let need: u64 = ((if tools_there { 1_500 } else { 3_000 }) + if same_drive { 0 } else { 4_100 }) * 1_000_000;
    if let Some(free) = free_bytes(&c.game) {
        c.log(&format!("free space {:.1} GB, need {:.1} GB", free as f64 / 1e9, need as f64 / 1e9));
        if free < need {
            return Err(format!("disk_space: {:.1} GB free, {:.1} GB needed", free as f64 / 1e9, need as f64 / 1e9));
        }
    }

    // 1. the ISO: the full SHA-256 once; later runs (an update, a rebuild) trust the marker while the game folder's ISO
    // still has the size and time it was checked with
    c.begin(0);
    let target = c.game.join("gamedata").join("LCS.iso");
    let same_file = fs::canonicalize(&target).ok() == fs::canonicalize(&c.iso_path).ok();
    if same_file && iso_marker_ok(&target) {
        c.log("ISO checked before (LCS.iso.ok matches)");
        c.report(1.0, "ok".into());
    } else {
        let iso_str = c.iso_path.display().to_string();
        let r = iso::check_with(&iso_str, &|_, pct| c.report(pct as f32 / 100.0, format!("SHA-256 {}%", pct)));
        if !r.supported() {
            return Err(format!("iso_unsupported: {}", r.error_text()));
        }
    }
    c.check_cancel()?;

    // 2. the ISO into the game folder (a hard link on the same drive, else a copy)
    c.begin(1);
    fs::create_dir_all(target.parent().unwrap()).map_err(|e| e.to_string())?;
    if !same_file {
        let ok_size = fs::metadata(&target).map(|m| m.len()).unwrap_or(0) == iso::SUPPORTED_SIZE;
        if !(ok_size && sha256_file(&target)? == iso::SUPPORTED_SHA256) {
            let _ = fs::remove_file(&target);
            if fs::hard_link(&c.iso_path, &target).is_err() {
                copy_with_progress(c, &c.iso_path, &target)?;
            }
        }
    }
    write_iso_marker(&target);

    // 3. the tools
    c.begin(2);
    let n = c.tools.lock().unwrap().len();
    for i in 0..n {
        c.check_cancel()?;
        fetch_tool(c, &c.tool(i), i, n)?;
    }
    let env = env_of(c);

    // 4. the ELF out of the ISO
    c.begin(3);
    let (off, size) = iso::find_root_file(&target.display().to_string(), iso::SUPPORTED_BOOT)?;
    let mut f = File::open(&target).map_err(|e| e.to_string())?;
    f.seek(SeekFrom::Start(off)).map_err(|e| e.to_string())?;
    let mut elf = vec![0u8; size as usize];
    f.read_exact(&mut elf).map_err(|e| e.to_string())?;
    let elf_path = work.join("elf").join(iso::SUPPORTED_BOOT);
    fs::create_dir_all(elf_path.parent().unwrap()).map_err(|e| e.to_string())?;
    fs::write(&elf_path, &elf).map_err(|e| e.to_string())?;

    // 5. the recompiler
    c.begin(4);
    let rec_build = work.join("recompiler-build");
    let deps = c.kit.join("source").join("deps");
    let mut args = vec![
        "-S".into(), s(&c.kit.join("recompiler")), "-B".into(), s(&rec_build), "-G".into(), "Ninja".into(),
        "-DCMAKE_BUILD_TYPE=Release".into(),
        format!("-DCMAKE_C_COMPILER={}", s(&env.llvm.join("clang.exe"))),
        format!("-DCMAKE_CXX_COMPILER={}", s(&env.llvm.join("clang++.exe"))),
        format!("-DCMAKE_MAKE_PROGRAM={}", s(&env.ninja)),
        "-DPS2X_BUILD_RUNTIME=OFF".into(), "-DPS2X_BUILD_ANALYZER=OFF".into(), "-DPS2X_BUILD_TEST=OFF".into(),
        "-DPS2X_BUILD_STUDIO=OFF".into(), "-DFETCHCONTENT_FULLY_DISCONNECTED=ON".into(),
    ];
    for (k, d) in [("ELFIO", "elfio"), ("FMT", "fmt"), ("TOML11", "toml11"), ("RABBITIZER", "rabbitizer"), ("LIBDWARF", "libdwarf")] {
        args.push(format!("-DFETCHCONTENT_SOURCE_DIR_{}={}", k, s(&deps.join(d))));
    }
    run(c, &env, &env.cmake, &args, &work, &mut |_| {})?;
    run(c, &env, &env.cmake, &["--build".into(), s(&rec_build), "--target".into(), "ps2_recomp".into()], &work, &mut |l| {
        if let Some((n, m)) = ninja_progress(l) {
            c.report(0.2 + 0.8 * n as f32 / m.max(1) as f32, format!("{} / {}", n, m));
        }
    })?;
    let recomp = rec_build.join("ps2xRecomp").join("ps2_recomp.exe");

    // 6. recompile
    c.begin(5);
    let out = work.join("recomp_out");
    let _ = fs::remove_dir_all(&out);
    fs::create_dir_all(&out).map_err(|e| e.to_string())?;
    let cfg = fs::read_to_string(c.kit.join("config").join("lcs_guest.toml")).map_err(|e| e.to_string())?;
    let cfg = cfg.replace("@ELF@", &s(&elf_path)).replace("@OUT@", &(s(&out) + "/"));
    let cfg_path = work.join("lcs_guest.toml");
    fs::write(&cfg_path, cfg).map_err(|e| e.to_string())?;
    let mut count = 0u32;
    run(c, &env, &recomp, &[s(&cfg_path)], &work, &mut |l| {
        if l.contains("processed") {
            count += 1;
            c.report((count as f32 / 60.0).min(1.0), l.trim().to_string());
        }
    })?;

    // 7. the sources, the generated code (post-processed), the VU1 microcode
    c.begin(6);
    let source = work.join("source");
    sync_source(c, &source)?;
    c.report(0.4, "lcs_postprocess".into());
    let ee = source.join("generated").join("ee");
    run(c, &env, &env.python, &[s(&source.join("tools").join("lcs_postprocess.py")), s(&out), s(&ee)], &work, &mut |_| {})?;
    c.report(0.8, "vu1_recomp".into());
    let vu1 = source.join("generated").join("vu1");
    fs::create_dir_all(&vu1).map_err(|e| e.to_string())?;
    run(c, &env, &env.python, &[s(&source.join("tools").join("vu1_recomp.py")), s(&elf_path), s(&vu1.join("vu1_compiled.cpp")), s(&c.kit.join("config").join("vu1_uploads.txt"))], &work, &mut |_| {})?;

    // 8. configure (the release build: LCS_DIAGNOSTICS OFF, the pause-menu work and keyboard / mouse ON)
    c.begin(7);
    let build = work.join("build");
    let game_out = work.join("out");
    let sdeps = source.join("deps");
    let mut args = vec![
        "-S".into(), s(&source), "-B".into(), s(&build), "-G".into(), "Ninja".into(),
        "-DCMAKE_BUILD_TYPE=Release".into(), "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF".into(), "-DPS2X_LTO=OFF".into(),
        "-DLCS_BUILD_TOOLS=OFF".into(), "-DLCS_DIAGNOSTICS=OFF".into(), "-DLCS_MENU_VIDEO=ON".into(),
        "-DLCS_RUNTIME_DEBUG_INFO=ON".into(), // the crash handler needs dbghelp (linked with the debug info)
        format!("-DLCS_GENERATED_DIR={}", s(&ee)),
        format!("-DLCS_GAME_DIR={}", s(&game_out)),
        format!("-DCMAKE_C_COMPILER={}", s(&env.llvm.join("clang.exe"))),
        format!("-DCMAKE_CXX_COMPILER={}", s(&env.llvm.join("clang++.exe"))),
        format!("-DCMAKE_RC_COMPILER={}", s(&env.llvm.join("llvm-windres.exe"))),
        "-DCMAKE_EXE_LINKER_FLAGS=-static".into(),
        format!("-DCMAKE_MAKE_PROGRAM={}", s(&env.ninja)),
        format!("-DPython3_EXECUTABLE={}", s(&env.python)),
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON".into(),
        format!("-DFFMPEG_PREBUILT_URL={}", s(&sdeps.join("ffmpeg-n7.1-241205-lgpl-amd64-shared.zip"))),
    ];
    for (k, d) in [("ELFIO", "elfio"), ("FMT", "fmt"), ("IMGUI", "imgui"), ("LIBDWARF", "libdwarf"), ("NLOHMANN_JSON", "nlohmann_json"),
        ("RABBITIZER", "rabbitizer"), ("SDL3", "SDL3"), ("TOML11", "toml11"), ("ZLIB", "zlib")] {
        args.push(format!("-DFETCHCONTENT_SOURCE_DIR_{}={}", k, s(&sdeps.join(d))));
    }
    run(c, &env, &env.cmake, &args, &work, &mut |_| {})?;

    // 9. build
    c.begin(8);
    run(c, &env, &env.cmake, &["--build".into(), s(&build)], &work, &mut |l| {
        if let Some((n, m)) = ninja_progress(l) {
            c.report(n as f32 / m.max(1) as f32, format!("{} / {}", n, m));
        }
    })?;

    // 10. install: the runtime and its DLLs into the game folder; saves and settings are never touched
    c.begin(9);
    let exe = game_out.join(crate::config::RUNTIME_EXE);
    if !exe.is_file() {
        return Err("build_no_exe".into());
    }
    backup_installed(c)?;
    for p in runtime_files(&game_out)? {
        let to = c.game.join(p.file_name().unwrap());
        let tmp = to.with_extension("new");
        fs::copy(&p, &tmp).map_err(|e| e.to_string())?;
        fs::rename(&tmp, &to).map_err(|e| format!("{}: {}", to.display(), e))?;
    }
    crate::library::migrate_default(&c.game);
    fs::create_dir_all(c.game.join(crate::library::DEFAULT_SAVE_DIR)).map_err(|e| e.to_string())?;
    let kit: serde_json::Value = serde_json::from_str(&fs::read_to_string(c.kit.join("kit.json")).unwrap_or_default()).unwrap_or_default();
    let mut info = serde_json::json!({ "build": "local", "kit_files": kit.get("file_count").or_else(|| kit.get("files")) });
    if let Some(v) = kit.get("version") {
        info["version"] = v.clone();
    }
    fs::write(c.game.join("build.json"), serde_json::to_string_pretty(&info).unwrap()).map_err(|e| e.to_string())?;
    if let Some(v) = kit.get("version").and_then(|v| v.as_str()) {
        fs::write(c.game.join("version.json"), serde_json::json!({ "version": v }).to_string()).map_err(|e| e.to_string())?;
    }
    c.report(1.0, "done".into());
    c.log("== done ==");
    Ok(())
}

// ---- the ISO marker, the build tree's sources, the previous version ----

fn iso_marker(p: &Path) -> Option<String> {
    let m = fs::metadata(p).ok()?;
    let t = m.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?.as_nanos();
    Some(format!("{} {} {}", m.len(), t, iso::SUPPORTED_SHA256))
}

fn iso_marker_ok(p: &Path) -> bool {
    let have = fs::read_to_string(p.with_extension("iso.ok")).ok();
    have.is_some() && have == iso_marker(p) && fs::metadata(p).map(|m| m.len()).unwrap_or(0) == iso::SUPPORTED_SIZE
}

fn write_iso_marker(p: &Path) {
    if let Some(m) = iso_marker(p) {
        let _ = fs::write(p.with_extension("iso.ok"), m);
    }
}

/// The kit's source\ into the build tree by kit.json's list (size + SHA-256): changed files are copied, files the kit
/// no longer has are removed - except what the build makes there (generated\, Python's caches). Unchanged files keep
/// their time, so Ninja rebuilds only what changed. A kit without a list (made before kit.json had one): copy_dir.
fn sync_source(c: &Ctx, source: &Path) -> Result<(), String> {
    let kit: serde_json::Value = serde_json::from_str(&fs::read_to_string(c.kit.join("kit.json")).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?;
    let Some(files) = kit.get("files").and_then(|f| f.as_array()) else {
        return copy_dir(&c.kit.join("source"), source);
    };
    let mut wanted = std::collections::HashSet::new();
    let (mut copied, mut removed) = (0u32, 0u32);
    for f in files {
        let Some(rel) = f.get("path").and_then(|p| p.as_str()).and_then(|p| p.strip_prefix("source/")) else {
            continue;
        };
        if !safe_rel(rel) {
            return Err(format!("unsafe path in kit.json: {}", rel));
        }
        wanted.insert(rel.to_ascii_lowercase());
        let to = source.join(rel);
        let size = f.get("size").and_then(|v| v.as_u64());
        let same = fs::metadata(&to).ok().map(|m| m.len()) == size
            && f.get("sha256").and_then(|v| v.as_str()) == Some(sha256_file(&to)?.as_str());
        if !same {
            if let Some(parent) = to.parent() {
                fs::create_dir_all(parent).map_err(|e| e.to_string())?;
            }
            fs::copy(c.kit.join("source").join(rel), &to).map_err(|e| format!("{}: {}", rel, e))?;
            copied += 1;
        }
    }
    fn walk(dir: &Path, base: &Path, wanted: &std::collections::HashSet<String>, removed: &mut u32) -> Result<(), String> {
        for e in fs::read_dir(dir).map_err(|e| e.to_string())? {
            let e = e.map_err(|e| e.to_string())?;
            let p = e.path();
            let rel = p.strip_prefix(base).unwrap().to_string_lossy().replace('\\', "/");
            let name = e.file_name().to_string_lossy().to_string();
            if rel == "generated" || name == "__pycache__" {
                continue;
            }
            if e.file_type().map_err(|e| e.to_string())?.is_dir() {
                walk(&p, base, wanted, removed)?;
            } else if !wanted.contains(&rel.to_ascii_lowercase()) {
                fs::remove_file(&p).map_err(|e| e.to_string())?;
                *removed += 1;
            }
        }
        Ok(())
    }
    fs::create_dir_all(source).map_err(|e| e.to_string())?;
    walk(source, source, &wanted, &mut removed)?;
    c.log(&format!("sources: {} copied, {} removed, {} listed", copied, removed, wanted.len()));
    Ok(())
}

/// A relative path that stays inside its folder: no root, no drive, no "..".
pub fn safe_rel(rel: &str) -> bool {
    !rel.is_empty()
        && !rel.starts_with('/')
        && !rel.starts_with('\\')
        && !rel.contains(':')
        && rel.split(['/', '\\']).all(|part| !part.is_empty() && part != "." && part != "..")
}

/// The runtime's files in a folder: LCSrecomp.exe and its DLLs.
fn runtime_files(dir: &Path) -> Result<Vec<PathBuf>, String> {
    let mut out = Vec::new();
    for e in fs::read_dir(dir).map_err(|e| e.to_string())? {
        let p = e.map_err(|e| e.to_string())?.path();
        let ext = p.extension().map(|x| x.to_string_lossy().to_ascii_lowercase());
        if p.is_file() && matches!(ext.as_deref(), Some("exe") | Some("dll")) {
            out.push(p);
        }
    }
    Ok(out)
}

pub const BACKUP_DIR: &str = "_backup";
const VERSION_FILES: [&str; 2] = ["version.json", "build.json"];

/// Before a new build goes in: the installed one (exe, DLLs, version.json, build.json) into _backup\ - one version is
/// kept, the one before it replaced. Saves, lcs.ini and the ISO are never in it and never touched.
fn backup_installed(c: &Ctx) -> Result<(), String> {
    if !c.game.join(crate::config::RUNTIME_EXE).is_file() {
        return Ok(());
    }
    let dir = c.game.join(BACKUP_DIR);
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let mut files = runtime_files(&c.game)?;
    files.extend(VERSION_FILES.iter().map(|f| c.game.join(f)).filter(|p| p.is_file()));
    for p in files {
        fs::copy(&p, dir.join(p.file_name().unwrap())).map_err(|e| format!("backup {}: {}", p.display(), e))?;
    }
    c.log("previous version kept in _backup");
    Ok(())
}

/// The version in _backup\ (None: no backup; "" for a backup without a version file).
pub fn backup_version(game: &Path) -> Option<String> {
    let dir = game.join(BACKUP_DIR);
    if !dir.join(crate::config::RUNTIME_EXE).is_file() {
        return None;
    }
    let v: Option<serde_json::Value> = fs::read_to_string(dir.join("version.json")).ok().and_then(|t| serde_json::from_str(&t).ok());
    Some(v.and_then(|v| v.get("version")?.as_str().map(str::to_owned)).unwrap_or_default())
}

/// Swaps the installed version and the one in _backup\ (a second restore goes back again).
pub fn restore_backup(game: &Path) -> Result<(), String> {
    let backup = game.join(BACKUP_DIR);
    if !backup.join(crate::config::RUNTIME_EXE).is_file() {
        return Err("no_backup".into());
    }
    let swap = game.join("_backup.swap");
    let _ = fs::remove_dir_all(&swap);
    fs::create_dir_all(&swap).map_err(|e| e.to_string())?;
    let mut current = runtime_files(game)?;
    current.extend(VERSION_FILES.iter().map(|f| game.join(f)).filter(|p| p.is_file()));
    for p in &current {
        fs::rename(p, swap.join(p.file_name().unwrap())).map_err(|e| format!("{}: {}", p.display(), e))?;
    }
    for e in fs::read_dir(&backup).map_err(|e| e.to_string())? {
        let p = e.map_err(|e| e.to_string())?.path();
        fs::rename(&p, game.join(p.file_name().unwrap())).map_err(|e| format!("{}: {}", p.display(), e))?;
    }
    fs::remove_dir_all(&backup).map_err(|e| e.to_string())?;
    fs::rename(&swap, &backup).map_err(|e| e.to_string())
}

fn copy_with_progress(c: &Ctx, from: &Path, to: &Path) -> Result<(), String> {
    let mut src = File::open(from).map_err(|e| e.to_string())?;
    let total = src.metadata().map(|m| m.len()).unwrap_or(1).max(1);
    let tmp = to.with_extension("iso.part");
    let mut dst = File::create(&tmp).map_err(|e| e.to_string())?;
    let mut buf = vec![0u8; 8 << 20];
    let mut done = 0u64;
    loop {
        c.check_cancel()?;
        let n = src.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        dst.write_all(&buf[..n]).map_err(|e| e.to_string())?;
        done += n as u64;
        c.report(done as f32 / total as f32, format!("{:.1} / {:.1} GB", done as f64 / 1e9, total as f64 / 1e9));
    }
    drop(dst);
    fs::rename(&tmp, to).map_err(|e| e.to_string())
}

/// The drive a path is on ("C:"), to know whether the ISO can be a hard link.
fn drive_of(p: &Path) -> Option<String> {
    let abs = fs::canonicalize(p).ok().or_else(|| Some(p.to_path_buf()))?;
    let s = abs.display().to_string();
    let s = s.trim_start_matches(r"\\?\");
    s.get(..2).map(|d| d.to_ascii_uppercase())
}

/// Free bytes for the user on the drive of `p` (Windows' GetDiskFreeSpaceExW).
#[cfg(windows)]
fn free_bytes(p: &Path) -> Option<u64> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" {
        fn GetDiskFreeSpaceExW(dir: *const u16, avail: *mut u64, total: *mut u64, free: *mut u64) -> i32;
    }
    let w: Vec<u16> = p.as_os_str().encode_wide().chain(Some(0)).collect();
    let mut avail = 0u64;
    let ok = unsafe { GetDiskFreeSpaceExW(w.as_ptr(), &mut avail, std::ptr::null_mut(), std::ptr::null_mut()) };
    (ok != 0).then_some(avail)
}
#[cfg(not(windows))]
fn free_bytes(_: &Path) -> Option<u64> {
    None
}

/// The folder the launcher ships its build kit in - the game's source code (runtime, recompiler, build config; the
/// user's ask 2026-11-19: named so it reads as the source code). Its list stays kit.json (the signed release format).
/// Updates use <name>.new / <name>.old / <name>.pending next to it.
pub const KIT_DIR: &str = "Source Code";
/// The name before 0.2.0: an installation that still has it is renamed on the first start.
const LEGACY_KIT_DIR: &str = "kit";

/// An installation from before 0.2.0: kit (and an update's kit.old / kit.new / kit.pending) -> "Source Code"*.
fn migrate_legacy_kit(dir: &Path) {
    if !dir.join(LEGACY_KIT_DIR).join("kit.json").is_file() || dir.join(KIT_DIR).exists() {
        return;
    }
    for suffix in ["", ".old", ".new", ".pending"] {
        let from = dir.join(format!("{}{}", LEGACY_KIT_DIR, suffix));
        if from.exists() {
            let _ = fs::rename(&from, dir.join(format!("{}{}", KIT_DIR, suffix)));
        }
    }
}

/// The kit next to the launcher (installed: <app>\Source Code), else in a folder above it (a development checkout).
pub fn find_kit() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let mut dir = exe.parent()?.to_path_buf();
    migrate_legacy_kit(&dir);
    crate::update::recover_kit(&dir);
    for _ in 0..6 {
        if dir.join(KIT_DIR).join("kit.json").is_file() {
            return Some(dir.join(KIT_DIR));
        }
        dir = dir.parent()?.to_path_buf();
    }
    None
}
