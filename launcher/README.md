# LCS Recomp Launcher

Tauri 2 (Rust backend, TypeScript/HTML/CSS frontend, Vite), Inno Setup installer. Private - not published.
Design: `design_pack/` (the provided design pack); the assets in use are copied to `public/media/`.

## Build

```
npm install
npx tauri build --no-bundle          -> src-tauri\target\release\lcs-recomp-launcher.exe
ISCC installer\lcs_recomp_launcher.iss  -> dist-installer\LCS-Recomp-Launcher-Setup-<version>.exe
```
Tools on the dev PC (installed 2026-11-17 via winget): Rust 1.100 (msvc), Node.js 26 LTS, Inno Setup 6.7; VS Build
Tools 2022 (C++) and Windows SDK 10.0.26100 were already there.

## Milestone A - audit (2026-11-17)

**Runtime.** `LCSrecomp.exe` (build tree `lcs-recomp-runtime/build/lcsrecomp`: LCS_DIAGNOSTICS OFF, LCS_MENU_VIDEO ON),
Direct3D 11, ~56 MB, plus 25 DLLs (FFmpeg 7.1 LGPL shared, OpenEXR, JXL, WebP, Brotli, FreeType, HarfBuzz, SDL2,
zlib). Started **without arguments** it uses its own folder (`main.cpp releaseStart`):

| Needed | Where it comes from |
|---|---|
| `gamedata\disc\SLUS_214.23` + the disc's folders | the player's ISO (ISO 9660) |
| `gamedata\LCS.iso` | the player's ISO (movies, CDSTREAM, sound read sectors from it) |
| `game\ioprp300\*.irx` (16) | the ISO's `MODULES\IOPRP300.IMG` (ROMDIR; all 16 byte-identical, checked) |
| `saves\mc0`, `logs\last_run.log`, `lcs.ini` | written by the runtime |

Environment switches it reads (defaults set by itself on a release start): `LCS_FULLSCREEN` (0/1) among others.
The launcher sets only `LCS_FULLSCREEN` (its window-mode setting); no invented parameters.

**The executable contains the recompiled game code** (generated C++ from the game's ELF, compiled in). So it is
game-derived and must be produced on the player's PC from their disc - it can't be shipped.

**Build chain** (lcs-recomp-runtime): ps2xRecomp + `lcs-recomp/generated/lcs_us/config.toml` + `config/lcs_us/*.csv` ->
`generated/ee` (5,958 files), `tools/sync_generated.py` adds 7 hooks; `tools/vu1_recomp.py` -> `generated/vu1`;
CMake + Ninja + llvm-mingw (currently inside the VCS reference folder) + `deps/`. Not yet proven reproducible from a
fresh ISO (needs one fresh recompile and a byte compare) - the prerequisite for Milestone E.

**Supported disc** (the only one the code matches): SLUS-21423, 4,010,016,768 bytes,
SHA-256 `9e45271f8835f6b77213c8a7ff3bd1e4307fffa9c3f4434cf8498a5a7b59c4fe`.

**Licences - blockers for a public binary release:**
- **PS2Recomp is GPL-3.0** (`third_party/PS2Recomp/LICENSE`); the runtime links its ps2xRuntime. Distributing
  `LCSrecomp.exe` (or anything built from it) requires offering the corresponding source under the GPL. A closed-source
  binary release of the runtime is **not** compatible with that. Options: permission / another licence from the
  PS2Recomp authors, replacing the GPL parts, or releasing the runtime's source under the GPL. Needs a decision
  before any release - not resolvable in code.
- FFmpeg (LGPL-2.1, shared DLLs): allowed as separate DLLs with notice and the source offer for FFmpeg itself.
- The launcher's own dependencies (Tauri, reqwest, serde, sha2, semver: MIT/Apache-2.0; fonts OFL) are fine for a
  closed-source launcher with their notices.
- Game files and the recompiled code never ship (the launcher prepares them locally).

## What works (built and checked here)

- Window: own dark title bar (drag, minimize, maximize, close), EN/DE switch, sidebar (Home, Installation, Settings,
  Updates, About - no Mods), the design pack's logo, skyline, icons, status symbols, palette. Starts (smoke test: the
  window came up with its title, 2026-11-17).
- Home: Play is enabled only when the game folder is complete (runtime, disc files, ISO, 16 IRX); starts
  `LCSrecomp.exe` with the game folder as working folder, no console window; reports the exit (code, time, the last
  25 lines of `logs\last_run.log` on an error); start errors shown. `logs\launcher.log`. Buttons for game files,
  saves, logs.
- Installation: ISO picker, the check (ISO 9660, SYSTEM.CNF's BOOT2, the executable on the disc, size, SHA-256 with
  progress) with clear messages for other versions / dumps; game folder picker and its file checklist.
- Settings: the runtime's real settings in `lcs.ini` (resolution, widescreen, VSync, frame limiter, MSAA,
  anisotropic filtering, trails, mouse sensitivity, invert mouse, raw input) - written atomically, unknown lines
  kept, locked while the game runs; launcher: window mode (`LCS_FULLSCREEN`), language.
