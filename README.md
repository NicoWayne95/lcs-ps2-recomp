# LCS PS2 Recomp

<p align="center">
  <img src="docs/images/cover.png" alt="LCS PS2 Recomp — concept cover artwork" width="430">
</p>

<p align="center"><strong>Experimental PlayStation 2-to-Windows recompilation project</strong></p>

<p align="center">Windows (x64) · C++ · Work in progress</p>

---

## AI-assisted development; a note on transparency

**Yes, this project is developed with AI assistance.** I use Claude and a locally running Qwen 27B model to speed up research, code exploration, implementation, debugging, and testing. Rebuilding the execution and rendering layers needed to run PlayStation 2 software on Windows is a substantial engineering challenge. Given my current experience, doing everything without AI could take me many months, potentially much longer. AI dramatically shortens the time between an idea, a working prototype, and the next test.

**AI assistance does not mean that everything is generated automatically or that technical understanding is optional.** Making a recompilation project work requires understanding how games and their original hardware behave: CPU instructions and memory layouts, vector units, DMA and synchronization, graphics commands and render pipelines, frame timing, audio and input, as well as the interactions between these systems. Progress also depends on interpreting disassembly and logs, isolating bugs, profiling bottlenecks, checking visual and behavioral correctness, and catching regressions. AI-generated code can be incomplete, misleading, or simply wrong; it must be reviewed and tested against actual results.

**I make the engineering decisions.** I choose the project's direction, decide what belongs in the codebase, what should be reworked or removed, and which trade-offs between accuracy, performance, maintainability, and scope make sense. Claude and Qwen help me work faster, but they do not replace my judgment, responsibility, or the hands-on work required to make the project function.

---

## About

**LCS PS2 Recomp** is an independent, unofficial project exploring how the PlayStation 2 version of LCS can run on Windows through static recompilation and a purpose-built runtime.

The goal is to preserve the original game's behavior while investigating modern rendering, input, compatibility, and performance improvements. This is an experimental engineering project, **not a finished game release**.

## Development status

**Status: Active development / experimental prototype.**

Internal development builds have reached interactive open-world gameplay. Rendering accuracy, performance, stability, audio, input, and overall compatibility are still being worked on. Features and results may change as development progresses.

There are currently **no public binaries or game downloads** provided by this repository.

## Technical focus

- PlayStation 2 program recompilation and runtime execution
- CPU, vector-unit, and system-level compatibility
- Graphics rendering and display correctness
- Input and audio integration
- Profiling, debugging, and performance optimization
- Reproducible Windows builds and automated testing where possible

## Long-term goals

Potential improvements include higher rendering resolutions, more flexible controls, improved frame pacing, and additional PC-friendly options. These are **development goals, not promises of available features**.

## Building

A verified, reproducible Windows build guide will be added after the source tree and its dependencies have been reviewed. Build commands and prerequisites will be documented here once confirmed against a clean checkout.

## Game files and redistribution

This project does **not** aim to distribute the original game or any files extracted from it. In particular, the repository must not contain:

- Game disc images or original game executables
- Proprietary game assets (textures, models, audio, movies, and similar content)
- Console BIOS or firmware files
- Proprietary SDKs, encryption keys, or credentials
- Third-party source code without appropriate redistribution rights

Any eventual workflow involving game data must respect applicable law and the rights of the original creators and publishers. Possessing a copy of a game does not, by itself, grant permission to redistribute its contents.

## Project independence

This is an **unofficial, independent fan research and development project**. It is not affiliated with, sponsored by, approved by, or endorsed by the game's rights holders or platform manufacturers.

The cover image is **unofficial concept artwork**, not official packaging or a commercial product announcement. Any depicted age-rating mark is part of the concept illustration and **does not represent a rating awarded to this project**.

## Licensing

A project license has **not yet been selected**. Before any public source-code release, the codebase, generated outputs, and third-party components must be reviewed for provenance, licensing, and redistributability.

Until an appropriate license is added, do not assume that the source code or artwork is licensed for reuse or redistribution.
