; Set search path
!addincludedir "${__FILEDIR__}"

; Installer initial constants
!define PRODUCT_NAME "ReDesk"
!define PRODUCT_VERSION "${VERSION}"
!define PRODUCT_VERSION_NUMERIC "${VERSION_NUMERIC}"
!define PRODUCT_PUBLISHER "ReDesk"
!define PRODUCT_WEB_SITE "https://www.redesk.cn/"
!define APP_NAME "ReDesk"
!define UNINSTALL_REG_KEY "ReDesk"
!define PRODUCT_SERVICE_NAME "ReDeskService"

; Installer icon path
!define MUI_ICON "${__FILEDIR__}\..\..\resources\windows\crossdesk.ico"

; Compression settings
SetCompressor /FINAL lzma

; Request admin privileges (needed to write HKLM)
RequestExecutionLevel admin

; ------ MUI Modern UI Definition ------
!include "MUI.nsh"
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

; Add run-after-install option
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Run ${PRODUCT_NAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp

!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_RESERVEFILE_INSTALLOPTIONS
; ------ End of MUI Definition ------

; Include LogicLib for process handling
!include "LogicLib.nsh"
!include "FileFunc.nsh"
Var UpdateMode

Function .onInit
    ${GetParameters} $0
    ClearErrors
    ${GetOptions} $0 "/REDESK_UPDATE" $1
    StrCpy $UpdateMode 0
    ${IfNot} ${Errors}
        StrCpy $UpdateMode 1
    ${EndIf}
FunctionEnd

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "redesk-win-x64-${PRODUCT_VERSION}.exe"
VIProductVersion "${PRODUCT_VERSION_NUMERIC}"
VIAddVersionKey /LANG=2052 "CompanyName" "ReDesk"
VIAddVersionKey /LANG=2052 "FileDescription" "ReDesk Installer"
VIAddVersionKey /LANG=2052 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "Copyright (C) ReDesk contributors"
VIAddVersionKey /LANG=2052 "OriginalFilename" "redesk-win-x64-${PRODUCT_VERSION}.exe"
VIAddVersionKey /LANG=2052 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${PRODUCT_VERSION}"
InstallDir "$PROGRAMFILES\ReDesk"
InstallDirRegKey HKCU "Software\${PRODUCT_NAME}" "InstallDir"
ShowInstDetails show

Section "MainSection"
    ; The updater has already coordinated shutdown. Never kill unrelated
    ; instances by name during an unattended upgrade.
    ${If} $UpdateMode = 1
        Goto installApp
    ${EndIf}
    ; Check if ReDesk is running
    StrCpy $1 "ReDesk.exe"
    
    nsProcess::_FindProcess "$1"
    Pop $R0
    ${If} $R0 = 0  ;
        MessageBox MB_ICONQUESTION|MB_YESNO "ReDesk is running. Do you want to close it and continue the installation?" IDYES closeApp IDNO cancelInstall
    ${Else}
        Goto installApp
    ${EndIf}

closeApp:
    nsProcess::_KillProcess "$1"
    Pop $R0
    Sleep 500
    Goto installApp

cancelInstall:
    SetDetailsPrint both
    MessageBox MB_ICONEXCLAMATION|MB_OK "Installation has been aborted."
    Abort

installApp:
    Call StopInstalledService

    SetOutPath "$INSTDIR"
    ; The signed release determines file versions, not filesystem timestamps.
    SetOverwrite on

    ; Main application executable path
    File /oname=ReDesk.exe "..\..\..\..\build\windows\x64\release\redesk.exe"
    File "..\..\..\..\build\windows\x64\release\redesk_updater.exe"
    ; Bundle service-side binaries required by the Windows service flow
    File "..\..\..\..\build\windows\x64\release\redesk_service.exe"
    File "..\..\..\..\build\windows\x64\release\redesk_session_helper.exe"
    File "..\..\..\..\build\windows\x64\release\redesk_privacy_cursor_helper.exe"
    File "..\..\..\..\build\windows\x64\release\redesk_privacy_window.dll"
    ; Require the Slint runtime so an incomplete build cannot be packaged.
    File "..\..\..\..\build\windows\x64\release\slint_cpp.dll"
    ; Bundle the remaining runtime DLLs from the release output directory
    File /x redesk_privacy_window.dll /x slint_cpp.dll "..\..\..\..\build\windows\x64\release\*.dll"
    ; Amyuni usbmmidd_v2 virtual display driver for headless hosts. xmake
    ; copies apps\desktop\resources\windows\usbmmidd_v2 into the release
    ; output directory after building redesk; its License.txt ships with it.
    File /r "..\..\..\..\build\windows\x64\release\usbmmidd_v2"

    Call RegisterInstalledService

    ; Replace an existing per-user Run entry only after the new binary is in
    ; place. A fresh installation must not opt the user into auto-start.
    ClearErrors
    ExecWait '"$INSTDIR\ReDesk.exe" --autostart-migrate' $0
    ${If} ${Errors}
    ${OrIf} $0 != 0
        MessageBox MB_ICONEXCLAMATION|MB_OK "Could not migrate ReDesk auto-start. Open ReDesk and enable Auto Start again."
    ${EndIf}

    ; Write uninstall information
    WriteUninstaller "$INSTDIR\uninstall.exe"

    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "UninstallString" "$INSTDIR\uninstall.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "DisplayIcon" "$INSTDIR\ReDesk.exe"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}" "NoRepair" 1
    WriteRegStr HKCU "Software\${PRODUCT_NAME}" "InstallDir" "$INSTDIR"
SectionEnd

Section -AdditionalIcons
    ; Desktop shortcut
    CreateShortCut "$DESKTOP\${PRODUCT_NAME}.lnk" "$INSTDIR\ReDesk.exe" "" "$INSTDIR\ReDesk.exe"

    ; Start menu shortcut
    CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}.lnk" "$INSTDIR\ReDesk.exe" "" "$INSTDIR\ReDesk.exe"
