<p align="center">
  <img src="docs/images/LCS_recomp2.png" alt="LCS Recomp — unofficial concept cover art" width="440">
</p><h1 align="center">LCS PS2 Recomp</h1><p align="center">
  <strong>An experimental project bringing the PlayStation 2 version of LCS to Windows through recompilation.</strong><br>
  DirectX 11 · Windows · Playable internal prototype · Active development
</p><p align="center">
  <a href="#current-development-status">Current Status</a> ·
  <a href="#development-roadmap">Roadmap</a> ·
  <a href="#classic-and-enhanced-experiences">Classic & Enhanced</a> ·
  <a href="#ai-assisted-development">AI Transparency</a> ·
  <a href="#credits-and-third-party-software">Credits</a>
</p>[!IMPORTANT]
This is an unofficial, experimental project — not a finished PC release. The internal build is playable, but full-game compatibility, accurate behavior at every frame rate, and public distribution have not been established. No release date is announced.

About the project
LCS PS2 Recomp explores running the PlayStation 2 release of Liberty City Stories on Windows using static recompilation, a host runtime, and DirectX 11 graphics rendering.

The goal is to offer two complementary experiences: a faithful classic mode that preserves the original console feel, and an optional enhanced PC mode with more flexibility for modern hardware. Recompilation is not the same as rebuilding every part of a game from scratch; the challenge is integrating and accurately reproducing the execution, rendering, input, audio, timing, and memory behavior that the game depends on.

The project is developed independently and is not affiliated with or endorsed by the game's rights holders or the original platform manufacturer.
Current development status
Last documented development update: November 15, 2026. The information below reflects internal testing, not a public release or a guarantee that every mission or system works in all circumstances.
Area	Status	What works or has been observed
Native Windows execution	✅ Playable foundation	The game starts and runs on Windows using a DirectX 11 renderer.
Open-world gameplay	✅ Working in tested builds	World exploration, vehicles, controller input, and gameplay interaction.
Missions	✅ Working in tested cases	Missions can be started and completed. A complete mission-by-mission validation is still pending.
Saving	✅ Working in tested cases	In-game saving functions; extended save/load and restart tests remain part of compatibility work.
World and character rendering	🟢 Advanced	Major renderer improvements and optimized native character-processing paths.
Frame-rate modes	✅ Selectable	30 / 60 / 120 / Uncapped FPS modes have been implemented. They are not a guarantee of sustained frame rate or correct timing in every situation.
Higher resolutions	✅ Implemented	1080p / 1440p / 4K options are implemented; broad hardware and visual-accuracy validation remains ongoing.
Streaming-related stutters	🟢 Major improvement	Repeated driving hitches were eliminated in a documented test after addressing synchronous logging; occasional spikes remain to be studied.
Overall compatibility	🟡 In progress	Additional work on missions, cutscenes, sound, long sessions, timing, and edge cases.
Public release	⚪ Not announced	The build and its dependencies require further technical, legal, and distribution review.


Recent technical milestones
- Native character processing: The tested lit/skinning path was validated against the interpreter. In one production run, 99.6% of lit packets used the native path, while the remaining cases fell back to the interpreter when needed.
- Verification: 451,589 fast-path packets were reported as exact in a dedicated verification run, with no incorrect packets in the tested fast-path comparison. This does not imply all rendering is bit-perfect; specific non-output state differences are tracked separately.
- Performance: A measured section using the 120 FPS mode averaged approximately 89 FPS. The mode works, but stable 120 FPS was not demonstrated in that run; performance varies by scene and hardware.
- Reduced hitching: A logging bottleneck responsible for severe recurring driving stutters was addressed. In a subsequent long test, the previously noticed recurring hitches were absent, although a smaller number of other spikes remained.
- Loading work: A native decompression path has been integrated. Additional verification during island transitions remains outstanding.
- Presentation and frame pacing: A new variable-refresh-rate-friendly presentation path has been built, but in-game visual verification remains outstanding.
[!NOTE]
Performance figures are results from individual internal test sessions, not benchmarks or minimum-system-requirement claims. No stable 120 FPS target or release-date commitment is implied.

AI-assisted development
Yes, AI is used extensively in this project — and I want to be transparent about that.

I work with Claude and a locally running Qwen 27B model to accelerate research, code exploration, implementation, debugging, profiling, and testing. At my current skill level, rebuilding and integrating the systems needed to run a complex console game on Windows could take many months, potentially much longer, without those tools. AI makes it possible to move much faster from a hypothesis to a working change and a real test.

