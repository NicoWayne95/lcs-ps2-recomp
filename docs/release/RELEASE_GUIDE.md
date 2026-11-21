# Lokale Release-Erstellung

`./Build-Release.ps1 -Checkpoint <freigegebener-Quellstand>` erstellt einen eindeutigen Staging-Ordner. Der Checkpoint ist die vom Aufrufer dokumentierte Freigabereferenz, das Kit entsteht aus dem aktuellen lokalen `game/`. `kit.json` erfasst Dateiliste und SHA-256. `build-info.json` und `SHA256SUMS.txt` beschreiben den Kandidaten.

`-BuildLauncher` baut den Launcher vor dem Packaging. `-Installer` fügt das Inno-Setup-Paket desselben Kits hinzu. `-Sign` erzeugt mit dem bestehenden lokalen Schlüssel das bekannte signierte Manifestformat. Signierte Artefakte bleiben im Staging-Ordner. Ein optionaler Installer im Manifest beschreibt ein Launcher-Update, reine Runtime-Updates benötigen nur das Kit. Die Versionen und Mindestversion vor einem späteren Release getrennt festlegen.

`./Test-Release.ps1 -PortableZip <zip>` prüft die Dateiliste und Hashes. Mit `-Iso <iso>` baut der Launcher zusätzlich aus einer unabhängigen ISO-Kopie. Move nach latest, Archivierung und Verteilung erfolgen nach Freigabe.

