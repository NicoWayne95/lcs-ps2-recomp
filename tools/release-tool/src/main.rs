//! LCS Recomp release tool - runs on the maintainer's PC only, never ships with the launcher.
//!
//!   lcs-release keygen [<key dir>]            a new Ed25519 key pair (default %USERPROFILE%\.lcs-release); refuses
//!                                             to overwrite one. The public half goes into the launcher (updates.rs).
//!   lcs-release pubkey [<key dir>]            prints the public key (hex)
//!   lcs-release make <kit dir> <out dir> [--key <file>] [--min-launcher <v>] [--setup <exe> --launcher-version <v>]
//!                                             the release assets: lcs-recomp-kit-<v>.zip, manifest.json,
//!                                             manifest.json.sig (and the setup exe, copied, when given)
//!   lcs-release verify <out dir> [--pub <hex>] checks a release folder the way the launcher does
//!
//! manifest.json (format 1): version, min_launcher, kit {file, size, sha256}, files [{path, size, sha256}] (the
//! kit's own list from kit.json), launcher {version, file, size, sha256} (optional). The signature is Ed25519 over the
//! exact bytes of manifest.json, stored as hex text in manifest.json.sig.

use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

fn unhex(s: &str) -> Result<Vec<u8>, String> {
    let s = s.trim();
    if s.len() % 2 != 0 {
        return Err("odd hex length".into());
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).map_err(|e| e.to_string())).collect()
}

fn sha256_file(p: &Path) -> Result<(u64, String), String> {
    let mut f = File::open(p).map_err(|e| format!("{}: {}", p.display(), e))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    let mut n_total = 0u64;
    loop {
        let n = f.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        n_total += n as u64;
        h.update(&buf[..n]);
    }
    Ok((n_total, hex(&h.finalize())))
}

fn default_key_dir() -> PathBuf {
    PathBuf::from(std::env::var("USERPROFILE").unwrap_or_else(|_| ".".into())).join(".lcs-release")
}

fn load_key(file: &Path) -> Result<SigningKey, String> {
    let text = fs::read_to_string(file).map_err(|e| format!("{}: {}", file.display(), e))?;
    let seed: [u8; 32] = unhex(&text)?.try_into().map_err(|_| "the key file must hold 32 bytes (64 hex digits)")?;
    Ok(SigningKey::from_bytes(&seed))
}

fn keygen(dir: &Path) -> Result<(), String> {
    let key = dir.join("release.key");
    if key.exists() {
        return Err(format!("{} exists already - not overwritten", key.display()));
    }
    fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    let mut seed = [0u8; 32];
    getrandom::getrandom(&mut seed).map_err(|e| e.to_string())?;
    let sk = SigningKey::from_bytes(&seed);
    let mut f = fs::OpenOptions::new().write(true).create_new(true).open(&key).map_err(|e| e.to_string())?;
    f.write_all(hex(&seed).as_bytes()).map_err(|e| e.to_string())?;
    let public = hex(sk.verifying_key().as_bytes());
    fs::write(dir.join("release.pub"), &public).map_err(|e| e.to_string())?;
    println!("private key: {} (keep it safe and backed up, never in a repository)", key.display());
    println!("public key:  {}", public);
    Ok(())
}

fn walk(dir: &Path, base: &Path, out: &mut Vec<String>) -> Result<(), String> {
    for e in fs::read_dir(dir).map_err(|e| format!("{}: {}", dir.display(), e))? {
        let e = e.map_err(|e| e.to_string())?;
        let p = e.path();
        if e.file_type().map_err(|e| e.to_string())?.is_dir() {
            walk(&p, base, out)?;
        } else {
            out.push(p.strip_prefix(base).unwrap().to_string_lossy().replace('\\', "/"));
        }
    }
    Ok(())
}

fn safe_name(s: &str) -> bool {
    !s.is_empty() && s.len() < 128 && s.chars().all(|c| c.is_ascii_alphanumeric() || "._-".contains(c))
}

