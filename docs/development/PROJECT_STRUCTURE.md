# Projektstruktur

- `game/` und `launcher/` sind die aktiven Quellbäume.
- `build/` enthält separate Build-Arbeitsordner. `bin/` enthält die lokalen Startversionen und deren Daten.
- `releases/staging/` sammelt lokale Kandidaten. `releases/latest/` und `releases/archive/` sind für freigegebene Stände und historische Pakete vorgesehen.
- `tests/` enthält isolierte Installationen. `tools/` enthält die Release-Werkzeuge und Toolchains.
- `backups/reference/` hält Originalkopien. `benchmarks/` hält Referenzdaten und neue Messungen.

