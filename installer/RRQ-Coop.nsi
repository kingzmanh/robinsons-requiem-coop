; Robinson's Requiem Co-op - installer (NSIS 3, Modern UI 2)
; Build: installer/build.sh (stages the files into installer/stage and runs makensis)
;
; Ships only the mod. On the player's PC it copies the game files from their own copy of the
; game (GOG or any other version) and makes the program icon from that game's art.

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!define APPNAME   "Robinson's Requiem Co-op"
!define APPKEY    "RRQ-Coop"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPKEY}"
!define STAGE     "stage"

Name "${APPNAME}"
OutFile "..\release\RRQ-Coop-Setup-${VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\${APPKEY}"
InstallDirRegKey HKCU "Software\${APPKEY}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "${APPNAME} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Fan mod. Robinson's Requiem (c) Silmarils."

Var GameDir
Var StartMenuDir
Var Args

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "${APPNAME}"
!define MUI_WELCOMEPAGE_TEXT "Online co-op for Robinson's Requiem, running natively on Windows.$\r$\n$\r$\nYou need your own copy of the game (GOG or any other version). Setup copies the game files from it, so nothing from the game is in this download.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME

; --- where the game is
!define MUI_PAGE_HEADER_TEXT "Your copy of Robinson's Requiem"
!define MUI_PAGE_HEADER_SUBTEXT "Setup copies the game files from it."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose the folder of your Robinson's Requiem game: the folder that contains MAIN.IO (for GOG: the game's install folder). Nothing in it is changed."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "Game folder"
!define MUI_DIRECTORYPAGE_VARIABLE $GameDir
!define MUI_DIRECTORYPAGE_VERIFYONLEAVE
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE GameDirLeave
!insertmacro MUI_PAGE_DIRECTORY

; --- where the mod goes
!define MUI_PAGE_HEADER_TEXT "Install location"
!define MUI_PAGE_HEADER_SUBTEXT "Where the co-op mod and its copy of the game files go (about 330 MB)."
!define MUI_DIRECTORYPAGE_TEXT_TOP "The co-op mod, its copy of the game files and your co-op saves go into this folder."
!insertmacro MUI_PAGE_DIRECTORY

!define MUI_COMPONENTSPAGE_NODESC
!insertmacro MUI_PAGE_COMPONENTS
!define MUI_STARTMENUPAGE_REGISTRY_ROOT HKCU
!define MUI_STARTMENUPAGE_REGISTRY_KEY "Software\${APPKEY}"
!define MUI_STARTMENUPAGE_REGISTRY_VALUENAME "StartMenuFolder"
!define MUI_STARTMENUPAGE_DEFAULTFOLDER "${APPNAME}"
!insertmacro MUI_PAGE_STARTMENU StartMenu $StartMenuDir
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Open the Start menu folder"
!define MUI_FINISHPAGE_RUN_FUNCTION OpenStartMenu
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------------------------
; find the game: GOG registry, then the usual folders
Function .onInit
  StrCpy $StartMenuDir "${APPNAME}"
  StrCpy $GameDir ""
  SetRegView 32
  ReadRegStr $0 HKLM "SOFTWARE\GOG.com\Games\1207662373" "path"
  ${If} ${FileExists} "$0\MAIN.IO"
    StrCpy $GameDir $0
    Return
  ${EndIf}
  ReadRegStr $0 HKCU "Software\Valve\Steam" "SteamPath"
  ${If} $0 != ""
    ${If} ${FileExists} "$0\steamapps\common\Robinson's Requiem\MAIN.IO"
      StrCpy $GameDir "$0\steamapps\common\Robinson's Requiem"
      Return
    ${EndIf}
    ${If} ${FileExists} "$0\steamapps\common\Robinsons Requiem\MAIN.IO"
      StrCpy $GameDir "$0\steamapps\common\Robinsons Requiem"
      Return
    ${EndIf}
  ${EndIf}
  ${If} ${FileExists} "$PROGRAMFILES32\GOG Galaxy\Games\Robinsons Requiem\MAIN.IO"
    StrCpy $GameDir "$PROGRAMFILES32\GOG Galaxy\Games\Robinsons Requiem"
  ${ElseIf} ${FileExists} "C:\GOG Games\Robinson's Requiem\MAIN.IO"
    StrCpy $GameDir "C:\GOG Games\Robinson's Requiem"
  ${ElseIf} ${FileExists} "C:\GOG Games\Robinsons Requiem\MAIN.IO"
    StrCpy $GameDir "C:\GOG Games\Robinsons Requiem"
  ${EndIf}