SectionEnd

Section "Uninstall"
    ; Check if ReDesk is running
    StrCpy $1 "ReDesk.exe"
    
    nsProcess::_FindProcess "$1"
    Pop $R0
    ${If} $R0 = 0
        MessageBox MB_ICONQUESTION|MB_YESNO "ReDesk is running. Do you want to close it and uninstall?" IDYES closeApp IDNO cancelUninstall
    ${Else}
        Goto uninstallApp
    ${EndIf}

closeApp:
    nsProcess::_KillProcess "$1"
    Pop $R0
    Sleep 500
    Goto uninstallApp

cancelUninstall:
    SetDetailsPrint both
    MessageBox MB_ICONEXCLAMATION|MB_OK "Uninstallation has been aborted."
    Abort

uninstallApp:
    ClearErrors
    ExecWait '"$INSTDIR\ReDesk.exe" --autostart-uninstall' $0
    ${If} ${Errors}
    ${OrIf} $0 != 0
        MessageBox MB_ICONEXCLAMATION|MB_OK "Could not remove ReDesk auto-start. Remove any remaining ReDesk Autostart tasks in Task Scheduler."
    ${EndIf}
    Call un.UnregisterInstalledService
    ; usbmmidd is shared with CrossDesk: never uninstall it on ReDesk removal.
    ; Call un.RemoveUsbmmiddDriver

    ; Delete main executable and uninstaller
    Delete "$INSTDIR\ReDesk.exe"
    Delete "$INSTDIR\redesk_updater.exe"
    Delete "$INSTDIR\redesk_service.exe"
    Delete "$INSTDIR\redesk_session_helper.exe"
    Delete "$INSTDIR\redesk_privacy_cursor_helper.exe"
    Delete "$INSTDIR\redesk_privacy_window.dll"
    Delete "$INSTDIR\uninstall.exe"

    ; Leave the install directory before removing it
    SetOutPath "$TEMP"

    ; Recursively delete installation directory
    RMDir /r "$INSTDIR"

    ; Delete desktop and start menu shortcuts
    Delete "$DESKTOP\${PRODUCT_NAME}.lnk"
    Delete "$SMPROGRAMS\${PRODUCT_NAME}.lnk"

    ; Delete registry uninstall entry
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${UNINSTALL_REG_KEY}"

    ; Delete remembered install dir
    DeleteRegKey HKCU "Software\${PRODUCT_NAME}"

    ; Recursively delete ReDesk folder in user AppData
    RMDir /r "$APPDATA\ReDesk"
    RMDir /r "$LOCALAPPDATA\ReDesk"
SectionEnd

; ------ Functions ------
Function LaunchApp
    ${If} $UpdateMode = 1
        Return
    ${EndIf}
    Exec "$INSTDIR\ReDesk.exe"
FunctionEnd

Function .onInstFailed
    SetErrorLevel 1
FunctionEnd

Function StopInstalledService
    IfFileExists "$INSTDIR\ReDesk.exe" 0 stop_with_sc
    IfFileExists "$INSTDIR\redesk_service.exe" 0 stop_with_sc

    DetailPrint "Stopping existing ReDesk service"
    ExecWait '"$INSTDIR\ReDesk.exe" --service-stop' $0
    ${If} $0 = 0
        Return
    ${EndIf}

stop_with_sc:
    DetailPrint "Stopping existing ReDesk service via Service Control Manager"
    ExecWait '"$SYSDIR\sc.exe" stop ${PRODUCT_SERVICE_NAME}' $0
    ${If} $0 != 0
    ${AndIf} $0 != 1060
    ${AndIf} $0 != 1062
        MessageBox MB_ICONSTOP|MB_OK "Failed to stop the existing ReDesk service. The installation will be aborted."
        Abort
    ${EndIf}
    Sleep 1500
FunctionEnd

