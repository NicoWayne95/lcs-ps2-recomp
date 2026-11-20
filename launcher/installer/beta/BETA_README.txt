================================================================================

                        L C S   R E C O M P   ·   0.2.0
                            PUBLIC BETA · PORTABLE

         A native PC port of Grand Theft Auto: Liberty City Stories (PS2)

================================================================================

   ENGLISH ........................................................... below
   DEUTSCH ................................. further down / weiter unten


################################################################################
#                                                                              #
#                                   ENGLISH                                    #
#                                                                              #
################################################################################


--------------------------------------------------------------------------------
  !  PLEASE READ FIRST
--------------------------------------------------------------------------------

  This is an UNFINISHED development build, not a finished release.

  Many features are incomplete, and some do not work correctly yet. You will
  run into bugs, glitches and possibly crashes. Please don't expect a
  polished experience.

  The purpose of this beta is to COLLECT DATA:

    *  How does it perform on other computers (frame rate, stutters)?
    *  How stable is it over longer sessions (crashes, freezes)?
    *  Does it build and start on different systems and hardware?

  Every report helps, even "it ran fine for an hour on my PC". See
  "How to send feedback" at the end of this section.


--------------------------------------------------------------------------------
  WHAT YOU NEED
--------------------------------------------------------------------------------

  *  Windows 10 or 11, 64-bit
  *  A graphics card with DirectX 11
  *  Your own disc image (ISO) of GTA: Liberty City Stories for PS2,
     USA version (SLUS-21423). Other versions are not supported.
  *  An internet connection for the first start (the launcher downloads
     its build tools once) and about 6 GB of free disk space

  No game files are included in this download. The launcher builds the game
  on your PC from your own ISO. The first build often takes 20 to 60
  minutes, depending on your PC.


--------------------------------------------------------------------------------
  INSTALLATION
--------------------------------------------------------------------------------

  This is the portable version: nothing is installed.

  1.  Unzip the archive to a folder you can write to, for example
      C:\Games\LCS Recomp - not into "Program Files".
  2.  Start lcs-recomp-launcher.exe from that folder.
  3.  In the launcher, open "Installation", select your ISO and press
      "Prepare game".
  4.  Wait for the build to finish, then press "Play game".

  Everything stays inside that folder: the launcher's settings, the built
  game, your saves, logs and lcs.ini (in the "game" folder). Nothing is
  written to AppData or the registry, and there is no Start menu entry.
  To remove LCS Recomp, delete the folder - back up "game\Savegame" first
  if you want to keep your saves.

  Windows SmartScreen may warn about an "unknown publisher", because the
  launcher is not code-signed yet. Click "More info" -> "Run anyway".
  The launcher needs Microsoft Edge WebView2, which is part of
  Windows 10/11 on almost every PC.


--------------------------------------------------------------------------------
  [OK]  WHAT WORKS
--------------------------------------------------------------------------------

  Game
    *  The game from boot to free roam and missions, including the
       original intro movies
    *  Native sound: music, radio, sound effects and cutscene audio
    *  Saving and loading (saves are stored in the "Savegame" folder)

  Graphics
    *  DirectX 11 renderer, internal resolution from the original
       640x448 up to 4K
    *  True 16:9 widescreen with the HUD kept in proportion
    *  Anti-aliasing (MSAA 2x/4x/8x), texture filtering (up to 16x)
    *  The PS2 "trails" motion-blur effect and glow effects
    *  Fullscreen and windowed mode (F11)

  Frame rate
    *  Frame limiter: 30 (as on the PS2), 60, 120 FPS or uncapped
    *  Game speed, physics and vehicle handling corrected for high
       frame rates (supported up to 240 FPS)
    *  Cutscenes stay in sync at 120 FPS and uncapped
    *  VSync, or tearing / variable refresh rate (G-Sync, FreeSync)

  Controls
    *  Keyboard and mouse with a PC-style layout, mouse camera on foot
    *  Controllers

  Settings
    *  New "Display Options" and "Video Options" pages in the game's own
       pause menu; settings are saved to lcs.ini
    *  Draw distance setting for pedestrians and vehicle detail

  Launcher
    *  ISO check, guided build, settings, save game page with backups
    *  Signed updates that keep your saves, settings and ISO
    *  Portable: runs from one folder, nothing installed


