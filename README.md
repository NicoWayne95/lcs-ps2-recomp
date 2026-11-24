
<h1 align="center">LCS Recomp</h1>

<p align="center">
  <img src="LCS_recomp2.png" alt="LCS Recomp — Unofficial Concept Artwork" width="430">
</p>

<h3 align="center">Liberty City Stories. Recompiled for PC.</h3>

<p align="center">
  An unofficial native PC recompilation of the PlayStation 2 version of<br>
  <strong>Grand Theft Auto: Liberty City Stories</strong>.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%20x64-0078D4?style=flat-square" alt="Windows x64">
  <img src="https://img.shields.io/badge/Renderer-DirectX%2011-0078D4?style=flat-square" alt="DirectX 11">
  <img src="https://img.shields.io/badge/Status-Public%20Beta-E97132?style=flat-square" alt="Public Beta">
  <img src="https://img.shields.io/github/v/release/NicoWayne95/lcs-ps2-recomp?style=flat-square" alt="Latest Release">
  <img src="https://img.shields.io/github/downloads/NicoWayne95/lcs-ps2-recomp/total?style=flat-square" alt="Downloads">
  <img src="https://img.shields.io/github/stars/NicoWayne95/lcs-ps2-recomp?style=flat-square" alt="GitHub Stars">
</p>

<p align="center">
  <a href="#latest-release"><strong>Download</strong></a> ·
  <a href="https://github.com/NicoWayne95/lcs-ps2-recomp/releases/latest">Release Notes</a> ·
  <a href="#whats-new">What's New</a> ·
  <a href="#performance">Performance</a> ·
  <a href="#features">Features</a> ·
  <a href="#installation">Installation</a>
</p>

<p align="center">
  <a href="#known-issues">Known Issues</a> ·
  <a href="#roadmap">Roadmap</a> ·
  <a href="#screenshots">Screenshots</a> ·
  <a href="#source-code">Source Code</a> ·
  <a href="https://github.com/NicoWayne95/lcs-ps2-recomp/discussions">Discussions</a> ·
  <a href="https://github.com/NicoWayne95/lcs-ps2-recomp/issues">Report a Bug</a>
</p>

---

## Welcome back to Liberty City!

**LCS Recomp** is an independent fan project bringing the PlayStation 2 version of Grand Theft Auto: Liberty City Stories to PC through static recompilation and a custom native runtime.

The goal is to preserve the atmosphere, gameplay, and visual identity of the original game while introducing optional improvements designed for modern PC hardware.

Players can enjoy the classic PlayStation 2 experience or take advantage of higher resolutions, improved frame rates, modern controls, additional graphics options, and native PC enhancements.

Unlike traditional emulation, LCS Recomp uses a locally prepared executable and a custom runtime that combines recompiled PS2 game code with increasingly optimized native PC implementations.

> [!IMPORTANT]
> **LCS Recomp is a solo-developed project.**
>
> I'm developing and maintaining this project entirely on my own in my spare time, covering reverse engineering, runtime development, debugging, optimization, testing, and releases.
>
> This is my first recompilation project of this kind, and I'm continuously learning along the way. The project builds upon existing open-source work and uses AI-assisted development tools, but development decisions, integration, testing, and release management remain my responsibility.
>
> As this is a personal hobby project, development progress depends on the time I have available.

> [!WARNING]
> **LCS Recomp is currently in Public Beta.**
>
> Although the game is playable, bugs, crashes, performance limitations, graphical issues, and incomplete features may still occur. Not every mission, hardware configuration, or gameplay feature has been fully validated.

**To play LCS Recomp, you need your own supported PlayStation 2 game ISO.** The launcher prepares the game locally. No original copyrighted game assets or ISO files are included with the project.

---

## <a id="latest-release"></a>Latest Release

<p align="center">
  <img src="docs/features/v0.3.0.png" alt="LCS Recomp v0.3.0 — Performance & Quality of Life Update" width="100%">
</p>

<h3 align="center">v0.3.0 — Performance & Quality of Life Update</h3>