Function RegisterInstalledService
    IfFileExists "$INSTDIR\ReDesk.exe" 0 missing_service_binary
    IfFileExists "$INSTDIR\redesk_service.exe" 0 missing_service_binary
    IfFileExists "$INSTDIR\redesk_session_helper.exe" 0 missing_service_binary

    DetailPrint "Registering ReDesk service"
    ExecWait '"$INSTDIR\ReDesk.exe" --service-install' $0
    ${If} $0 != 0
        MessageBox MB_ICONSTOP|MB_OK "Failed to register the ReDesk service. The installation will be aborted."
        Abort
    ${EndIf}

    ; Preserve an existing unattended opt-in and resume it after upgrades.
    ; A fresh install still registers the ordinary on-demand lock-screen service.
    SetRegView 64
    ReadRegDWORD $1 HKLM "Software\ReDesk\Unattended" "Enabled"
    SetRegView 32
    ${If} $1 = 1
        ExecWait '"$INSTDIR\ReDesk.exe" --service-start' $0
        ${If} $0 != 0
            MessageBox MB_ICONEXCLAMATION|MB_OK "Could not restart the unattended host. Run ReDesk.exe --service-start from an administrator terminal."
        ${EndIf}
    ${EndIf}
    DetailPrint "ReDesk service registered"

    Return

missing_service_binary:
    MessageBox MB_ICONSTOP|MB_OK "ReDesk service files are missing from the installer package. The installation will be aborted."
    Abort
FunctionEnd

Function un.UnregisterInstalledService
    ; Also disarm unattended startup when removal must fall back to sc.exe.
    SetRegView 64
    DeleteRegKey HKLM "Software\ReDesk\Unattended"
    SetRegView 32
    IfFileExists "$INSTDIR\ReDesk.exe" 0 unregister_with_sc

    DetailPrint "Stopping ReDesk service"
    ExecWait '"$INSTDIR\ReDesk.exe" --service-stop' $0
    ${If} $0 = 0
        DetailPrint "Removing ReDesk service"
        ExecWait '"$INSTDIR\ReDesk.exe" --service-uninstall' $0
        ${If} $0 = 0
            Return
        ${EndIf}
    ${EndIf}

unregister_with_sc:
    DetailPrint "Removing ReDesk service via Service Control Manager"
    ExecWait '"$SYSDIR\sc.exe" stop ${PRODUCT_SERVICE_NAME}' $0
    ${If} $0 != 0
    ${AndIf} $0 != 1060
    ${AndIf} $0 != 1062
        MessageBox MB_ICONSTOP|MB_OK "Failed to stop the ReDesk service. Uninstall will be aborted."
        Abort
    ${EndIf}
    Sleep 1500

    ExecWait '"$SYSDIR\sc.exe" delete ${PRODUCT_SERVICE_NAME}' $0
    ${If} $0 != 0
    ${AndIf} $0 != 1060
        MessageBox MB_ICONSTOP|MB_OK "Failed to remove the ReDesk service. Uninstall will be aborted."
        Abort
    ${EndIf}
FunctionEnd

; The session helper (or an elevated ReDesk) installs the bundled usbmmidd
; driver on first headless use and records HKLM\SOFTWARE\ReDesk\
; UsbmmiddDriverInstalled in the 64-bit registry view. Only a driver ReDesk
; installed is removed here; one set up by the user for other software stays.
; deviceinstaller64.exe resolves usbmmIdd.dll relative to the working
; directory, so run it from the driver folder. Failures are logged, not fatal:
; the driver can still be removed from Device Manager.
Function un.RemoveUsbmmiddDriver
    ; A shared system driver must never be removed while another client uses it.
    Return
    IfFileExists "$INSTDIR\usbmmidd_v2\deviceinstaller64.exe" 0 done

    SetRegView 64
    ClearErrors
    ReadRegDWORD $0 HKLM "Software\${PRODUCT_NAME}" "UsbmmiddDriverInstalled"
    ${If} ${Errors}
    ${OrIf} $0 != 1
        SetRegView lastused
        Return
    ${EndIf}

    DetailPrint "Removing the usbmmidd virtual display driver installed by ReDesk"
    SetOutPath "$INSTDIR\usbmmidd_v2"
    ; Unplug every virtual monitor first (the driver allows up to four).
    StrCpy $1 0
unplug_loop:
    nsExec::ExecToLog '"$INSTDIR\usbmmidd_v2\deviceinstaller64.exe" enableidd 0'
    Pop $2
    IntOp $1 $1 + 1
    ${If} $2 = 0
    ${AndIf} $1 < 4
        Goto unplug_loop
    ${EndIf}
    nsExec::ExecToLog '"$INSTDIR\usbmmidd_v2\deviceinstaller64.exe" stop usbmmidd'
    Pop $2
    nsExec::ExecToLog '"$INSTDIR\usbmmidd_v2\deviceinstaller64.exe" remove usbmmidd'
    Pop $2
    ${If} $2 != 0
        DetailPrint "usbmmidd driver removal returned $2; remove 'USB Mobile Monitor Virtual Display' from Device Manager if it remains"
    ${EndIf}
    SetOutPath "$TEMP"

    DeleteRegValue HKLM "Software\${PRODUCT_NAME}" "UsbmmiddDriverInstalled"
    DeleteRegKey /ifempty HKLM "Software\${PRODUCT_NAME}"
    SetRegView lastused

done:
FunctionEnd
