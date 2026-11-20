# Baseline und Frametime

| Historischer Szenariowert | Einfach | Komplex | Extrem |
|---|---:|---:|---:|
| Game FPS | 180 | 83,1 | 50 |
| Frametime (ms) | 5,6 | 12 | 20 |
| Game (ms) | 2,2 | 7,3 | 15,7 |
| Renderer (ms) | 3,3 | 4,7 | 4,1 |
| Draws | 1.331 | 4.775 | 5.632 |
| Vertices | 70.000 | 259.000 | 426.000 |
| DMA-Tags | 7.028 | 35.734 | 30.763 |

Diese Werte stammen aus dem Auftrag. Das archivierte perf_20261120_004223.csv enthält nur drei Samples mit 0,496, 0,684 und 100,394 ms. Es reicht für keine Szenenbaseline. Der letzte dieser Einträge enthält 100,116 ms Renderzeit.

## Messprotokoll

- Für jedes Szenario denselben Save, dieselbe Kameraposition und Grafikoptionen verwenden. Stationäre Last und Fahrt mit Streaming getrennt aufzeichnen. Mindestens 15 s Warm-up und 60 s Messfenster je Durchlauf. Zweimal pro Variante in wechselnder Reihenfolge.
- Uncapped für CPU-/GPU-Headroom messen. Separat bei stabiler Zielframerate den Anteil der Anzeigeintervalle im Zeitbudget messen. Bei 120 FPS liegt das Budget bei 8,33 ms plus vorab festgelegter Messtoleranz. Ziel sind mindestens 99 % im Budget und ein P99 innerhalb derselben Grenze.
- Timing-CSV für Game, Render, Wait, VBlank, DMA und Uploads auswerten. Flip-CSV und die CPU-Present-Aufrufe getrennt zählen. Tatsächliche Anzeigeintervalle, Drops und GPU-Zeiten benötigen einen DXGI-/ETW-Trace auf dem Testrechner. CPU-Present-Dauer allein misst keine GPU-Ausführung.
- Für Hotspots Host-IP und Call-Stacks der passenden EXE/PDB erfassen. Guest-PC ist bei Lean PC eine zuletzt beobachtete Position. Kopierkosten nach Aufrufer, Länge und Thread aggregieren.
- Die 180 FPS separat mit VSync aus und an sowie fixem Refresh testen. Korrelation von Wait, Flip und Present zeigt, an welcher Stufe das Plateau entsteht.

Beispiel für vorhandene Traces

```powershell
python tools/frametime_report.py <presents.csv> --skip-seconds 15 --duration 60 --budget-ms 8.5
```

Start-Profiling.ps1 -Variant PipelineOff und PipelineOn verwenden dieselbe Kandidaten-EXE und den vorhandenen Spielordner. run.json hält Variante und EXE-Hash fest. Für reine Frametime-Vergleiche den Sampling-Profiler ausgeschaltet lassen. Mit -SpikeMs werden Sample-Profiling und Spike-Trace separat aktiviert.

## Testszene Polizeistation neben dem Krankenhaus

Der Spieltest meldet hier Einbrüche unter 100 FPS. Die genaue Frametime-Verteilung ist noch offen. Für Current, PipelineOff und PipelineOn denselben Standpunkt, dieselbe Blickrichtung und dieselben Grafikeinstellungen verwenden. Zuerst im Stand messen, danach eine separate Fahrt auf derselben Route für Streaming-Spitzen aufzeichnen. Vor dem Vergleich Save, Kameraposition, Auflösung, FPS-Limit und Present-Modus notieren. Die Uncapped-Aufnahme zeigt, wie weit die Framerate unter 100 FPS fällt. Ein separates Messfenster mit festem Limit zeigt, ob 99 Prozent der Anzeigeintervalle im Budget bleiben.

### Spielmessung um 10:20 Uhr

Der normale D3D11-Spielstart meldet 95,6 Game-FPS, 10,5 ms im Mittel, P95 10,9 ms und maximal 16,9 ms im internen Logfenster. Mittlere CPU-Aufteilung 6,7 ms Game, 3,7 ms Render, 0,1 ms IOP und gerundet 0 ms Wait. Last rund 4.469 Draws, 308.000 Vertices und 22.208 DMA-Tags pro Game-Frame. Das Log meldet 61 Host-FPS und rund 60 VBlanks pro Sekunde. Die Anzeigeintervalle wurden separat noch nicht erfasst. Der CSV-Export enthält eine Zeile mit 10,460 ms. Daher gibt es für diesen Lauf keinen belastbaren P99 oder A/B-Vergleich. Die CPU-Aufteilung priorisiert GameThread-Stacks, während die Host-Ausgabe separat vermessen werden muss.

- Start-Profiling.ps1 -GpuAudit aktiviert D3D11-GPU-Timestamps und erzeugt phases.csv mit Upload- und Renderphasenzählern. GPU-Zeiten sind nach Renderabschnitt aggregiert im Spiellog. Für eine Zuordnung einzelner GPU-Spitzen und tatsächlicher Anzeigeintervalle bleibt ein zeitlich passender ETW-/CapFrameX-Trace erforderlich.