<p align="center">
  Faster rendering, significantly improved frame pacing, reduced stuttering,
  HD texture packs, and additional PC-focused features.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Version-v0.3.0-2ea44f?style=flat-square" alt="Version 0.3.0">
  <img src="https://img.shields.io/badge/Released-November%2024%2C%202026-555555?style=flat-square" alt="Release Date">
  <img src="https://img.shields.io/badge/Status-Public%20Beta-E97132?style=flat-square" alt="Public Beta">
</p>

Version **0.3.0** introduces one of the largest technical updates to LCS Recomp so far.

Major parts of the rendering pipeline, frame presentation, CPU scheduling, resource management, and world streaming have been optimized to reduce unnecessary processing overhead and improve performance consistency.

In internal testing, demanding scenes with heavy traffic and pedestrians improved from approximately **130 FPS to 210–225 FPS**, representing a performance increase of roughly **60–70%** in that particular workload.

The update also introduces **PCSX2-compatible HD texture packs, SMAA anti-aliasing, native HDR output, adjustable text size, controller vibration**, and numerous rendering and gameplay improvements.

<p align="center">
  <a href="https://github.com/NicoWayne95/lcs-ps2-recomp/releases/download/v0.3.0/LCS-Recomp-Beta-0.3.0-portable.zip">
    <img src="https://img.shields.io/badge/Download-v0.3.0%20Portable-2878c7?style=for-the-badge&logo=github" alt="Download LCS Recomp v0.3.0">
  </a>
  <a href="https://github.com/NicoWayne95/lcs-ps2-recomp/releases/tag/v0.3.0">
    <img src="https://img.shields.io/badge/View-Release%20Notes-555555?style=for-the-badge&logo=github" alt="View v0.3.0 Release Notes">
  </a>
</p>

> [!TIP]
> **Already playing LCS Recomp?**
>
> Open your existing launcher and install the latest compatible game update through the integrated updater. You normally do not need to reinstall the launcher.
>
> Game updates are designed to preserve your original ISO, savegames, and configuration. Launcher upgrades may occasionally be required for compatibility.

> [!NOTE]
> **New players:** Download the v0.3.0 portable package, extract it into a writable folder, and follow the included `START-HERE.txt`.
>
> You will need your own supported **USA / NTSC-U PS2 ISO (SLUS-21423)**.

---

## <a id="whats-new"></a>What's New in v0.3.0?

### Major Improvements at a Glance

| Category | Improvements |
| :--- | :--- |
| **Performance** | Up to approximately 60–70% higher FPS in the tested traffic-heavy scenario |
| **Native Rendering** | Optimized vehicle rendering, shared meshes, faster skinning, and reduced GS processing |
| **Frame Pacing** | Reworked presentation pipeline to reduce wasted GPU work and improve high-refresh-rate smoothness |
| **CPU Efficiency** | Faster scheduler, optimized memory handling, reduced allocations, and reusable command buffers |
| **World Streaming** | Persistent level decompression cache and asynchronous texture processing |
| **HD Texture Packs** | Native support for compatible PCSX2 replacement texture packs |
| **Anti-Aliasing** | SMAA, SMAA 2X, and SMAA 4X |
| **HDR** | Native HDR output with configurable brightness and highlight controls |
| **Display Configuration** | Automatic resolution and refresh-rate detection |
| **Interface** | Adjustable text size and improved HUD rendering |
| **Controllers** | Native vibration and original DualShock 2/3 button prompts |
| **Gameplay Timing** | Improved high-FPS simulation and animation consistency |
| **Savegames** | Improved backup reliability and consolidated save directories |

### HD Texture Pack Support

LCS Recomp now supports compatible **PCSX2 texture replacement packs** without requiring manual filename changes or texture conversion.

Features include:

- Automatic recognition of supported replacement textures.
- Support for 4-bit and 8-bit palettized textures.
- HD replacements for HUD elements, icons, radar, maps, fonts, and loading screens.
- Background texture loading to reduce gameplay interruptions.
- Original textures remain visible until their replacements are ready.

**Installation:**

Place the texture pack's `SLUS-21423` folder inside `textures/` next to `LCSrecomp.exe`.

