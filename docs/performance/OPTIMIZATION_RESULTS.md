# Optimierungsergebnisse

## D3D11-Pipeline-Bindings

Der A/B-Kandidat überspringt die acht D3D11-Bindings, wenn dieselbe Pipeline bereits aktiv ist. Present und ImGui invalidieren den vorhandenen Cache. Der Blend-Konstantenpfad aktualisiert den gebundenen State unabhängig. A/B-Schalter LCS_D3D11_REUSE_PIPELINE=0. Das Release-Kompilat liegt unter build/game/out, der Referenzbuild unter bin/current.

Build erfolgreich. Messbarer FPS-/P99-Gewinn und visuelle Regressionen sind noch offen. Zuerst dieselbe Szene mit Schalter 0 und 1 vergleichen. Bei Bildabweichung oder ausbleibendem Gewinn den Kandidaten nicht übernehmen.

## Compiler und größere Pfade

Baseline ohne LTO oder PGO bestätigt. ThinLTO, Full LTO, PGO und PGO mit ThinLTO benötigen separate reproduzierbare Szenenläufe. Für native DMA-/RSL-Änderungen zuerst Fallback-Anteile, Snapshot-Bytes und Kopier-Stacks messen. GPU-Skinning, Instancing und weitere GS-Semantik bleiben Folgearbeiten nach der Messung.

## Host-Ausgabe bei 60 und 180 Hz

Der SDL-Host-Loop wartet standardmäßig auf 60 Hz, unabhängig vom Uncapped-Modus der Spiellogik. CapFrameX zeigt im gemeldeten Lauf 60 DXGI-FPS, 43 FPS im 1%-Low und momentane GPU-Auslastung von 100 Prozent. Der Testbuild nimmt ein explizites Host-Ziel an. Die Spielphysik und das 60-Hz-Guest-VBlank werden durch diese Option nicht geändert. Das Host-Ziel misst keine Anzahl neuer Spielbilder, da der Presenter auch das zuletzt verfügbare Bild ausgeben kann.

Für den isolierten Vergleich Start-Profiling.ps1 -Variant PipelineOff -HostHz 60 und anschließend -HostHz 180 verwenden. Der Kandidat ist gebaut. Present-Intervalle, tatsächliche Display-Intervalle, Renderlast und Wiederholungen im Spiel sind noch zu messen.

### Fahrt mit angeforderter Host-Ausgabe von 180 Hz

PipelineOff, Lauf 4c92684f8fa94adf8343b5ce44b4bc5d. Nach 30 s Warm-up 7.456 CPU-Present-Aufrufe, 107,5 FPS, P99 22,26 ms und 43,4 FPS im 1%-Low. Bei 12 bis unter 20 ms Intervall dauerte der vorangegangene Present-Aufruf im Median 15,17 ms. Bei mindestens 20 ms waren es 20,70 ms. Game-Timer-Intervalle hatten separat P99 15,27 ms. Der Screenshot mit kurzzeitig 180 DXGI-FPS und der längere CPU-Present-Trace beschreiben unterschiedliche Messfenster.

Der gespeicherte FRAME SYNC-Wert ist 1. Das Startlog meldet Tearing vor dem Laden der Einstellungen. Der neue Profiling-Build hält den gewählten Present-Modus auch nach dem Laden der Einstellungen fest. -SpikeMs 12 erfasst Host-Phasen, Game-/Render-Phasen und Profiler-Samples als separaten Diagnoselauf. Zuerst PipelineOff mit Tearing fest vorgegeben fahren, danach PipelineOn mit denselben Einstellungen.

Der zweite PipelineOff-Lauf mit festem Tearing bei 180 Hz liefert nach 30 s Warm-up 10.581 CPU-Present-Intervalle. Mittel 153,65 FPS, P95 16,67 ms, P99 17,89 ms, Maximum 29,12 ms, 1%-Low 52,87 FPS. 63,68 Prozent der Intervalle lagen innerhalb 6 ms. Der Vergleich zur vorigen Fahrt ist indikativ. Die Fahrten waren nicht kontrolliert identisch und es liegen keine ETW-Anzeigeintervalle vor.

## Direkte LCS-Draw-Items für Welt und Modelle

