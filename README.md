LCS PS2 Recomp
<p align="center">
  <img src="docs/images/LCS_recomp2.png" alt="LCS Recomp — unofficial concept artwork" width="430">
</p><p align="center"><strong>Experimental PlayStation 2-to-Windows recompilation project</strong></p><p align="center">Windows (x64) · C++ · DirectX 11 · Work in progress</p>About
LCS PS2 Recomp is an independent project exploring native execution of the PlayStation 2 version of Liberty City Stories on Windows using recompilation and a custom runtime.

The aim is to make the game playable on PC while preserving the original experience. Optional improvements for modern hardware are also planned, but they should never replace the classic way to play.

This is an experimental project, not a finished PC port or public game release.
Current status
Playable internal build — active development. The game has progressed well beyond a rendering demonstration:
- Windows / DirectX 11: The game starts and runs through the DX11 rendering path.
- Open world: Exploration, driving and core gameplay interactions work in tested builds.
- Missions: Missions can be started and completed in tested scenarios.
- Save system: Saving game progress works; broader save/load testing is still planned.
- Controllers: Gamepad input is functional, with additional controller features planned.
- Frame-rate modes: 30, 60, 120 and Uncapped FPS can be selected.
- Resolution options: 1080p, 1440p and 4K options have been implemented.
These are results from internal development. Full-game compatibility, consistently high frame rates and correct behavior in every mission have not yet been verified.
Recent progress — November 2026
- Rendering: Significant improvements to world and character rendering. In a documented test, 99.6% of the relevant character-rendering packets used an optimized native path.
- Verification: 451,589 fast-path packets matched the reference execution in a dedicated verification run. This does not establish complete graphical accuracy across the game.
- Performance: A tested segment in the 120 FPS mode averaged approximately 89 FPS. Stable 120 FPS remains an optimization goal, not a current guarantee.
- Stuttering: A logging-related bottleneck behind repeated driving hitches was addressed. The recurring hitches were no longer experienced in a subsequent long test, although isolated frame-time spikes remain.
- Timing and presentation: Higher-frame-rate behavior, smooth frame pacing and variable-refresh-rate presentation still need more testing and refinement.
Classic and Enhanced experiences
Two ways to play are planned. Features marked planned or exploratory are not commitments to a release date.
Classic PS2 experience	Enhanced PC experience
Original 4:3 presentation — available	1080p, 1440p and 4K options — implemented
Classic 30 FPS mode — selectable	30 / 60 / 120 / Uncapped modes — selectable; higher frame-rate behavior is still being validated
Original-style visuals, gameplay and camera — ongoing refinement	Optional visual improvements and greater draw distance — planned, subject to feasibility
Traditional controller layout — being refined	Modern controller layouts — further support planned
Pressure-sensitive buttons on compatible controllers — planned	Optional free-moving vehicle camera — planned
Faithful game timing and handling — ongoing testing	Additional display, performance and customization settings — planned


Development roadmap
The roadmap reflects the state of the project, not a promised release schedule. A completed milestone means its core objective has been reached in internal builds, not that every related system is free of bugs.
Phase	Milestone	Status	Focus
01	Playable PC foundation	Completed	Windows execution, open-world gameplay, missions, saving and controller input
02	Graphics and rendering	Advanced	Improve visual accuracy, characters, effects and demanding scenes
03	Performance and stability	In progress	Smooth frame pacing, higher FPS, fewer stalls and crashes
04	Game compatibility and polish	In progress	More missions, saves, audio, cutscenes and longer play sessions
05	Classic PS2 experience	Partially available	Authentic presentation, controls and pressure-sensitive buttons
06	Enhanced PC experience	Partially implemented	Higher resolutions, FPS options and optional modern features
07	Modding and customization	Exploratory	Investigate mod loading, configuration and custom content support
08	Possible closed beta	Under consideration	Collect bug reports and test across a broader range of PCs
09	Public release preparation	Future goal	Clean builds, documentation, licensing and final compatibility review


What comes next
The immediate priorities are gameplay correctness and performance, rather than adding new features before the existing ones are reliable:
1. Check game speed, physics, animations and mission timing at 60, 120 and uncapped frame rates.
2. Improve frame pacing, responsiveness and performance in busy scenes.
3. Expand testing of missions, save/load behavior, cutscenes, audio and long sessions.
4. Resolve remaining rendering issues and improve controller compatibility.
5. Verify a clean, reproducible Windows build and review third-party dependencies before any release.
A limited closed beta may be considered later to gather bug reports and hardware feedback. No beta or public release date has been announced.
Building and downloads
There is currently no public game download or verified clean-checkout build guide. Installation steps and prerequisites will be documented after the source tree and dependencies have been audited.

This repository is not intended to distribute original game disc images, executables, audio, textures, models, movies, console firmware or other proprietary game files. Any generated or recompiled output must also be checked before redistribution.
AI-assisted development
I use Claude and a locally running Qwen 27B model to help with research, code analysis, implementation and debugging. They make it possible to explore and test ideas much faster than I could working alone.

That does not mean the project builds itself. Working on a PS2 recompilation project still requires understanding hardware behavior, memory, rendering, timing, input and how game systems interact. AI-generated suggestions can be wrong, so results have to be reviewed and tested. I decide what goes into the project, what gets changed and what does not belong.
Credits
PS2Recomp
This project uses parts of PS2Recomp by ran-j, an experimental PlayStation 2 static recompiler and runtime project. Thanks to its developers and contributors for the work that made this possible.

PS2Recomp is published under the GPL-3.0 license. Attribution alone does not satisfy all license obligations; before distributing source code or binaries, the incorporated components, modifications and required notices must be reviewed.

Additional third-party acknowledgements will be added once the codebase audit is complete.
Legal notice
LCS PS2 Recomp is an unofficial, independent fan development project. It is not affiliated with, endorsed by or sponsored by the original game's rights holders or any console manufacturer.

No rights to the original game, its trademarks, characters or assets are claimed. The cover displayed here is unofficial concept artwork, not official packaging; any age-rating-style symbol visible in the illustration is not an official rating for this project. The artwork should be reviewed before public distribution.

The project does not grant permission to redistribute copyrighted game files. A license for original project contributions has not yet been selected; third-party components remain subject to their own licenses.