```text
LCS Recomp/
├── LCSrecomp.exe
└── textures/
    └── SLUS-21423/
        └── replacements/
            └── ...
```

> [!NOTE]
> HD texture packs are not included. Only use replacement textures you are authorized to use.
>
> Region-suffixed replacements and custom mipmap levels are not yet supported.

### SMAA Anti-Aliasing

Three additional anti-aliasing options are available:

- SMAA
- SMAA 2X
- SMAA 4X

SMAA processes the rendered 3D scene before the HUD is composited, helping maintain sharp text and interface elements.

These options provide a less expensive alternative to demanding MSAA settings, particularly at higher resolutions.

### HDR Output — Stage 1

LCS Recomp supports native HDR output on compatible displays when HDR is enabled in Windows.

The HDR settings provide controls for:

- Paper-white brightness.
- Peak brightness.
- Highlight intensity.
- Highlight start.
- Saturation.
- Display-reported peak brightness.

HDR activation changes require a game restart. Image adjustments take effect immediately.

### Adjustable Text Size

A new **Text Size** setting is available in the F2 configuration menu.

- Adjustable from 75% to 150%.
- Applies to subtitles, mission messages, help text, area names, vehicle names, and radio stations.
- Text wrapping, background boxes, and embedded button prompts scale together.
- The clock, money display, ammunition counter, and pause menu retain their existing sizes.

### Controller & Input Improvements

Version 0.3.0 also introduces:

- Native controller vibration with adjustable intensity.
- Individual vibration profiles for supported controllers.
- Original DualShock 2 and DualShock 3 button glyphs.
- Improved vibration shutdown behavior.
- Persistent vibration preferences through `lcs.ini`.

### Additional Quality of Life Improvements

- Automatic monitor detection during initial configuration.
- Live vehicle reflection resolution selection.
- Improved radar clipping.
- Improved decal and object visibility.
- More reliable memory-card backups.
- Improved loading-screen and audio synchronization.
- More consistent gameplay timing at high frame rates.