- Updates: GitHub `releases/latest` (HTTPS only, 10 s timeout, no token) and the release's signed manifest, checked
  at start; installing only on a click. See "Updates and releases" below.
- Installer: `dist-installer\LCS-Recomp-Launcher-Setup-0.1.0.exe` (per user, no admin, Start menu, optional desktop
  icon, uninstaller; English and German). **Built, not yet installed / uninstalled for a test.**

## Not built yet (shown as such in the UI)

- **Prepare game** (Milestone E): extracting the disc, the IRX modules, recompiling and building on the player's PC
  - needs the reproducibility check, an own toolchain download (not the VCS folder), bundled `deps/` and Python.
- Installed version: read from `version.json` next to the runtime; none exists yet, so it shows "local build".
- WebView2: present on Windows 10/11 normally; the installer doesn't install it yet.
- Note: an earlier requirement put saves **next to the exe**; the installer keeps that by installing per user into
  `%LOCALAPPDATA%\Programs\LCS Recomp` with the game folder `game\` inside (writable, kept on uninstall).

## Updates and releases (2026-11-17)

Since 0.2.0 the kit folder is called `Source Code` (it is the game's source code: runtime, recompiler, build config);
its file list is still `kit.json`, and an update works in `Source Code.new` / `.old` / `.pending`. A `kit` folder from
0.1.x is renamed on the launcher's first start (`prepare.rs` `migrate_legacy_kit`); the installer removes it on an
upgrade.

A release on GitHub (`NicoWayne95/lcs-ps2-recomp`) carries these assets, made by `release-tool` on the maintainer's
PC (never shipped):

| Asset | Content |
|---|---|
| `manifest.json` | format 1: `version`, `min_launcher`, `kit` {file, size, sha256}, `files` (every kit file: path, size, sha256 - the kit's own `kit.json` list), optional `launcher` {version, file, size, sha256} |
| `manifest.json.sig` | Ed25519 over the exact bytes of `manifest.json`, hex; the public key is `update.rs` `PUBLIC_KEY_HEX` |
| `lcs-recomp-kit-<v>.zip` | the whole kit (`make_kit.py`), no game data |
| `LCS-Recomp-Setup-<v>.exe` | only when the launcher changed |

Making a release:

```
cargo build --release --manifest-path release-tool\Cargo.toml
python <runtime>\tools\make_kit.py "Source Code" C:/Users/Nico/Projects/lcs-recomp <guest.toml> <runtime>/tools/lcs_vu1_uploads.txt
release-tool\target\release\lcs-release.exe make "Source Code" dist-release\<v> --min-launcher <v> [--setup dist-installer\LCS-Recomp-Setup-<v>.exe --launcher-version <v>]
release-tool\target\release\lcs-release.exe verify dist-release\<v>
```

The private key is `%USERPROFILE%\.lcs-release\release.key` (`lcs-release keygen` made it; it refuses to overwrite).
Keep a backup: without it no installed launcher accepts a new release. Versions: the runtime's `VERSION` -> kit.json
-> manifest -> `version.json`; the launcher's own version (`Cargo.toml`, `tauri.conf.json`, the installer's
`AppVersion`) separately, and `min_launcher` says which launcher a release needs.

What the launcher does (`update.rs`, `updates.rs`): check the signature, then the manifest -> download the kit zip
into `game\_update\<v>` (size, SHA-256) -> unpack into `kit.new`, every file against the manifest, nothing extra ->
`kit` becomes `kit.old`, `kit.new` becomes `kit` -> rebuild in `game\_build\work` (`prepare.rs`: sources synced by
the kit's list, only changed files copied and stale ones removed, so Ninja rebuilds only what changed; the ISO's full
SHA-256 is skipped while `gamedata\LCS.iso.ok` matches; build tools from the kit's `config\tools.json`) -> the
installed exe/DLLs go to `game\_backup` and the new ones in -> `kit.old` and the download are removed. A failure
before that puts `kit.old` back and leaves the game as it was; an interrupted swap is repaired at the next start.
"Restore version" swaps the installed build with `_backup`. A newer `min_launcher` offers the release's installer
(SHA-256 checked, started on a click, the launcher closes). `saves\`, `lcs.ini` and the ISO are never touched.

Headless test: `lcs-recomp-launcher.exe --update-from <release folder> <game folder>`.

## Entwicklung im zentralen Projekt

Für aktuelle Build- und Release-Pfade gilt die zentrale Dokumentation unter ../docs/. Der aktive Spielquellcode liegt in ../game. Der Ordner Source Code entsteht bei der Paketerstellung aus diesem Stand. Für den lokalen Start dienen ../Build-Launcher.ps1 und ../Start-Launcher.ps1. Die Prepare-Funktion ist in prepare.rs implementiert, einschließlich ISO-Prüfung, lokaler Recompilation und inkrementeller Kompilierung. Die Abschnitte oben dokumentieren den historischen Stand.
