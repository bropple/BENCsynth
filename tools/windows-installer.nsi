; BENCsynth for Windows - installer and upgrader.
;
; Per-user throughout, so it never asks for administrator rights: the plugin
; folders it writes to are the per-user ones every host already searches, and
; the program goes in LocalAppData rather than Program Files. Nothing here
; needs elevation, and an installer that asks for it when it does not need it
; is an installer people cancel.
;
; Upgrading is the point. Every bundle is deleted before it is written, not
; merged - a .vst3 or .clap is a directory, and copying a new build over an
; old one leaves whatever the new build no longer ships. The macOS installer
; got this wrong in the opposite direction, skipping an existing app entirely,
; and shipped two releases where the plugin and its editor were different
; versions of the same program.
;
;   makensis -DSRCDIR=stage -DVERSION=v0.2.0 tools/windows-installer.nsi

!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "dev"
!endif
!ifndef SRCDIR
  !define SRCDIR "stage"
!endif
!ifndef ICONFILE
  ; Relative to wherever makensis was invoked, which is the repository root.
  ; Forward slashes: the compiler reads this on the build machine, which is
  ; Linux, where a backslash is not a path separator and the error you get is
  ; "can't open file" three macros deep.
  !define ICONFILE "assets/icon/bencsynth.ico"
!endif

Name "BENCsynth ${VERSION}"
OutFile "bencsynth-${VERSION}-windows-setup.exe"
Unicode true
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\BENCsynth"
ShowInstDetails show
ShowUninstDetails show

!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\BENCsynth"

; Where each format lives. These are the per-user directories the hosts search
; by default - see docs/PLUGIN.md. LV2 is under Roaming and the other two under
; Local, which is not a mistake and has cost an evening before now.
!define CLAPDIR "$LOCALAPPDATA\Programs\Common\CLAP"
!define VST3DIR "$LOCALAPPDATA\Programs\Common\VST3"
!define LV2DIR  "$APPDATA\LV2"

!define MUI_ABORTWARNING
!define MUI_ICON "${ICONFILE}"
!define MUI_UNICON "${ICONFILE}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\bencsynth.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run BENCsynth"

!insertmacro MUI_PAGE_LICENSE "${SRCDIR}/LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------- program

Section "BENCsynth (the program)" SEC_APP
  SectionIn RO      ; the plugins need its binary as their editor
  SetOutPath "$INSTDIR"
  SetOverwrite on
  File "${SRCDIR}/bencsynth.exe"
  File "${SRCDIR}/README.md"
  File "${SRCDIR}/LICENSE"
  File "${SRCDIR}/NOTICE"
  File /nonfatal "${SRCDIR}/ARCHITECTURE.md"

  ; The font and the wordmark. Without them the program still runs, in
  ; raylib's built-in face, which is not the look this is for.
  RMDir /r "$INSTDIR\assets"
  SetOutPath "$INSTDIR\assets"
  File /r "${SRCDIR}/assets/*.*"
  SetOutPath "$INSTDIR"

  CreateDirectory "$SMPROGRAMS\BENCsynth"
  CreateShortCut "$SMPROGRAMS\BENCsynth\BENCsynth.lnk" "$INSTDIR\bencsynth.exe"
  CreateShortCut "$SMPROGRAMS\BENCsynth\Uninstall BENCsynth.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

; ---------------------------------------------------------------- CLAP

Section "CLAP plug-in" SEC_CLAP
  ; Deleted first. A bundle is a directory and overwriting one file at a time
  ; leaves behind anything the new build stopped shipping.
  RMDir /r "${CLAPDIR}\bencsynth.clap"
  SetOutPath "${CLAPDIR}"
  SetOverwrite on
  File "${SRCDIR}/bencsynth.clap"

  ; The editor, beside the plugin.
  ;
  ; The plugin opens the rack by starting this binary as a separate process -
  ; raylib keeps its window in one global, so a plugin cannot host a window of
  ; its own. It looks in BENCSYNTH_EDITOR, then its own directory, then PATH.
  ; A copy here is five megabytes and needs no environment variable, no PATH
  ; edit and no reboot, and it can never be a different build from the plugin
  ; beside it.
  File "${SRCDIR}/bencsynth.exe"
SectionEnd

; ---------------------------------------------------------------- VST3

Section "VST3 plug-in" SEC_VST3
  RMDir /r "${VST3DIR}\bencsynth.vst3"
  SetOutPath "${VST3DIR}\bencsynth.vst3"
  SetOverwrite on
  File /r "${SRCDIR}/bencsynth.vst3/*.*"
SectionEnd

; ---------------------------------------------------------------- LV2

Section /o "LV2 plug-in (LMMS)" SEC_LV2
  RMDir /r "${LV2DIR}\bencsynth.lv2"
  SetOutPath "${LV2DIR}\bencsynth.lv2"
  SetOverwrite on
  File /r "${SRCDIR}/bencsynth.lv2/*.*"
SectionEnd

; ---------------------------------------------------------------- finish

Section -Post
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr   HKCU "${REGKEY}" "DisplayName"     "BENCsynth"
  WriteRegStr   HKCU "${REGKEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKCU "${REGKEY}" "Publisher"       "BENCO"
  WriteRegStr   HKCU "${REGKEY}" "URLInfoAbout"    "https://github.com/bropple/BENCsynth"
  WriteRegStr   HKCU "${REGKEY}" "DisplayIcon"     "$INSTDIR\bencsynth.exe"
  WriteRegStr   HKCU "${REGKEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr   HKCU "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${REGKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${REGKEY}" "EstimatedSize" "$0"
SectionEnd

LangString DESC_APP  ${LANG_ENGLISH} \
  "The standalone program. Always installed: the plug-ins open their rack by \
running this binary, so they cannot work without it."
LangString DESC_CLAP ${LANG_ENGLISH} \
  "For REAPER, Bitwig and Studio One. Installs a copy of the program beside \
the plug-in as its editor."
LangString DESC_VST3 ${LANG_ENGLISH} \
  "For Ableton, Cubase, FL Studio and Studio One. Loads the CLAP at run time, \
so leave that one selected too."
LangString DESC_LV2  ${LANG_ENGLISH} \
  "For LMMS, which needs a 1.3 build from April 2026 or later. No editor and \
no saved rack - LMMS implements neither. Off by default."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_APP}  $(DESC_APP)
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_CLAP} $(DESC_CLAP)
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_VST3} $(DESC_VST3)
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_LV2}  $(DESC_LV2)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------- uninstall

Section "Uninstall"
  Delete "$INSTDIR\bencsynth.exe"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\NOTICE"
  Delete "$INSTDIR\ARCHITECTURE.md"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /r "$INSTDIR\assets"
  RMDir "$INSTDIR"

  Delete "${CLAPDIR}\bencsynth.clap"
  Delete "${CLAPDIR}\bencsynth.exe"
  RMDir /r "${VST3DIR}\bencsynth.vst3"
  RMDir /r "${LV2DIR}\bencsynth.lv2"

  Delete "$SMPROGRAMS\BENCsynth\BENCsynth.lnk"
  Delete "$SMPROGRAMS\BENCsynth\Uninstall BENCsynth.lnk"
  RMDir  "$SMPROGRAMS\BENCsynth"

  ; Racks a person saved are theirs. Nothing under Documents or the patches
  ; folder is touched, and neither is the log.
  DeleteRegKey HKCU "${REGKEY}"
SectionEnd
