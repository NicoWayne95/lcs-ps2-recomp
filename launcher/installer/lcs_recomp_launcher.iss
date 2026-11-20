; LCS Recomp - Windows installer (Inno Setup 6)
; Per user, no admin rights. Suggested folder C:\Games\LCS Recomp (any writable folder works; not Program Files,
; where the game folder could not be written). Everything stays in that folder: the launcher, its source code ("Source Code"),
; launcher.json, webview\ and the game folder (game\: the player's ISO, the game the launcher builds from it,
; saves\mc0, logs, lcs.ini) - nothing in AppData.
; "Portable": no Start menu entry and no entry in Windows' installed apps (no uninstaller) - deleting the folder
; removes everything. The normal install's uninstaller removes what it installed and leaves game\ in place.
; Ships no game files and no game code: the game is built on the player's PC from their own ISO.

#define AppName "LCS Recomp"
#define AppVersion "0.2.0"
#define AppExe "lcs-recomp-launcher.exe"
#ifndef LauncherSource
#define LauncherSource "..\src-tauri\target\release\" + AppExe
#endif
#ifndef KitSource
#define KitSource "..\Source Code"
#endif

[Setup]
AppId={{7C2E4B1A-5D3F-4E8B-9A61-2B0C7D4E9F13}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion} Beta
AppPublisher=LCS Recomp
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
DefaultDirName={sd}\Games\LCS Recomp
DisableDirPage=no
DefaultGroupName=LCS Recomp
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist-installer
OutputBaseFilename=LCS-Recomp-Beta-Setup-{#AppVersion}
SetupIconFile=..\src-tauri\icons\icon.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
; portable: no uninstaller, no entry in Windows' installed apps
Uninstallable=not WizardIsTaskSelected('portable')
CreateUninstallRegKey=not WizardIsTaskSelected('portable')
LicenseFile={#KitSource}\NOTICE.txt
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
MinVersion=10.0
; the language dialog always in English (not in the Windows display language), English preselected
LanguageDetectionMethod=none
ShowLanguageDialog=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"

[LangOptions]
; the list shows the languages' English names
english.LanguageName=English
german.LanguageName=German

[CustomMessages]
english.PortableGroup=Install type:
english.Portable=Portable (no Start menu entry, no uninstaller - delete the folder to remove everything)
german.PortableGroup=Installationsart:
german.Portable=Portabel (kein Startmenü-Eintrag, kein Deinstallationsprogramm - Ordner löschen entfernt alles)

[Tasks]
Name: "portable"; Description: "{cm:Portable}"; GroupDescription: "{cm:PortableGroup}"; Flags: unchecked
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#LauncherSource}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#KitSource}\*"; DestDir: "{app}\Source Code"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#KitSource}\NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#KitSource}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "beta\BETA_README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme

[InstallDelete]
; the kit is replaced whole (an update from the launcher may have changed it; no files of an older kit stay)
Type: filesandordirs; Name: "{app}\Source Code"
Type: filesandordirs; Name: "{app}\Source Code.new"
Type: filesandordirs; Name: "{app}\Source Code.old"
Type: files; Name: "{app}\Source Code.pending"
; the folder's name before 0.2.0
Type: filesandordirs; Name: "{app}\kit"
Type: filesandordirs; Name: "{app}\kit.new"
Type: filesandordirs; Name: "{app}\kit.old"
Type: files; Name: "{app}\kit.pending"

[UninstallDelete]
; the kit as an update left it (files the installer didn't put there) and the launcher's own files - never game\
Type: filesandordirs; Name: "{app}\Source Code"
Type: filesandordirs; Name: "{app}\Source Code.new"
Type: filesandordirs; Name: "{app}\Source Code.old"
Type: files; Name: "{app}\Source Code.pending"
; the folder's name before 0.2.0
Type: filesandordirs; Name: "{app}\kit"
Type: filesandordirs; Name: "{app}\kit.new"
Type: filesandordirs; Name: "{app}\kit.old"
Type: files; Name: "{app}\kit.pending"
Type: files; Name: "{app}\launcher.json"
Type: filesandordirs; Name: "{app}\webview"

[Dirs]
Name: "{app}\game"

[Icons]
Name: "{autoprograms}\LCS Recomp"; Filename: "{app}\{#AppExe}"; Tasks: not portable
Name: "{autodesktop}\LCS Recomp"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,LCS Recomp}"; Flags: nowait postinstall skipifsilent

[Code]
// The uninstaller removes the game too (the user's ask 2026-11-18: after an uninstall the built game, its build tools
// and logs were all still there) - all of game\ except the player's own: the savegames (Savegame\, an older saves\),
// their backups (backups\) and the ISO (gamedata\LCS.iso). A savegame folder chosen elsewhere is never touched.
function KeepInGame(const Name: String): Boolean;
begin
  Result := (CompareText(Name, 'Savegame') = 0) or (CompareText(Name, 'saves') = 0) or
            (CompareText(Name, 'backups') = 0) or (CompareText(Name, 'gamedata') = 0);
end;

procedure DeleteAllBut(const Dir: String; const KeepIso: Boolean);
var
  FindRec: TFindRec;
  Path: String;
begin
  if FindFirst(Dir + '\*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Path := Dir + '\' + FindRec.Name;
          if KeepIso then
          begin
            if CompareText(FindRec.Name, 'LCS.iso') <> 0 then
            begin
              if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
                DelTree(Path, True, True, True)
              else
                DeleteFile(Path);
            end;
          end
          else if not KeepInGame(FindRec.Name) then
          begin
            if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
              DelTree(Path, True, True, True)
            else
              DeleteFile(Path);
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Game: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Game := ExpandConstant('{app}\game');
    if DirExists(Game) then
    begin
      DeleteAllBut(Game, False);
      if DirExists(Game + '\gamedata') then
        DeleteAllBut(Game + '\gamedata', True);
      RemoveDir(Game + '\gamedata'); // only when empty
      RemoveDir(Game);
    end;
    RemoveDir(ExpandConstant('{app}'));
  end;
end;
