; Enquber Windows installer.
;
; Built by packaging/windows/package-windows.py --installer, which passes
; SOURCE_DIR (the deployed build tree), OUTPUT_FILE, APP_VERSION,
; APP_VERSION_QUAD, LICENSE_FILE, ICON_FILE, WELCOME_BITMAP and HEADER_BITMAP
; as /D defines.
; Needs NSIS 3.x (makensis).
;
; The install is per-user: no UAC prompt, everything below
; %LOCALAPPDATA%\Programs\Enquber.  Windows 10 or newer is required because
; the MinGW runtime and Qt DLLs need the Universal CRT.

Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "WinVer.nsh"
!include "FileFunc.nsh"

!ifndef APP_VERSION
    !define APP_VERSION "0.0.0"
!endif
!ifndef APP_VERSION_QUAD
    !define APP_VERSION_QUAD "0.0.0.0"
!endif
!ifndef OUTPUT_FILE
    !define OUTPUT_FILE "enquber-setup.exe"
!endif
!ifndef SOURCE_DIR
    !error "SOURCE_DIR is not defined"
!endif
!ifndef LICENSE_FILE
    !error "LICENSE_FILE is not defined"
!endif

Name "Enquber ${APP_VERSION}"
Caption "Enquber ${APP_VERSION} Setup"
BrandingText "Enquber ${APP_VERSION}"
OutFile "${OUTPUT_FILE}"

InstallDir "$LOCALAPPDATA\Programs\Enquber"
InstallDirRegKey HKCU "Software\Enquber" "InstallDir"
RequestExecutionLevel user
ManifestDPIAware true
ManifestSupportedOS all

VIProductVersion "${APP_VERSION_QUAD}"
VIAddVersionKey "ProductName" "Enquber"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "FileDescription" "Enquber setup"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 Morgan Astra"

!ifdef ICON_FILE
    !define MUI_ICON "${ICON_FILE}"
    !define MUI_UNICON "${ICON_FILE}"
!endif
!ifdef WELCOME_BITMAP
    !define MUI_WELCOMEFINISHPAGE_BITMAP "${WELCOME_BITMAP}"
!endif
!ifdef HEADER_BITMAP
    !define MUI_HEADERIMAGE
    !define MUI_HEADERIMAGE_RIGHT
    !define MUI_HEADERIMAGE_BITMAP "${HEADER_BITMAP}"
!endif
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "Welcome to Enquber ${APP_VERSION}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\enquber.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run Enquber"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Enquber" SecEnquber
    SectionIn RO

    SetOutPath "$INSTDIR"
    File /r "${SOURCE_DIR}/*"

    WriteUninstaller "$INSTDIR\uninstall.exe"

    CreateDirectory "$SMPROGRAMS\Enquber"
    CreateShortcut "$SMPROGRAMS\Enquber\Enquber.lnk" "$INSTDIR\enquber.exe"
    CreateShortcut "$SMPROGRAMS\Enquber\Uninstall Enquber.lnk" "$INSTDIR\uninstall.exe"

    ; The entry in Settings > Apps.
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber" \
        "DisplayName" "Enquber"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber" \
        "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber" \
        "Publisher" "Morgan Astra"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber" \
        "DisplayIcon" "$INSTDIR\enquber.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber" \
        "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber" \
        "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber" \
        "InstallLocation" "$INSTDIR"
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber" \
        "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber" \
        "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber" \
        "EstimatedSize" $0

    WriteRegStr HKCU "Software\Enquber" "InstallDir" "$INSTDIR"
SectionEnd

Section /o "Desktop shortcut" SecDesktop
    CreateShortcut "$DESKTOP\Enquber.lnk" "$INSTDIR\enquber.exe"
SectionEnd

; A running instance keeps enquber.exe open for writing, so both the installer
; (on an upgrade) and the uninstaller have to wait until it is closed.
; /SD IDCANCEL keeps silent runs from waiting on a user that is not there.
Function CheckRunning
retry:
    IfFileExists "$INSTDIR\enquber.exe" 0 done
    ClearErrors
    FileOpen $9 "$INSTDIR\enquber.exe" a
    IfErrors 0 close
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
        "Enquber appears to be running. Please close it, then click Retry." \
        /SD IDCANCEL IDRETRY retry
    Abort
close:
    FileClose $9
done:
FunctionEnd

Function un.CheckRunning
retry:
    IfFileExists "$INSTDIR\enquber.exe" 0 done
    ClearErrors
    FileOpen $9 "$INSTDIR\enquber.exe" a
    IfErrors 0 close
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
        "Enquber appears to be running. Please close it, then click Retry." \
        /SD IDCANCEL IDRETRY retry
    Abort
close:
    FileClose $9
done:
FunctionEnd

Function .onInit
    ${IfNot} ${AtLeastWin10}
        MessageBox MB_OK|MB_ICONSTOP "Enquber requires Windows 10 or newer."
        Abort
    ${EndIf}
    Call CheckRunning
FunctionEnd

Function un.onInit
    Call un.CheckRunning
FunctionEnd

Section "Uninstall"
    Delete "$SMPROGRAMS\Enquber\Enquber.lnk"
    Delete "$SMPROGRAMS\Enquber\Uninstall Enquber.lnk"
    RMDir "$SMPROGRAMS\Enquber"
    Delete "$DESKTOP\Enquber.lnk"

    RMDir /r "$INSTDIR"

    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Enquber"
    DeleteRegKey HKCU "Software\Enquber"
SectionEnd
