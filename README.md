# LCS PS2 Recomp

<p align="center">
  <img src="LCS_recomp2.png" alt="LCS Recomp — unofficial concept artwork" width="430">
</p>

<p align="center"><strong>Experimental PS2 to Windows recompilation project</strong></p>

<p align="center">Windows (x64) DirectX 11 · Work in progress (other platforms are planned)</p> 






## About

**LCS PS2 Recomp** is an independent project exploring native execution of the PlayStation 2 version of *Liberty City Stories* on Windows using recompilation and a custom runtime.

The aim is to make the game playable on PC while preserving the original experience. Optional improvements for modern hardware are also planned, but they should never replace the classic way to play.

This is an experimental project, **not a finished PC port or public game release**.

## AI-assisted development

Yes, AI is used extensively in this project - and I want to be transparent about that.

I work with Claude and a locally running Qwen 27B model to accelerate research, code exploration, implementation, debugging, profiling, and testing. At my current skill level, rebuilding and integrating the systems needed to run a complex console game on Windows could take many months, potentially much longer, without those tools. AI makes it possible to move much faster from a hypothesis to a working change and a real test.

Using AI does not mean the project builds itself or that technical knowledge is unnecessary. The work still requires understanding how the game and its original hardware behave: CPU execution and memory layouts, vector processors, synchronization, rendering commands, graphics pipelines, timing, controller inputs, audio, reverse engineering, and performance bottlenecks. It also requires recognizing incorrect assumptions, reading logs, reproducing bugs, comparing results, and preventing regressions.

AI-generated code can be wrong, inefficient, or incompatible with existing systems. I decide what belongs in the codebase, what gets rewritten or rejected, what trade-offs are acceptable, and what the project should become. AI helps with the workload; engineering decisions, integration, validation, and responsibility remain mine.

## Current status

**Playable internal build - active development.** The game has progressed well beyond a rendering demonstration:

- **Windows / DirectX 11:** The game starts and runs through the DX11 rendering path.
- **Open world:** Exploration, driving and core gameplay interactions work in tested builds.
- **Missions:** Missions can be started and completed in tested scenarios.
- **Save system:** Saving game progress works; broader save/load testing is still planned.
- **Controllers:** Gamepad input is functional, with additional controller features planned.
- **Frame-rate modes:** 30, 60, 120 and Uncapped FPS can be selected.
- **Resolution options:** 1080p, 1440p and 4K options have been implemented.

These are results from internal development. **Full-game compatibility, consistently high frame rates and correct behavior in every mission have not yet been verified.**

### Recent progress - November 2026

- **Rendering:** Significant improvements to world and character rendering. In a documented test, 99.6% of the relevant character-rendering packets used an optimized native path.
- **Verification:** 451,589 fast-path packets matched the reference execution in a dedicated verification run. This does not establish complete graphical accuracy across the game.
- **Performance:** A tested segment in the 120 FPS mode averaged approximately 89 FPS. Stable 120 FPS remains an optimization goal, not a current guarantee.
- **Stuttering:** A logging-related bottleneck behind repeated driving hitches was addressed. The recurring hitches were no longer experienced in a subsequent long test, although isolated frame-time spikes remain.
- **Timing and presentation:** Higher-frame-rate behavior, smooth frame pacing and variable-refresh-rate presentation still need more testing and refinement.

## Classic and Enhanced Experiences

The project aims to offer two ways to play. Players should be able to enjoy the original console experience or enable optional improvements designed for modern PCs.

### Classic PS2 Experience

The goal is to preserve the original gameplay and presentation as closely as possible.

**Already available**
- Original 4:3 aspect ratio
- Classic 30 FPS mode
- Functional controller input

**In development**
- Accurate gameplay behavior and timing
- Original camera behavior and vehicle handling
- Improved visual accuracy
- Traditional controller layout

**Planned**
- Pressure-sensitive button support for compatible controllers
- Further improvements to the original console experience

### Enhanced PC Experience

The Enhanced experience focuses on flexibility, higher performance and modern PC features.

**Already available**
- Selectable 30, 60, 120 and Uncapped FPS modes
- 1080p, 1440p and 4K resolution options
- DirectX 11 rendering

**In development**
- Improved frame pacing and stability
- High refresh rate optimization
- More consistent performance at higher frame rates
- Modern controller support

**Planned**
- Increased draw distance
- Optional graphics enhancements
- Modern controller layouts
- Optional free vehicle camera similar to San Andreas
- Additional display and graphics settings

Some enhancements may require changes to the original game behavior. These will remain optional whenever technically possible.


## Development Roadmap

Development is ongoing. The following milestones show what has already been achieved and what is planned for future updates.




Planned features may change depending on technical feasibility and development progress.


### What comes next

The immediate priorities are **gameplay correctness and performance**, rather than adding new features before the existing ones are reliable:

1. Check game speed, physics, animations and mission timing at 60, 120 and uncapped frame rates.
2. Improve frame pacing, responsiveness and performance in busy scenes.
3. Expand testing of missions, save/load behavior, cutscenes, audio and long sessions.
4. Resolve remaining rendering issues and improve controller compatibility.
5. Verify a clean, reproducible Windows build and review third-party dependencies before any release.

A **limited closed beta** may be considered later to gather bug reports and hardware feedback. No beta or public release date has been announced.

## Building and downloads

There is currently **no public game download or verified clean-checkout build guide**. Installation steps and prerequisites will be documented after the source tree and dependencies have been audited.

This repository is not intended to distribute original game disc images, executables, audio, textures, models, movies, console firmware or other proprietary game files. Any generated or recompiled output must also be checked before redistribution.

## Credits

### PS2Recomp

This project uses parts of **[PS2Recomp by ran-j](https://github.com/ran-j/PS2Recomp)**, an experimental PlayStation 2 static recompiler and runtime project. Thanks to its developers and contributors for the work that made this possible.

PS2Recomp is published under the **[GPL-3.0 license](https://github.com/ran-j/PS2Recomp/blob/main/LICENSE)**. Attribution alone does not satisfy all license obligations; before distributing source code or binaries, the incorporated components, modifications and required notices must be reviewed.

Additional third-party acknowledgements will be added once the codebase audit is complete.

## Legal notice

**LCS PS2 Recomp is an unofficial, independent fan development project.** It is not affiliated with, endorsed by or sponsored by the original game's rights holders or any console manufacturer.

No rights to the original game, its trademarks, characters or assets are claimed. The cover displayed here is unofficial concept artwork, not official packaging; any age-rating-style symbol visible in the illustration is **not an official rating for this project**. The artwork should be reviewed before public distribution.

The project does not grant permission to redistribute copyrighted game files. A license for original project contributions has not yet been selected; third-party components remain subject to their own licenses.
