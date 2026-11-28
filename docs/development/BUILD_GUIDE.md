# Entwicklungsbuilds

PowerShell 7, Git, CMake 4.4.4, Ninja, Python und der lokale LLVM-MinGW 20261020 aus `tools/toolchains/` für das Spiel. `./Build-Game.ps1 -Fresh` erzeugt die CMake-Konfiguration neu. Folgeläufe verwenden den Ninja-Baum wieder. Die Ausgabe liegt unter `build/game/out/`.

Für den Launcher Node/npm, Rust mit MSVC, VS Build Tools und Windows-SDK verwenden. `./Build-Launcher.ps1` installiert gesperrte JavaScript-Abhängigkeiten. Bei bereits vorhandenen, zum Lockfile passenden Abhängigkeiten funktioniert `-UseInstalledDependencies`. Cargo-Artefakte liegen in `build/launcher/`, Frontend-Ausgaben in `launcher/dist/`. Für den Start ist WebView2 erforderlich.

Bei laufendem Spiel den Build erst nach dem Beenden starten. Neue Build-Artefakte zuerst separat testen. Die kopierten Startversionen liegen unter `bin/`.

Nach Funktionsprüfung übernimmt der jeweilige Build-Aufruf mit `-Install` EXE und Build-Artefakte in den Startordner. Er sichert ersetzte Dateien unter `backups/builds/`. ISO, Einstellungen und Spielstände werden bei der Übernahme nicht berührt.
Die früheren Start-LCS-Varianten sind historische Skripte mit den damaligen Pfaden. Für neue Lauf- und Profilingtests die Skripte im Projektstamm verwenden.