Using AI does not mean the project builds itself or that technical knowledge is unnecessary. The work still requires understanding how the game and its original hardware behave: CPU execution and memory layouts, vector processors, synchronization, rendering commands, graphics pipelines, timing, controller inputs, audio, reverse engineering, and performance bottlenecks. It also requires recognizing incorrect assumptions, reading logs, reproducing bugs, comparing results, and preventing regressions.

AI-generated code can be wrong, inefficient, or incompatible with existing systems. I decide what belongs in the codebase, what gets rewritten or rejected, what trade-offs are acceptable, and what the project should become. AI helps with the workload; engineering decisions, integration, validation, and responsibility remain mine.
Classic and Enhanced Experiences
The project is intended to give players a choice. The Classic experience prioritizes the original console feel; the optional Enhanced experience adds modern PC conveniences where they can be implemented without breaking the game.
Classic experience	Enhanced PC experience
4:3 presentation — available	1080p, 1440p, 4K options — implemented
30 FPS mode — selectable	30 / 60 / 120 / Uncapped modes — selectable; stable high-FPS behavior still being validated
Original-style controls and camera — refinement planned	Modern controller options and free-moving vehicle camera — planned/exploratory
Pressure-sensitive buttons on compatible hardware — planned	Extended draw distance and visual enhancements — feasibility-dependent
Focus on authentic timing and presentation	Focus on player choice, customization, and smooth high-refresh-rate output


Neither experience is considered complete or fully verified across the entire game.
Development roadmap
The phases below describe the current direction. Completed means the stated milestone was achieved in internal development — it does not mean every related feature has been fully tested across the entire game.
#	Milestone	Status
01	Playable PC Experience	✅ Foundation completed
02	Graphics & Rendering	🟢 Advanced; improving
03	Performance & Stability	🟡 In progress
04	Game Compatibility & Polish	🟡 In progress
05	Classic PS2 Experience	🟣 Partially available / planned
06	Enhanced PC Experience	🟣 Partially implemented / planned
07	Modding & Customization	🔵 Exploratory
08	Potential Closed Beta	🔵 Under consideration
09	Public Release Preparation	⚪ Future milestone