FunctionEnd

Function GameDirLeave
  ${IfNot} ${FileExists} "$GameDir\MAIN.IO"
  ${OrIfNot} ${FileExists} "$GameDir\YODA.IO"
  ${OrIfNot} ${FileExists} "$GameDir\HEROS.IO"
    MessageBox MB_ICONEXCLAMATION "This folder does not contain the PC (DOS) game files of Robinson's Requiem (MAIN.IO, YODA.IO, ...).$\r$\n$\r$\nChoose the folder that contains MAIN.IO."
    Abort
  ${EndIf}
FunctionEnd

Function OpenStartMenu
  ExecShell "open" "$SMPROGRAMS\$StartMenuDir"
FunctionEnd

; ---------------------------------------------------------------------------------------------
Section "!Co-op mod" SecMain
  SectionIn RO
  AddSize 330000
  SetOutPath "$INSTDIR"
  File "${STAGE}\RRQ-Coop.exe"
  File "${STAGE}\SDL2.dll"
  File "${STAGE}\README.txt"
  File "${STAGE}\LICENSE-alis.txt"
  File "${STAGE}\LICENSE-SDL2.txt"

  ; the game files, from the player's own copy
  DetailPrint "Copying the game files from $GameDir ..."
  CreateDirectory "$INSTDIR\game"
  CopyFiles /SILENT "$GameDir\*.IO" "$INSTDIR\game"
  CopyFiles /SILENT "$GameDir\*.DAT" "$INSTDIR\game"
  CopyFiles /SILENT "$GameDir\START.EXE" "$INSTDIR\game"
  CopyFiles /SILENT "$GameDir\START.STP" "$INSTDIR\game"
  CopyFiles /SILENT "$GameDir\S.TST" "$INSTDIR\game"
  ${IfNot} ${FileExists} "$INSTDIR\game\MAIN.IO"
    MessageBox MB_ICONSTOP "Copying the game files failed."
    Abort
  ${EndIf}

  ; the program icon, made from the game's own art (the options menu's hero). A temporary copy
  ; of the program does it, so it can write the icon into the installed RRQ-Coop.exe.
  DetailPrint "Making the program icon from your game (about 20 seconds) ..."
  InitPluginsDir
  CopyFiles /SILENT "$INSTDIR\RRQ-Coop.exe" "$PLUGINSDIR\rrq-icon.exe"
  CopyFiles /SILENT "$INSTDIR\SDL2.dll" "$PLUGINSDIR\SDL2.dll"
  SetOutPath "$PLUGINSDIR"
  nsExec::Exec '"$PLUGINSDIR\rrq-icon.exe" --make-icon "$INSTDIR\RRQ-Coop.ico" --icon-exe "$INSTDIR\RRQ-Coop.exe" "$INSTDIR\game"'
  Pop $0
  SetOutPath "$INSTDIR"
  ${If} ${FileExists} "$INSTDIR\RRQ-Coop.ico"
    DetailPrint "Icon done."
  ${Else}
    DetailPrint "Could not make the icon (the program keeps its default icon)."
  ${EndIf}

  ; uninstaller + Add/Remove Programs entry (per user)
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\${APPKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\${APPKEY}" "GameSource" "$GameDir"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "Robinson's Requiem Co-op mod"
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\RRQ-Coop.exe"
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" "$0"
SectionEnd

Section /o "Start in fullscreen" SecFull
SectionEnd

Section "-Shortcuts" SecShortcuts
  StrCpy $Args ""
  ${If} ${SectionIsSelected} ${SecFull}
    StrCpy $Args "-f "
  ${EndIf}
  ; Join asks for the host's address in a small console, then starts the game without it
  FileOpen $0 "$INSTDIR\Join Co-op.bat" w
  FileWrite $0 '@echo off$\r$\ncd /d "%~dp0"$\r$\ntitle Join Robinson$\'s Requiem Co-op$\r$\necho Join a co-op game. Type the host$\'s IP address (add :port if the host uses another port than 27531).$\r$\nset /p HOSTIP=Host IP address: $\r$\nif "%HOSTIP%"=="" exit /b$\r$\nstart "" "%~dp0RRQ-Coop.exe" $Args--join %HOSTIP% "%~dp0game"$\r$\n'
  FileClose $0

  StrCpy $1 "$INSTDIR\RRQ-Coop.exe"
  ${If} ${FileExists} "$INSTDIR\RRQ-Coop.ico"
    StrCpy $1 "$INSTDIR\RRQ-Coop.ico"
  ${EndIf}
  SetOutPath "$INSTDIR"
  !insertmacro MUI_STARTMENU_WRITE_BEGIN StartMenu
    CreateDirectory "$SMPROGRAMS\$StartMenuDir"
    CreateShortcut "$SMPROGRAMS\$StartMenuDir\Host Co-op.lnk" "$INSTDIR\RRQ-Coop.exe" '$Args--host "$INSTDIR\game"' "$1" 0
    CreateShortcut "$SMPROGRAMS\$StartMenuDir\Join Co-op.lnk" "$INSTDIR\Join Co-op.bat" "" "$1" 0
    CreateShortcut "$SMPROGRAMS\$StartMenuDir\Play Solo.lnk" "$INSTDIR\RRQ-Coop.exe" '$Args"$INSTDIR\game"' "$1" 0
    CreateShortcut "$SMPROGRAMS\$StartMenuDir\Read Me.lnk" "$INSTDIR\README.txt"
    CreateShortcut "$SMPROGRAMS\$StartMenuDir\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  !insertmacro MUI_STARTMENU_WRITE_END
SectionEnd

Section "Desktop shortcuts (Host / Join)" SecDesktop
  StrCpy $1 "$INSTDIR\RRQ-Coop.exe"
  ${If} ${FileExists} "$INSTDIR\RRQ-Coop.ico"
    StrCpy $1 "$INSTDIR\RRQ-Coop.ico"
  ${EndIf}
  SetOutPath "$INSTDIR"
  CreateShortcut "$DESKTOP\RRQ Co-op - Host.lnk" "$INSTDIR\RRQ-Coop.exe" '$Args--host "$INSTDIR\game"' "$1" 0
  CreateShortcut "$DESKTOP\RRQ Co-op - Join.lnk" "$INSTDIR\Join Co-op.bat" "" "$1" 0
  WriteRegDWORD HKCU "Software\${APPKEY}" "DesktopShortcuts" 1
SectionEnd

Section "-Refresh icons"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

; ---------------------------------------------------------------------------------------------
Section "Uninstall"
  !insertmacro MUI_STARTMENU_GETFOLDER StartMenu $StartMenuDir
  Delete "$SMPROGRAMS\$StartMenuDir\*.lnk"
  RMDir "$SMPROGRAMS\$StartMenuDir"
  Delete "$DESKTOP\RRQ Co-op - Host.lnk"
  Delete "$DESKTOP\RRQ Co-op - Join.lnk"

  Delete "$INSTDIR\RRQ-Coop.exe"
  Delete "$INSTDIR\SDL2.dll"
  Delete "$INSTDIR\RRQ-Coop.ico"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\LICENSE-alis.txt"
  Delete "$INSTDIR\LICENSE-SDL2.txt"
  Delete "$INSTDIR\Join Co-op.bat"
  Delete "$INSTDIR\*.log"
  ; the copied game files go; saves (*.SAV, *.SAV.p2, BACKSAVE.INF) stay
  Delete "$INSTDIR\game\*.IO"
  Delete "$INSTDIR\game\*.DAT"
  Delete "$INSTDIR\game\START.EXE"
  Delete "$INSTDIR\game\START.STP"
  Delete "$INSTDIR\game\S.TST"
  Delete "$INSTDIR\game\alis.state"
  Delete "$INSTDIR\game\*.log"
  RMDir "$INSTDIR\game"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  ${If} ${FileExists} "$INSTDIR\game\*.*"
    DetailPrint "Your saves are kept in $INSTDIR\game"
  ${EndIf}

  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "Software\${APPKEY}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