[**Read the complete v0.3.0 changelog**](https://github.com/NicoWayne95/lcs-ps2-recomp/releases/tag/v0.3.0)

---

## <a id="performance"></a>Rendering & Performance

### Major Performance Improvements

Performance optimization is one of the main development priorities of LCS Recomp.

Version 0.3.0 substantially reduces CPU-side rendering overhead and improves resource reuse, command submission, and frame delivery.

### Internal Benchmark Results

| Test Scenario | Previous | v0.3.0 |
| :--- | ---: | ---: |
| Portland Police Station — heavy traffic and pedestrians | ~130 FPS | **~210–225 FPS** |
| Frame time in the same scene | ~7.5–7.8 ms | **~4.5–4.8 ms** |
| Command-chain buffer reuse benchmark | 118 FPS | **145 FPS** |
| Frames exceeding 10 ms in the same test | 3.3% | **0.1%** |
| Free camera — ground toward water | 247 FPS | **Up to 330 FPS** |
| Free camera — above the city | 154 FPS | **170 FPS** |
| Free camera — high above toward another island | 224 FPS | **277 FPS** |

> [!NOTE]
> These are internal development measurements collected on an NVIDIA RTX 4090 system.
>
> Actual performance depends on CPU, GPU, resolution, graphics settings, and scene complexity. Improvements observed on this system are not guaranteed across all hardware configurations.

### Rendering Architecture

The runtime combines recompiled PS2 game code with increasingly optimized native rendering implementations.

Current improvements include:

- Native rendering of eligible static world geometry.
- Persistent GPU buffers for supported world meshes.
- Shared vehicle geometry.
- Native shader-based vehicle environment mapping.
- Optimized character and vehicle skinning.
- Faster mesh identification and visibility checks.
- Reduced unnecessary PS2 Graphics Synthesizer processing.
- Improved synchronization between native and PS2-oriented rendering paths.

The native rendering paths are only used where their behavior has been sufficiently validated. Other operations continue through compatible fallback paths where necessary.

### Frame Pacing & Presentation

The presentation system has been reworked to reduce unnecessary GPU work.

Previously, the rendering pipeline could accumulate multiple frames ahead of the display. Under heavy GPU load, this resulted in additional work for frames that were never presented.

Version 0.3.0 introduces:

- Reduced rendering queue depth.
- Improved frame presentation synchronization.
- Reduced wasted GPU processing.
- More consistent high-refresh-rate behavior.
- Improved accuracy between internal FPS reporting and actual frame presentation.

These changes are especially relevant when using higher resolutions, high refresh rates, or demanding graphics settings.

### Texture & Resource Management

Additional improvements include:

- Reference-based texture processing to reduce unnecessary memory copying.
- Redundant texture upload detection.
- Improved palette reuse.
- More efficient command submission.
- Expanded GPU mesh caching.
- Incremental cache eviction.
- Asynchronous texture processing and cleanup.

The GPU mesh cache now supports **64,000 entries**, compared with the previous 12,000-entry limit.

### Streaming & Loading

World streaming benefits from:

- Persistent level decompression caching.
- Additional texture worker threads.
- Reduced main-thread resource creation.
- Background cache cleanup.
- Improved memory-card backup processing.
- Reduced frame-time spikes during island transitions and district loading.

### CPU Compatibility

**LCS Recomp automatically detects supported CPU instruction sets during game preparation.**

The launcher selects an appropriate build configuration:

- AVX2-optimized builds for compatible processors.
- AVX-compatible builds for older processors without AVX2.
- No manual CPU optimization selection required for normal installation.

This improves compatibility across different CPU generations while allowing modern systems to use more optimized instruction sets.

Actual compatibility still depends on the complete CPU and operating system configuration.

---

## <a id="features"></a>Features

The following features are available in the current public beta. Some remain experimental or may require further compatibility testing.

| Graphics & Display | Performance & Gameplay |
| :--- | :--- |
| DirectX 11 rendering | Playable open-world gameplay |
| Original PS2 rendering resolution | Playable missions and original cutscenes |
| 1080p, 1440p, and 4K options | 30, 60, 120, 144, 180, and 240 FPS modes |
| Widescreen support | Uncapped mode with a default 240 FPS safety limit |
| MSAA up to 8× | Improved high-FPS gameplay timing |
| SMAA, SMAA 2X, SMAA 4X | Optimized native rendering paths |
| Anisotropic filtering up to 16× | Improved frame pacing |
| Original trails and glow effects | Improved world streaming |
| HDR output — Stage 1 | Expanded 64 MB game memory configuration |
| Configurable vehicle reflections | Local savegames and backups |

| Controls & Input | PC Enhancements |
| :--- | :--- |
| Xbox controller support | PCSX2-compatible HD texture packs |
| PlayStation controller support | Adjustable text scaling |
| DualShock 3 pressure-sensitive controls | Additional graphics configuration |
| Original DualShock 2/3 button prompts | Automatic monitor detection |
| Adaptive controller prompts | Unified `lcs.ini` configuration |
| Native controller vibration | Local performance diagnostics |
| Seamless input-device switching | Free Camera mode |
| Native keyboard and mouse input | Toggleable HUD |
| Free mouse-controlled vehicle camera | Mouse-wheel weapon switching |
| Classic controller layout | Native audio and video playback |

### Original DualShock 3 Pressure-Sensitive Controls

Compatible DualShock 3 controllers support pressure-sensitive input for the original game controls.

| Action | Function |
| :--- | :--- |
| Acceleration | Analog pressure-sensitive throttle |
| Braking | Analog pressure-sensitive braking |
| Steering | Pressure-sensitive directional-pad steering |

Compatible drivers exposing analog button-pressure values are required.

### Audio & Video

The native audio and video systems support:

- Original radio stations.
- Original music and sound effects.
- Ambient audio.
- Dialogue and mission audio.
- Cutscene audio.
- Introductory movie playback.

### Classic & Enhanced Gameplay

The long-term vision is to allow players to choose how closely their experience resembles the original PlayStation 2 release.

| Classic Experience | Enhanced PC Experience |
| :--- | :--- |
| Original PS2 visual identity | Higher rendering resolutions |
| Original rendering resolution | Up to 4K |
| Classic controller layout | Native keyboard and mouse |
| Original 30 FPS timing | Higher frame rates |
| Pressure-sensitive controls | Modern input options |
| Original visual effects | SMAA, HDR, and additional settings |
| Original camera behavior | Improved vehicle camera controls |
| Original texture presentation | Optional HD texture replacements |
| Original world presentation | Extended draw distance (planned) |

These represent available options and long-term development goals, not two fully independent gameplay modes.

---

## <a id="installation"></a>Installation

### First-Time Installation

**New players should download the latest portable release.**

1. Download [**LCS Recomp v0.3.0 Portable**](https://github.com/NicoWayne95/lcs-ps2-recomp/releases/download/v0.3.0/LCS-Recomp-Beta-0.3.0-portable.zip).
2. Extract the ZIP archive into a writable folder.
3. Follow the included `START-HERE.txt`.
4. Start **LCS Recomp Launcher**.
5. Open the **Installation** section.
6. Select your original supported PlayStation 2 game ISO.
7. Use **Check ISO** to validate the selected image.
8. Select your preferred game installation directory if needed.
9. Click **Prepare game**.
10. Allow the launcher to download the required tools and prepare the native executable.
11. Open **Home** and select **Play**.

No manual compilation or programming knowledge is required for normal installation.

The original ISO remains on your computer. The launcher prepares the game locally.

### Updating an Existing Installation

> [!TIP]
> **Already have LCS Recomp installed?**
>
> You normally do not need to download the launcher again for compatible game updates.

1. Open the existing LCS Recomp Launcher.
2. Check for the latest available game update.
3. Install v0.3.0 if it is offered.
4. Allow the launcher to complete download, verification, and preparation.
5. Start the game normally.

Launcher upgrades are separate from game updates and may occasionally be necessary.

Backing up your savegames before major updates is recommended while the project remains in beta.

### Supported Game Version

> [!IMPORTANT]
> You must provide your own legally obtained PlayStation 2 copy of Grand Theft Auto: Liberty City Stories in ISO format.
>
> **Supported version: USA / NTSC-U (SLUS-21423).**
>
> Other regional releases and the PSP version are not currently supported.

### System Requirements

| Requirement | Details |
| :--- | :--- |
| Operating System | Windows 10 / Windows 11 (64-bit) |
| Graphics API | DirectX 11 |
| CPU | AVX-capable x64 processor |
| CPU Optimization | Automatic AVX / AVX2 selection |
| Game Version | PlayStation 2 — USA / NTSC-U |
| Game ID | SLUS-21423 |
| Game Format | Original supported ISO |
| Internet Connection | Required for initial preparation and updates |
| Launcher Runtime | Microsoft Edge WebView2 |

Definitive minimum and recommended CPU, GPU, and RAM specifications have not yet been established through broad hardware testing.

### Recommended Settings

| Setting | Recommendation |
| :--- | :--- |
| Frame Rate | 60 FPS for a conservative gameplay baseline |
| Input Device | Controller |
| Controller Layout | Classic |
| Resolution | Native monitor resolution, adjusted to available performance |
| Anti-Aliasing | SMAA as an alternative to expensive MSAA settings |
| HD Textures | Optional, depending on available memory and performance |
| HDR | Optional on supported displays |

Higher frame rates remain experimental in some gameplay scenarios.

### Microsoft Edge WebView2

The launcher requires Microsoft Edge WebView2 Runtime.

Most current Windows installations already include it. If the launcher fails to start or displays an empty window, the runtime may need to be installed.

[Download Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)

### Windows SmartScreen

The launcher and distributed executables are not currently Windows code-signed.

Windows SmartScreen may display an unknown publisher warning.

Only download LCS Recomp from the official repository and verify downloaded files before execution.

---

## <a id="known-issues"></a>Known Issues

LCS Recomp remains under active development. The following limitations are known in the current public beta.

| Issue | Current Status |
| :--- | :--- |
| **Extended World Draw Distance** | Not fully implemented; original streaming and LOD systems still impose limitations |
| **Blue Beta Police Cars** | Optional recoloring requires further material-handling work |
| **Native Mouse Free Aim** | Additional development required |
| **Vehicle Camera** | Certain angles and recentering scenarios may still require refinement |
| **HD Texture Packs** | Region-suffixed replacements and custom mipmap levels are not yet supported |
| **Pixel Fonts** | Some HUD and menu elements retain original pixel fonts when replacement packs do not cover them |
| **Particle Rotation** | Some effects still contain frame-dependent rotational behavior |
| **HDR** | HUD brightness is affected by the current HDR implementation; launcher and pause menu HDR remain incomplete |
| **High-FPS Gameplay** | Not every mission, animation, or cutscene has been validated at elevated frame rates |
| **Hardware Compatibility** | Additional testing on older and lower-end systems is needed |
| **Long-Session Stability** | Further gameplay and compatibility validation remains ongoing |

### Draw Distance & World Streaming

The current Draw Distance setting primarily adjusts supported object detail distances.

A complete increase in world geometry draw distance requires additional changes to:

- World streaming ranges.
- Object loading and unloading.
- LOD selection and transitions.
- Visibility and culling.
- Streaming memory management.
- Rendering performance.

The goal is to provide optional extended draw distance while preserving original PS2 behavior by default.

### HDR Limitations

The initial HDR implementation supports HDR output and configurable image controls, but further UI-specific work is necessary.

In particular, HUD brightness, pause-menu presentation, and launcher HDR integration require additional improvements.

### High-FPS Limitations

Although many timing issues have been corrected, some systems may still depend on the original PS2 timing model.

Not every mission, cutscene, animation, and particle effect has been individually validated above 30 FPS.

**Recommended baseline:** 60 FPS with a controller using Classic controls.

[**Report a Bug or View Current Issues**](https://github.com/NicoWayne95/lcs-ps2-recomp/issues)

---

## <a id="roadmap"></a>Roadmap

<p align="center">
  <img src="docs/features/roadmap-banner.png" alt="LCS Recomp Roadmap" width="100%">
</p>

Development continues to focus on performance, stability, compatibility, and gradually replacing unnecessary PS2-specific processing with more efficient native PC implementations.

The following items represent development goals rather than confirmed release commitments.

### Rendering & Graphics

- Further improve native rendering accuracy.
- Reduce remaining PS2 Graphics Synthesizer overhead.
- Improve synchronization between native and PS2-oriented rendering paths.
- Continue developing world streaming and LOD handling.
- Implement functional extended world draw distance.
- Improve HDR presentation and UI brightness handling.
- Expand HD texture replacement compatibility.
- Restore optional blue beta police vehicles.

### Performance & Compatibility

- Continue reducing CPU-side rendering overhead.
- Improve performance on older and lower-end hardware.
- Further optimize the Render Worker and GS processing stages.
- Reduce memory transfers and synchronization costs.
- Improve frame-time consistency.
- Expand performance profiling and diagnostics.
- Improve long-session stability.
- Continue testing CPU compatibility across different generations.

### Gameplay & Controls

- Complete native mouse free aim.
- Further refine vehicle camera behavior.
- Implement customizable keyboard bindings.
- Improve motorcycle handling.
- Expand controller compatibility.
- Complete high-FPS timing corrections.
- Expand mission and cutscene compatibility testing.

### In-Game Trainer & Function Key System

A more structured in-game interface is planned to separate gameplay features from graphics settings and development tools.

<details>
<summary><b>View Planned F1–F12 Layout</b></summary>

| Key | Planned Function | Description |
| :--- | :--- | :--- |
| `F1` | Help & Shortcuts | Controls and keyboard shortcuts |
| `F2` | Video & HUD | Graphics, resolution, effects, and HUD settings |
| `F3` | Trainer | Player, weapons, vehicles, world, and original cheats |
| `F4` | Performance | FPS, frame times, and profiling |
| `F5` | Resolution | Cycle through rendering resolutions |
| `F6` | FPS Mode | Cycle through frame-rate limits |
| `F7` | Trails | Toggle original PS2 trails |
| `F8` | Free Camera | Toggle free camera |
| `F9` | HUD | Show or hide the HUD |
| `F10` | Developer Diagnostics | Debugging and world inspection |
| `F11` | Fullscreen | Toggle fullscreen |
| `F12` | Screenshot | Capture gameplay screenshots |

This table describes the planned layout, not a guarantee that every shortcut currently behaves as shown.

</details>

### Launcher Development

Further launcher improvements are planned, including:

- More modern and consistent interface design.
- Improved installation and preparation workflows.
- Better dependency management.
- Clearer download and compilation progress.
- Improved error reporting and recovery options.
- More reliable updates.
- Better separation between launcher and game runtime.

### Long-Term Development

- Cleaner and more accessible public source code.
- Reproducible build processes.
- Expanded technical documentation.
- Community contributions and pull requests.
- More configurable Classic and Enhanced PC experiences.
- Investigation of modding and mod-loader support.
- Potential additional platform support, subject to feasibility.

No specific release dates are promised for individual roadmap items.

---

## <a id="screenshots"></a>Screenshots

### PC Features & Settings

The screenshots below showcase the PC interface, controller configuration, and additional graphics options.

Some screenshots may represent earlier development versions.

<p align="center">
  <a href="docs/features/controls-overview.webp">
    <img src="docs/features/controls-overview.webp" width="32%" alt="LCS Recomp controls overview">
  </a>
  <a href="docs/features/mouse-keyboard-setup.webp">
    <img src="docs/features/mouse-keyboard-setup.webp" width="32%" alt="Mouse and keyboard settings">
  </a>
  <a href="docs/features/display-settings.webp">
    <img src="docs/features/display-settings.webp" width="32%" alt="Graphics and display settings">
  </a>
</p>

<p align="center">
  <a href="docs/FEATURES.md"><strong>View All PC Feature Screenshots</strong></a>
</p>

### Gameplay Screenshots

<p align="center">
  <a href="docs/screenshots/lcpd-helicopter.webp">
    <img src="docs/screenshots/lcpd-helicopter.webp" width="32%" alt="Police helicopter above Liberty City">
  </a>
  <a href="docs/screenshots/sunset-railway.webp">
    <img src="docs/screenshots/sunset-railway.webp" width="32%" alt="Liberty City railway at sunset">
  </a>
  <a href="docs/screenshots/downtown-street.webp">
    <img src="docs/screenshots/downtown-street.webp" width="32%" alt="Downtown Liberty City">
  </a>
</p>

<p align="center">
  <a href="docs/GALLERY.md"><strong>View the Full Screenshot Gallery</strong></a>
</p>

---

## Community & Feedback

Community testing is one of the most valuable ways to improve LCS Recomp.

I'm particularly interested in **real-world performance results**, especially on older or mid-range CPUs and GPUs.

If you're testing the latest version, please consider sharing:

| Information | Example |
| :--- | :--- |
| CPU | AMD Ryzen 5 5600X |
| GPU | NVIDIA RTX 3060 |
| RAM | 16 GB |
| Windows Version | Windows 11 |
| Resolution | 1920×1080 |
| FPS Mode | 60 / 120 / 144 / Uncapped |
| Anti-Aliasing | SMAA / MSAA |
| Average FPS | 65 FPS |
| 1% Low | 42 FPS |
| Test Location | Portland Police Station |
| Issue | Frame drops while driving through traffic |

Useful reports include:

- Average FPS and frame-time consistency.
- CPU and GPU utilization.
- Performance in densely populated areas.
- Stuttering during island transitions.
- Controller compatibility.
- Rendering artifacts.
- Mission and cutscene issues.
- Savegame behavior.
- Long-session stability.

Comparisons with previous versions are particularly helpful when the same location and graphics settings are used.

Screenshots, videos, performance captures, and reproducible steps make technical investigations significantly easier.

<p align="center">
  <a href="https://github.com/NicoWayne95/lcs-ps2-recomp/discussions">
    <img src="https://img.shields.io/badge/Community-Discussions-2878c7?style=for-the-badge&logo=github" alt="GitHub Discussions">
  </a>
  <a href="https://github.com/NicoWayne95/lcs-ps2-recomp/issues">
    <img src="https://img.shields.io/badge/Feedback-Report%20an%20Issue-555555?style=for-the-badge&logo=github" alt="Report an Issue">
  </a>
</p>

Constructive feedback, technical suggestions, and future contributions are always welcome.

---

## <a id="source-code"></a>Source Code & Development

LCS Recomp builds upon existing open-source recompilation projects and includes custom runtime modifications and native PC implementations.

The public releases provide the game preparation resources and source kit needed by the distribution workflow.

The complete reorganized development repository is still being prepared.

### Development Priorities

- Better separation between game runtime and launcher.
- Reproducible build processes.
- Cleaner organization of source code and dependencies.
- Improved build documentation.
- Expanded automated testing.
- More detailed profiling and benchmarking.
- Consistent third-party attribution and licensing.
- Removal of machine-specific paths and unrelated local files.
- Separation of copyrighted original game data from distributable code.

### Open-Source Development

The long-term goal is to make the project easier for other developers to inspect, build, test, and contribute to.

The public source code must remain consistent with the applicable licensing requirements of incorporated third-party projects.

Preparing the reorganized repository does not remove existing source distribution obligations associated with GPL-covered components.

No original game ISOs or copyrighted Rockstar assets are intended to be distributed.

---

## AI-Assisted Development

LCS Recomp is developed with assistance from AI tools, including Claude and a locally running Qwen model.

These tools assist with research, code exploration, implementation, debugging, profiling, and testing.

AI-generated code is not automatically considered correct or production-ready. Changes require technical review, integration, and practical validation.

Project direction, implementation decisions, testing, and release responsibility remain mine.

This is a personal hobby and learning project, and AI-assisted development allows me to explore complex technical systems while continuously improving my understanding.

---

## Credits

LCS Recomp builds upon the work of several open-source projects and their contributors.

Their work provides essential technical foundations for the project.

### PS2Recomp

[**PS2Recomp by ran-j**](https://github.com/ran-j/PS2Recomp)

Provides the static PlayStation 2 recompiler and foundational runtime used by LCS Recomp.

License: GPL-3.0.

### GTA: Vice City Stories Recomp

[**GTA: Vice City Stories Recomp by gothamor**](https://github.com/gothamor/gta-vcs-recomp)

Provides technical reference

License: GPL-3.0.

### HCTP Recomp

[**HCTP Recomp by BigDaddyJRich**](https://github.com/BigDaddyJRich/hctp-recomp)

Provides the foundation for the VU1 recompiler used within LCS Recomp.

License: GPL-3.0.

### Kenney

[**Input Prompts by Kenney**](https://kenney.nl/assets/input-prompts)

Provides controller, keyboard, and mouse icon assets used by the adaptive input prompt system.

License: [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/).

### Third-Party Licenses

All incorporated third-party components remain subject to their applicable licenses and copyright notices.

Additional attribution and licensing information can be found in the `NOTICE.txt` and associated license files included with the distribution.

---

## Legal Notice

LCS Recomp is an unofficial, independent fan development project.

It is not affiliated with, endorsed by, or sponsored by Rockstar Games, Rockstar Leeds, Rockstar North, Take-Two Interactive, Sony Interactive Entertainment, or Microsoft.

Grand Theft Auto, Liberty City Stories, and all associated characters, trademarks, and copyrighted assets belong to their respective owners.

No ownership of the original game or its assets is claimed.

The cover artwork displayed in this repository is unofficial concept artwork. Any age-rating-style symbol shown is not an official rating for this project.

No original game ISOs or copyrighted Rockstar game assets are included with LCS Recomp.

Users must provide their own legally obtained copy of the supported game.

---

<p align="center">
  <strong>Liberty City Stories. Recompiled for PC.</strong>
</p>

<p align="center">
  Developed in my free time, with the goal of preserving and enhancing the original experience.
</p>

<p align="center">
  <strong>See you in Liberty City!</strong>
</p>