01 — Playable PC Experience · Foundation completed
- [x] Run the game on Windows with DirectX 11
- [x] Reach and explore the open world
- [x] Start and complete missions in tested scenarios
- [x] Save game progress in tested scenarios
- [x] Drive vehicles and interact with gameplay systems
- [x] Use controller input
Achieved: The project has moved beyond rendering a scene: the internal build supports meaningful, mission-based gameplay.
02 — Graphics & Rendering · Advanced, improvements ongoing
- [x] Render the 3D world, characters, vehicles, and environments
- [x] Implement major rendering optimizations
- [x] Improve native character-processing performance and verification
- [ ] Continue fixing visual inaccuracies and rendering edge cases
- [ ] Improve demanding scenes, effects, and consistency
03 — Performance & Stability · In progress
- [x] Implement selectable 30 / 60 / 120 / Uncapped FPS modes
- [x] Resolve the main recurring driving-related logging stalls seen in testing
- [ ] Improve frame pacing and high-refresh-rate smoothness
- [ ] Validate game speed, animation, physics, and mission timing at higher frame rates
- [ ] Reduce remaining slowdowns, spikes, and crashes
- [ ] Test variable-refresh-rate presentation and optimize demanding scenes
04 — Game Compatibility & Polish · In progress
- [x] Confirm working mission starts and completions in tested cases
- [x] Confirm that the in-game save system works in tested cases
- [ ] Expand mission-by-mission testing
- [ ] Test save/load persistence, reloads, and different progression states
- [ ] Validate audio, cutscenes, transitions, and long play sessions
- [ ] Fix remaining gameplay, compatibility, and stability issues
05 — Classic PS2 Experience · Partially available / planned
Preserve the original game feel as closely as possible.
- [x] Classic 4:3 display option
- [x] Selectable 30 FPS mode
- [ ] Verify faithful gameplay, camera, handling, and timing throughout the game
- [ ] Refine original-style controller mappings
- [ ] Add pressure-sensitive button support for compatible controllers where possible
- [ ] Preserve the classic look without forcing modern visual enhancements
06 — Enhanced PC Experience · Partially implemented / planned
Offer optional improvements without replacing the classic mode.
- [x] Selectable 30 / 60 / 120 / Uncapped FPS modes
- [x] Implemented 1080p / 1440p / 4K resolution options
- [ ] Improve sustained high-frame-rate performance and timing correctness
- [ ] Explore increased draw distance, subject to technical feasibility
- [ ] Add optional visual enhancements and PC-specific settings
- [ ] Expand modern controller layouts
- [ ] Explore an optional free-moving vehicle camera
- [ ] Improve high-refresh-rate support and frame pacing
07 — Modding & Customization · Exploratory
- [ ] Investigate a mod loader and its technical and legal feasibility
- [ ] Explore support for optional custom content and gameplay modifications
- [ ] Expand configuration options and player-controlled settings
- [ ] Keep Classic and Enhanced options clearly separated
08 — Potential Closed Beta · Under consideration
- [ ] Assess whether a limited, lawful distribution model is feasible
- [ ] If appropriate, invite selected testers to report issues
- [ ] Collect mission, save/load, controller, audio, and graphics bug reports
- [ ] Compare stability and performance across different PC hardware
- [ ] Use reproducible reports to prioritize fixes
A beta is not promised or scheduled. Any testing release depends on technical readiness, rights, licensing, and distribution requirements. No original game content would be distributed by this project.
09 — Public Release Preparation · Future milestone
- [ ] Validate the complete game and address critical failures
- [ ] Review and clean the source tree and Git history for protected or sensitive content
- [ ] Audit third-party code, generated code, and license obligations
- [ ] Prepare reproducible builds and accurate setup documentation
- [ ] Decide what can lawfully be shared and in what form
- [ ] Consider an initial public development release only if feasible
No public release date has been announced.
What comes next
Short-term work is focused on compatibility, stable frame pacing, accurate gameplay timing, and continued renderer optimization rather than adding a long list of untested enhancements.
1. Timing and smoothness: Check 60 and 120 FPS behavior, presentation timing, refresh-rate synchronization, and remaining frame spikes.
2. Full-game compatibility: Test more missions, save/load paths, cutscenes, audio, and long sessions.
3. Rendering accuracy: Address remaining edge cases and the performance of densely populated scenes.
4. Classic and Enhanced modes: Refine controllers and presentation, then evaluate optional visual and camera improvements.
5. Repository readiness: Verify dependencies, licensing, generated outputs, and a clean reproducible build before sharing source or downloads.
Building and downloads
There is no verified public build procedure or downloadable game release at this time. A clean-checkout Windows build guide will be added after the repository structure and third-party dependencies have been audited. Please do not assume development-only paths, scripts, or binaries are suitable for public use.

The project will not distribute original game ISOs, executables, firmware, or game assets. Any future user-side setup instructions will be reviewed for legality and accuracy before publication.
Credits and third-party software
PS2Recomp
This project uses components and tooling from PS2Recomp by ran-j, an experimental PlayStation 2 static recompiler and runtime project. Many thanks to the upstream developers and contributors for making this work available.

License note: The upstream PS2Recomp repository identifies its license as GPL-3.0. Including attribution here is not a substitute for complying with the applicable license and its requirements. Before distributing this project's source or binaries, the exact incorporated code, changes, upstream notices, and license obligations must be checked and preserved as required.

Other third-party components and contributors will be documented after the source and dependency audit.
Legal and project independence
This is an unofficial, independent fan-made research and development project. It is not affiliated with, sponsored by, approved by, or endorsed by the original game's developers, publishers, trademark holders, or console manufacturers.
- No original game files, extracted assets, commercial game binaries, proprietary firmware, or restricted development kits are intended to be distributed through this repository.
- Recompiled or generated outputs must also be reviewed before any public distribution; generated code is not automatically free of copyright or licensing restrictions.
- The cover art is unofficial fan-made concept artwork, not official packaging or a product release. Any age-rating-style symbol included in an illustration does not indicate an official rating for this project.
- Mention of product or platform names is for identification and technical context only; no affiliation is claimed.
- No rights to the original game, its characters, trademarks, or other protected material are claimed.
License status
A standalone license for original project contributions has not yet been selected. This does not override the license terms of incorporated third-party code. The repository and its complete Git history must be reviewed before any public release or reuse permissions are offered.
<p align="center"><sub>Experimental project · AI-assisted development · Work in progress · Plans may change</sub></p>
