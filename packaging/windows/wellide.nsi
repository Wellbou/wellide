; Wellide installer (NSIS). Built by .github/workflows/release.yml
Unicode true
!include "MUI2.nsh"
!ifndef VERSION
  !define VERSION "0.2.0"
!endif
Name "Wellide ${VERSION}"
OutFile "wellide-${VERSION}-setup.exe"
InstallDir "$PROGRAMFILES64\Wellide"
InstallDirRegKey HKLM "Software\Wellide" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
!define MUI_ICON "wellide.ico"
!define MUI_UNICON "wellide.ico"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\wellide.exe"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Russian"

Section "Wellide"
  nsExec::Exec 'taskkill /F /IM wellide.exe'
  nsExec::Exec 'taskkill /F /IM sing-box.exe'
  SetOutPath "$INSTDIR"
  File /r "dist\*.*"
  WriteRegStr HKLM "Software\Wellide" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Wellide.lnk" "$INSTDIR\bin\wellide.exe" "" "$INSTDIR\bin\wellide.exe"
  CreateShortcut "$DESKTOP\Wellide.lnk" "$INSTDIR\bin\wellide.exe" "" "$INSTDIR\bin\wellide.exe"
  ; wellide:// deep links
  WriteRegStr HKCR "wellide" "" "URL:Wellide"
  WriteRegStr HKCR "wellide" "URL Protocol" ""
  WriteRegStr HKCR "wellide\shell\open\command" "" '"$INSTDIR\bin\wellide.exe" "%1"'
  !define UK "Software\Microsoft\Windows\CurrentVersion\Uninstall\Wellide"
  WriteRegStr HKLM "${UK}" "DisplayName" "Wellide"
  WriteRegStr HKLM "${UK}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UK}" "Publisher" "Wellbou"
  WriteRegStr HKLM "${UK}" "DisplayIcon" "$INSTDIR\bin\wellide.exe"
  WriteRegStr HKLM "${UK}" "UninstallString" '"$INSTDIR\uninstall.exe"'
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /F /IM wellide.exe'
  nsExec::Exec 'taskkill /F /IM sing-box.exe'
  RMDir /r "$INSTDIR"
  Delete "$SMPROGRAMS\Wellide.lnk"
  Delete "$DESKTOP\Wellide.lnk"
  DeleteRegKey HKCR "wellide"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Wellide"
  DeleteRegKey HKLM "Software\Wellide"
SectionEnd