--------------------------------------------------------------------------------
  [EXPERIMENTAL]  BUILT IN, BUT NOT TESTED ENOUGH
--------------------------------------------------------------------------------

  These parts are new. They may work, or they may cause problems:

    *  64 MB memory mode: the game now uses 64 MB instead of the PS2's
       32 MB. It reduced stutters in our tests, but long sessions have
       not been tested yet.
    *  Cutscenes above 150 FPS (picture and sound should stay in sync)
    *  Wanted stars and flashing HUD items at high frame rates
    *  Aiming through scopes with the mouse
    *  Shop sale meters and mission countdowns at high frame rates
    *  Mouse support in the pause menu
    *  The launcher's error handling (no internet, wrong ISO, little
       disk space) and cancelling / resuming the first build


--------------------------------------------------------------------------------
  [X]  NOT WORKING YET
--------------------------------------------------------------------------------

    *  Draw distance of buildings, roads, street lights and props is still
       limited to about 300-500 m by the game's map data. The draw distance
       setting only affects pedestrians and vehicle detail.
    *  Mouse free aim is not finished (aiming uses the game's lock-on).
    *  Button icons in on-screen prompts don't always match your device.
    *  Above 240 FPS the game runs slightly faster than real time.
    *  Only the USA version of the game (SLUS-21423) is supported.


--------------------------------------------------------------------------------
  HOW TO SEND FEEDBACK
--------------------------------------------------------------------------------

  Please open an issue on GitHub:

      https://github.com/NicoWayne95/lcs-ps2-recomp/issues

  What helps most:

    1.  Your PC: graphics card, CPU, RAM, Windows version
    2.  Your settings: frame limiter, resolution, MSAA
    3.  How it runs: average frame rate, stutters, how long you played
    4.  What happened, what you expected, and how to get there
        (mission, place, steps) - for bugs and crashes
    5.  The log file and your settings from the game folder:
          logs\last_run.log        (after a crash: logs\previous_run.log)
          lcs.ini
    6.  A screenshot or short video, if you can

  Game folder:  the "game" folder next to lcs-recomp-launcher.exe

  Please do NOT upload ISOs, other people's saves or files from the
  "cache" folder.


--------------------------------------------------------------------------------
  LEGAL
--------------------------------------------------------------------------------

  LCS Recomp is a fan project. It is not affiliated with or endorsed by
  Rockstar Games or Take-Two Interactive. Grand Theft Auto and all game
  content are property of their respective owners. You need to own the
  original game. LCS Recomp is free software under the GPL-3.0 - see
  NOTICE.txt and LICENSE.txt; its source code is in the "Source Code"
  folder.


################################################################################
#                                                                              #
#                                   DEUTSCH                                    #
#                                                                              #
################################################################################


--------------------------------------------------------------------------------
  !  BITTE ZUERST LESEN
--------------------------------------------------------------------------------

  Das ist ein UNFERTIGER Entwicklungs-Build, kein fertiges Release.

  Viele Funktionen sind unvollständig, und manche funktionieren noch nicht
  korrekt. Du wirst auf Fehler, Grafikprobleme und möglicherweise Abstürze
  stoßen. Bitte erwarte kein ausgereiftes Spielerlebnis.

  Ziel dieser Beta ist es, DATEN ZU SAMMELN:

    *  Wie läuft es auf anderen Computern (Bildrate, Ruckler)?
    *  Wie stabil ist es über längere Zeit (Abstürze, Hänger)?
    *  Lässt es sich auf unterschiedlichen Systemen bauen und starten?

  Jede Rückmeldung hilft, auch "lief bei mir eine Stunde problemlos".
  Siehe "So gibst du Feedback" am Ende dieses Abschnitts.


--------------------------------------------------------------------------------
  WAS DU BRAUCHST
--------------------------------------------------------------------------------

  *  Windows 10 oder 11, 64 Bit
  *  Eine Grafikkarte mit DirectX 11
  *  Dein eigenes Disc-Image (ISO) von GTA: Liberty City Stories für die
     PS2, US-Version (SLUS-21423). Andere Versionen werden nicht unterstützt.
  *  Eine Internetverbindung beim ersten Start (der Launcher lädt seine
     Build-Werkzeuge einmalig) und etwa 6 GB freier Speicherplatz

  Dieser Download enthält keine Spieldateien. Der Launcher baut das Spiel
  auf deinem PC aus deiner eigenen ISO. Der erste Build dauert je nach PC
  oft 20 bis 60 Minuten.


--------------------------------------------------------------------------------
  INSTALLATION
--------------------------------------------------------------------------------

  Das ist die portable Version: Es wird nichts installiert.

  1.  Das Archiv in einen Ordner entpacken, in den du schreiben darfst,
      zum Beispiel C:\Games\LCS Recomp - nicht nach "Programme".
  2.  lcs-recomp-launcher.exe aus diesem Ordner starten.
  3.  Im Launcher "Installation" öffnen, die ISO auswählen und auf
      "Spiel vorbereiten" klicken.
  4.  Warten, bis der Build fertig ist, dann auf "Spiel starten" klicken.

  Alles bleibt in diesem Ordner: die Einstellungen des Launchers, das
  gebaute Spiel, deine Spielstände, Logs und lcs.ini (im Ordner "game").
  Es wird nichts in AppData oder die Registry geschrieben, und es gibt
  keinen Startmenü-Eintrag. Zum Entfernen den Ordner löschen - vorher
  "game\Savegame" sichern, wenn du deine Spielstände behalten willst.

  Windows SmartScreen warnt eventuell vor einem "unbekannten Herausgeber",
  weil der Launcher noch nicht signiert ist. Klicke auf "Weitere
  Informationen" -> "Trotzdem ausführen". Der Launcher braucht Microsoft
  Edge WebView2, das auf fast jedem PC mit Windows 10/11 schon vorhanden
  ist.


--------------------------------------------------------------------------------
  [OK]  WAS FUNKTIONIERT
--------------------------------------------------------------------------------

  Spiel
    *  Das Spiel vom Start bis zum freien Spielen und den Missionen,
       inklusive der originalen Intro-Videos
    *  Nativer Sound: Musik, Radio, Soundeffekte und Cutscene-Ton
    *  Speichern und Laden (Spielstände im Ordner "Savegame")

  Grafik
    *  DirectX-11-Renderer, interne Auflösung vom Original 640x448 bis 4K
    *  Echtes 16:9-Widescreen mit proportionalem HUD
    *  Kantenglättung (MSAA 2x/4x/8x), Texturfilterung (bis 16x)
    *  Der PS2-"Trails"-Bewegungsunschärfe-Effekt und Glow-Effekte
    *  Vollbild und Fenstermodus (F11)

  Bildrate
    *  Frame-Limiter: 30 (wie auf der PS2), 60, 120 FPS oder unbegrenzt
    *  Spieltempo, Physik und Fahrverhalten für hohe Bildraten korrigiert
       (unterstützt bis 240 FPS)
    *  Cutscenes bleiben bei 120 FPS und unbegrenzt synchron
    *  VSync oder Tearing / variable Bildwiederholrate (G-Sync, FreeSync)

  Steuerung
    *  Tastatur und Maus mit PC-typischer Belegung, Mauskamera zu Fuß
    *  Controller

  Einstellungen
    *  Neue Seiten "Display Options" und "Video Options" im Pausenmenü
       des Spiels; Einstellungen werden in lcs.ini gespeichert
    *  Sichtweite für Fußgänger und Fahrzeug-Details einstellbar

  Launcher
    *  ISO-Prüfung, geführter Build, Einstellungen, Spielstand-Seite mit
       Backups
    *  Signierte Updates, die Spielstände, Einstellungen und ISO behalten
    *  Portabel: läuft aus einem Ordner, es wird nichts installiert


--------------------------------------------------------------------------------
  [EXPERIMENTELL]  EINGEBAUT, ABER NOCH NICHT AUSREICHEND GETESTET
--------------------------------------------------------------------------------

  Diese Teile sind neu. Sie können funktionieren oder Probleme machen:

    *  64-MB-Speichermodus: Das Spiel nutzt jetzt 64 MB statt der 32 MB der
       PS2. In unseren Tests wurden die Ruckler weniger, lange Spielzeiten
       sind aber noch nicht getestet.
    *  Cutscenes über 150 FPS (Bild und Ton sollten synchron bleiben)
    *  Wanted-Sterne und blinkende HUD-Elemente bei hohen Bildraten
    *  Zielen durchs Zielfernrohr mit der Maus
    *  Verkaufsbalken in Shops und Missions-Countdowns bei hohen Bildraten
    *  Mausbedienung im Pausenmenü
    *  Fehlerbehandlung im Launcher (kein Internet, falsche ISO, wenig
       Speicherplatz) sowie Abbrechen / Fortsetzen des ersten Builds


--------------------------------------------------------------------------------
  [X]  FUNKTIONIERT NOCH NICHT
--------------------------------------------------------------------------------

    *  Die Sichtweite von Gebäuden, Straßen, Laternen und Props ist durch
       die Kartendaten des Spiels noch auf etwa 300-500 m begrenzt. Die
       Sichtweiten-Einstellung wirkt nur auf Fußgänger und Fahrzeug-Details.
    *  Freies Zielen mit der Maus ist noch nicht fertig (Zielen über die
       Zielerfassung des Spiels).
    *  Tasten-Symbole in Hinweisen passen nicht immer zum Eingabegerät.
    *  Über 240 FPS läuft das Spiel etwas schneller als in Echtzeit.
    *  Nur die US-Version des Spiels (SLUS-21423) wird unterstützt.


--------------------------------------------------------------------------------
  SO GIBST DU FEEDBACK
--------------------------------------------------------------------------------

  Bitte erstelle ein Issue auf GitHub:

      https://github.com/NicoWayne95/lcs-ps2-recomp/issues

  Am meisten helfen:

    1.  Dein PC: Grafikkarte, Prozessor, Arbeitsspeicher, Windows-Version
    2.  Deine Einstellungen: Frame-Limiter, Auflösung, MSAA
    3.  Wie es läuft: durchschnittliche Bildrate, Ruckler, wie lange du
        gespielt hast
    4.  Was passiert ist, was du erwartet hättest und wie man dorthin kommt
        (Mission, Ort, Schritte) - bei Fehlern und Abstürzen
    5.  Die Logdatei und deine Einstellungen aus dem Spielordner:
          logs\last_run.log        (nach einem Absturz: logs\previous_run.log)
          lcs.ini
    6.  Wenn möglich ein Screenshot oder kurzes Video

  Spielordner:  der Ordner "game" neben lcs-recomp-launcher.exe

  Bitte KEINE ISOs, Spielstände anderer Leute oder Dateien aus dem Ordner
  "cache" hochladen.


--------------------------------------------------------------------------------
  RECHTLICHES
--------------------------------------------------------------------------------

  LCS Recomp ist ein Fan-Projekt. Es steht in keiner Verbindung zu Rockstar
  Games oder Take-Two Interactive und wird von ihnen nicht unterstützt.
  Grand Theft Auto und alle Spielinhalte sind Eigentum der jeweiligen
  Rechteinhaber. Du musst das Originalspiel besitzen. LCS Recomp ist freie
  Software unter der GPL-3.0 - siehe NOTICE.txt und LICENSE.txt; der
  Quellcode liegt im Ordner "Source Code".

================================================================================