Der LCS-Pfad hatte bereits bestätigte Draw Items mit unveränderlichen Paketkopien. Neu ist der direkte Index im Draw der jeweiligen DMA-Kette und ein kurzer Replay-Pfad für Welt und beide Modelltypen. Er akzeptiert nur ein bestätigtes Paket mit identischem Hash, passenden VIF-Modus- und Zyklusregistern und replayfähiger Aufnahme. Der bestehende Pfad verarbeitet Abweichungen mit der Paketkopie. Das Replay führt weiterhin die erforderlichen VIF-/VU1-Operationen aus und kann die vorhandenen nativen Mesh-Pfade nutzen. Das ist kein vollständiger GPU-Bypass.

AVX1-Build erfolgreich. Windows PowerShell 5.1 hat die Syntax des Profiling-Skripts geprüft. Der Testbuild ist unter build/game/out. Der installierte Spielbuild bleibt die Referenz. Mit -DirectItemsOff lässt sich der neue Replay-Kurzpfad im selben Testbuild deaktivieren. Der direkte Item-Index ist in beiden Läufen aktiv. -NativeAudit zählt direkte Replays je Draw-Typ. Für belastbare Frametime-Messungen separat ohne diese zusätzliche Diagnostik fahren.

### Erste Fahrt mit direkten Draw Items

Gleich langes Fenster von Sekunde 30 bis 75, jeweils 180 Hz, Tearing, PipelineOff. Vorheriger Build gegen aktuellen Kandidaten. Durchschnitt 147,1 gegen 159,3 CPU-Present-FPS, P95 17,03 gegen 12,10 ms, P99 18,22 gegen 18,24 ms, 1%-Low 51,6 gegen 51,5 FPS, Anteil bis 6 ms 61,5 gegen 66,4 Prozent. Das aktuelle run.json und der Build-Hash stimmen überein. NativeAudit war aus. Trefferzahlen und der kausale Gewinn sind ohne einen Gegenlauf derselben EXE mit DirectItemsOff offen. Der langsamste Prozentbereich ist in diesem Vergleich praktisch gleich.

### Run 1 mit GPU-Audit

Nach 30 s Warm-up 2.570 CPU-Present-Intervalle, P99 33,64 ms, 1%-Low 27,29 FPS. Dieser Lauf aktiviert D3D11-Query-Paare pro Render-Marker und ist wegen Diagnose-Overhead kein sauberer Vergleich mit der Fahrt ohne GpuAudit. In phases.csv nach Sekunde 30 ist die Game-Phase bei 4,49 ms Median / 6,33 ms P99 und die Render-Phase bei 3,14 / 6,28 ms. Game-Frame-P99 12,23 ms. Rund 19.926 DMA-Tags im Median, 34.924 im P99. Textur-Decode P99 377 µs, Texturerstellung P99 1 µs. Diese Zähler belegen keine dominanten Upload-Spitzen in diesem Lauf. Thread-CPU-Samples ohne GPU-Audit sind nötig, um den stark ausgelasteten Kern einem Thread und konkreten Call-Stacks zuzuordnen.

### Run 2 mit CPU-Sampling

PipelineOff, direkte Items an, festes Tearing, 180 Hz und kein GPU-Audit. Ab Sekunde 30 ergeben 4.732 CPU-Present-Intervalle 179,9 FPS, P99 8,39 ms, 1%-Low 106,9 FPS und 67,7 Prozent bis 6 ms. Die Game-Frame-Phasen haben P99 11,46 ms gesamt, 6,67 ms Game und 5,26 ms Render. Rund 20.045 DMA-Tags im Median. RenderWorker-memcpy erreicht 19 bis 23 Prozent aller Samples in den aktiveren Profilfenstern. GameThread und GsStage sind zeitweise ebenfalls stark ausgelastet, große Host-Anteile bleiben unsymbolisiert. Die 0-Prozent-Zuordnung für generierten Code ist damit nicht belastbar. Die nächsten Kopieroptimierungen brauchen Host-Call-Stacks und Byte-Mengen pro Aufrufer. Die schlechteren Werte des GPU-Audits sind wegen unterschiedlicher Instrumentierung kein direkter A/B-Vergleich.
