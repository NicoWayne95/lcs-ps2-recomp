# LCS Recomp 0.2.0 — Public Beta

**Release date:** November 2026 · **Status:** Beta · **Platform:** Windows 10/11 (64-bit), DirectX 11

LCS Recomp is a native PC port of *Grand Theft Auto: Liberty City Stories* (PS2), created through static
recompilation. The launcher builds the game on your PC from **your own copy** of the game — no game files are
included in this download.

This is the first public beta. Most of the game is fully playable, but there are known issues and unfinished
features listed below. Your feedback decides what we fix next.

> **Requirements:** your own ISO of *GTA: Liberty City Stories* for PS2, **USA version (SLUS-21423)**. An internet
> connection is needed on the first start (the launcher downloads its build tools), plus a few GB of free disk space.

---

## Highlights of 0.2.0

- **Cutscenes are in sync at high frame rates.** At 120 FPS and uncapped, characters and vehicles no longer drift
  off their paths (the opening bus no longer turns into a building), and picture and sound stay together.
- **Fewer stutters.** The game now has 64 MB of memory instead of the PS2's 32 MB. The streaming memory, which was
  almost always full on the PS2, now has plenty of room, so models are no longer constantly thrown out and
  reloaded from disc.
- **Draw distance setting.** Pedestrians and vehicle detail can be drawn much further away (50–250%).
- **Leaner release build.** Diagnostic logging no longer runs in the background; one leftover diagnostic that
  caused hitches whenever police cars appeared has been removed.

---

## ✅ What works

### Game
- The full game from boot to free roam and missions, including the original intro movies.
- Native sound driver: music, radio, effects and cutscene audio, played in real time.
- Saves are stored in the `Savegame` folder inside your game folder.

### Graphics
- DirectX 11 renderer with internal resolutions from the PS2's original 640×448 up to 4K.
- True 16:9 widescreen (more picture at the sides, not stretched), with the HUD kept in proportion.
- MSAA anti-aliasing (2×/4×/8×) and anisotropic texture filtering (up to 16×).
- The PS2 "trails" motion-blur effect, rendered sharply at your resolution, and the glow effects.
- Fullscreen and windowed mode (F11).

### Frame rate
- Frame limiter with **30 (PS2), 60, 120 FPS and uncapped**, with even frame pacing.
- Game speed, physics, vehicle handling and per-frame mission meters are corrected to run at the right speed at
  high frame rates (supported up to 240 FPS).
- VSync, or tearing / variable refresh rate (G-Sync, FreeSync).

### Controls
- Keyboard and mouse with a layout modelled on the PC versions of GTA, and a mouse-controlled camera on foot.
- Controller support.

### Pause menu
- New **Display Options** and **Video Options** pages inside the game's own pause menu: resolution, widescreen,
  frame sync, frame limiter, draw distance, MSAA, texture filtering, trails, beta police cars, mouse settings.
- All settings are saved to `lcs.ini` and restored on the next start.
- Mouse support in the menu (hover and click), Esc/right-click to go back.

### Launcher
- Checks your ISO, then builds the game on your PC in one guided step.
- Settings before and after the build, save game page with backups.
- Signed updates: future versions can be installed from the launcher, your saves, settings and ISO are never
  touched, and the previous version is kept so you can go back.
- Setup (default folder `C:\Games\LCS Recomp`) or a portable zip that keeps everything in one folder.
- Uninstalling keeps your saves.

---

## 🧪 New in this beta — please verify

These changes are built and partly tested, but we need more eyes on them:

- **Wanted stars and flashing HUD items** blink at the PS2's rate at any frame rate (they used to strobe at
  120 FPS and above).
- **64 MB memory layout** across long sessions (30 minutes and more): crashes, missing objects or graphics
  glitches would point to it.
- **Cutscenes at uncapped frame rates** (150 FPS and above): picture and sound should stay in sync.
- **Scope aiming with the mouse** (sniper rifle, rocket launcher).
- **Shop sale meters** (car and bike dealerships) and **mission countdowns** at high frame rates.

---

## ⚠️ Known issues and limitations

- **Building and prop draw distance:** The draw distance setting affects pedestrians and vehicle detail. Buildings,
  roads, street lights and other props are limited to roughly 300–500 m by the game's own map data, so they don't
  appear further away yet. This is the next big item we are working on (see *Coming next*).
- **Mouse free aim** is not finished: aiming with the mouse uses the game's target lock for now.
- **Button icons** in on-screen prompts don't always match the device you are using.
- **Frame rates above 240 FPS** are not supported: at that point the game runs slightly faster than real time.
- **Launcher:** cancelling and resuming the first build, and error cases (no internet, a wrong ISO, little disk
  space), have had limited testing.
- Only the **USA version (SLUS-21423)** of the game is supported.

---

## 🔜 Coming next

- **Extended world draw distance** — buildings, roads and props drawn much further than on the PS2, using the
  extra memory this version adds.
- **Mouse free aim.**
- **Button icons** that follow your input device (keyboard/mouse or controller).
- More options in the pause menu.
- Fixes based on your feedback.

---

## 🐞 Feedback and bug reports

Please report bugs and feedback as a GitHub issue:
**https://github.com/NicoWayne95/lcs-ps2-recomp/issues**

Helpful to include:
1. What happened, what you expected, and how to get there (mission, place, steps).
2. The log file `logs\last_run.log` from your game folder (after a crash: `previous_run.log`).
3. Your settings file `lcs.ini` from your game folder.
4. Graphics card, CPU, Windows version and the frame rate you play at.
5. A screenshot or short video if possible.

Your game folder is `C:\Games\LCS Recomp\game` with the setup, or the `game` folder next to the launcher with the
portable version.

**Please don't upload ISOs, other people's saves or files from the `cache` folder.**

---

## Downloads

| File | Description |
|---|---|
| `LCS-Recomp-Beta-Setup-0.2.0.exe` | Installer (Start menu entry, uninstaller; optional portable mode) |
| `LCS-Recomp-Beta-0.2.0-portable.zip` | Portable version — unzip anywhere writable and start `lcs-recomp-launcher.exe` |

*LCS Recomp is a fan project and is not affiliated with or endorsed by Rockstar Games or Take-Two Interactive.
You need to own the original game.*