fn make(kit: &Path, out: &Path, key_file: &Path, min_launcher: &str, setup: Option<(PathBuf, String)>) -> Result<(), String> {
    let sk = load_key(key_file)?;
    let kit_json: Value = serde_json::from_str(&fs::read_to_string(kit.join("kit.json")).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?;
    let version = kit_json["version"].as_str().ok_or("kit.json without a version")?.to_string();
    semver::Version::parse(&version).map_err(|e| format!("version {}: {}", version, e))?;
    semver::Version::parse(min_launcher).map_err(|e| format!("min launcher {}: {}", min_launcher, e))?;
    let files = kit_json["files"].as_array().ok_or("kit.json without a file list (made by an older make_kit.py)")?;

    // the kit on disk must be exactly its list: every file, nothing else (kit.json itself aside)
    let mut on_disk = Vec::new();
    walk(kit, kit, &mut on_disk)?;
    on_disk.retain(|p| p != "kit.json");
    on_disk.sort();
    let mut listed: Vec<String> = Vec::new();
    for f in files {
        let path = f["path"].as_str().ok_or("file without a path")?;
        let (size, sha) = sha256_file(&kit.join(path))?;
        if Some(size) != f["size"].as_u64() || Some(sha.as_str()) != f["sha256"].as_str() {
            return Err(format!("kit file differs from kit.json: {}", path));
        }
        listed.push(path.to_string());
    }
    listed.sort();
    if listed != on_disk {
        let extra: Vec<_> = on_disk.iter().filter(|p| !listed.contains(p)).take(5).collect();
        return Err(format!("files in the kit that kit.json does not list: {:?}", extra));
    }

    fs::create_dir_all(out).map_err(|e| e.to_string())?;
    let zip_name = format!("lcs-recomp-kit-{}.zip", version);
    let zip_path = out.join(&zip_name);
    {
        let f = File::create(&zip_path).map_err(|e| e.to_string())?;
        let mut z = zip::ZipWriter::new(f);
        let opts = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
        let mut all = listed.clone();
        all.push("kit.json".into());
        for p in &all {
            z.start_file(p.as_str(), opts).map_err(|e| e.to_string())?;
            z.write_all(&fs::read(kit.join(p)).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
        }
        z.finish().map_err(|e| e.to_string())?;
    }
    let (zsize, zsha) = sha256_file(&zip_path)?;
    let mut manifest = json!({
        "format": 1,
        "version": version,
        "min_launcher": min_launcher,
        "kit": { "file": zip_name, "size": zsize, "sha256": zsha },
        "files": files,
    });
    if let Some((exe, lver)) = setup {
        semver::Version::parse(&lver).map_err(|e| format!("launcher version {}: {}", lver, e))?;
        let name = exe.file_name().unwrap().to_string_lossy().to_string();
        if !safe_name(&name) {
            return Err(format!("setup file name not allowed: {}", name));
        }
        fs::copy(&exe, out.join(&name)).map_err(|e| e.to_string())?;
        let (size, sha) = sha256_file(&exe)?;
        manifest["launcher"] = json!({ "version": lver, "file": name, "size": size, "sha256": sha });
    }
    let bytes = serde_json::to_vec_pretty(&manifest).map_err(|e| e.to_string())?;
    fs::write(out.join("manifest.json"), &bytes).map_err(|e| e.to_string())?;
    fs::write(out.join("manifest.json.sig"), hex(&sk.sign(&bytes).to_bytes())).map_err(|e| e.to_string())?;
    println!("release {}: {} ({:.1} MB), manifest.json, manifest.json.sig in {}", version, zip_name, zsize as f64 / 1e6, out.display());
    verify(out, &sk.verifying_key())
}

fn verify(out: &Path, public: &VerifyingKey) -> Result<(), String> {
    let bytes = fs::read(out.join("manifest.json")).map_err(|e| e.to_string())?;
    let sig: [u8; 64] = unhex(&fs::read_to_string(out.join("manifest.json.sig")).map_err(|e| e.to_string())?)?
        .try_into()
        .map_err(|_| "signature must be 64 bytes")?;
    public.verify(&bytes, &Signature::from_bytes(&sig)).map_err(|_| "signature invalid")?;
    let m: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    let mut parts = vec![m["kit"].clone()];
    if m.get("launcher").is_some() {
        parts.push(m["launcher"].clone());
    }
    for p in parts {
        let name = p["file"].as_str().ok_or("asset without a file")?;
        let (size, sha) = sha256_file(&out.join(name))?;
        if Some(size) != p["size"].as_u64() || Some(sha.as_str()) != p["sha256"].as_str() {
            return Err(format!("{} does not match the manifest", name));
        }
    }
    println!("verified: signature ok, assets match ({} kit files listed)", m["files"].as_array().map_or(0, |a| a.len()));
    Ok(())
}

fn flag(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1).cloned())
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let r = match args.get(1).map(String::as_str) {
        Some("keygen") => keygen(&args.get(2).map(PathBuf::from).unwrap_or_else(default_key_dir)),
        Some("pubkey") => {
            let dir = args.get(2).map(PathBuf::from).unwrap_or_else(default_key_dir);
            load_key(&dir.join("release.key")).map(|k| println!("{}", hex(k.verifying_key().as_bytes())))
        }
        Some("make") if args.len() >= 4 => {
            let key = flag(&args, "--key").map(PathBuf::from).unwrap_or_else(|| default_key_dir().join("release.key"));
            let min = flag(&args, "--min-launcher").unwrap_or_else(|| "0.0.0".into());
            let setup = match (flag(&args, "--setup"), flag(&args, "--launcher-version")) {
                (Some(s), Some(v)) => Some((PathBuf::from(s), v)),
                (None, None) => None,
                _ => {
                    eprintln!("--setup and --launcher-version go together");
                    std::process::exit(2);
                }
            };
            make(Path::new(&args[2]), Path::new(&args[3]), &key, &min, setup)
        }
        Some("verify") if args.len() >= 3 => {
            let public = match flag(&args, "--pub") {
                Some(h) => Ok(h),
                None => fs::read_to_string(default_key_dir().join("release.pub")).map_err(|e| e.to_string()),
            };
            public
                .and_then(|h| unhex(&h))
                .and_then(|b| <[u8; 32]>::try_from(b).map_err(|_| "public key must be 32 bytes".to_string()))
                .and_then(|b| VerifyingKey::from_bytes(&b).map_err(|e| e.to_string()))
                .and_then(|k| verify(Path::new(&args[2]), &k))
        }
        _ => {
            eprintln!("usage: lcs-release keygen [dir] | pubkey [dir] | make <kit> <out> [--key f] [--min-launcher v] [--setup exe --launcher-version v] | verify <out> [--pub hex]");
            std::process::exit(2);
        }
    };
    if let Err(e) = r {
        eprintln!("error: {}", e);
        std::process::exit(1);
    }
}
