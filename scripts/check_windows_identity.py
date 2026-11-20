#!/usr/bin/env python3
"""Static Windows identity consistency check for ReDesk's CrossDesk fork.

Run with: python scripts/check_windows_identity.py
Does not require Xmake, download packages, or modify any build artifacts.
This is not a substitute for Windows GUI/SCM integration tests.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


checks = {
    "apps/desktop/src/platform/windows/service/service_host.h": [
        'L"ReDeskService"', r'pipe\\ReDeskService',
    ],
    "apps/desktop/src/platform/windows/service/session_helper_shared.h": [
        r'pipe\\ReDeskSessionHelper-', r'pipe\\ReDeskSecureInputHelper-',
        r'Global\\ReDeskSecureDesktopFrame-',
    ],
    "apps/desktop/src/platform/windows/service/unattended_config.cpp": [
        r'SOFTWARE\\ReDesk\\Unattended', 'L"ReDeskUnattended"',
    ],
    "apps/desktop/src/platform/windows/service/local_host_channel.cpp": [
        r'pipe\\ReDesk.Host.',
    ],
    "apps/desktop/src/platform/windows/service/service_host.cpp": [
        'L"redesk.exe"', 'L"ReDesk.exe"', 'L"redesk_session_helper.exe"',
        r'Global\\ReDeskUnattendedStop-', r'Global\\ReDeskServiceLifetime-',
    ],
    "apps/desktop/src/platform/windows/service/session_helper_main.cpp": [
        'L"ReDesk.exe"',
    ],
    "apps/desktop/src/platform/windows/privacy/privacy_band_ipc.h": [
        'L"redesk_privacy_window.dll"',
    ],
    "apps/desktop/src/platform/windows/privacy/privacy_cursor_guard.cpp": [
        'L"redesk_privacy_cursor_helper.exe"',
    ],
    "apps/desktop/src/platform/windows/updater/update_platform.cpp": [
        'L"ReDeskService"', 'L"redesk_updater.exe"',
        r'Uninstall\\ReDesk',
    ],
    "apps/desktop/xmake/targets.lua": [
        'target("crossdesk")', 'set_basename("ReDesk")',
        'set_basename("redesk_service")', 'set_basename("redesk_session_helper")',
        'set_basename("redesk_updater")',
        'set_basename("redesk_privacy_window")',
        'set_basename("redesk_privacy_cursor_helper")',
    ],
    "apps/desktop/scripts/windows/nsis_script.nsi": [
        '!define PRODUCT_NAME "ReDesk"', '!define PRODUCT_SERVICE_NAME "ReDeskService"',
        'File /oname=ReDesk.exe', 'redesk_service.exe',
        'redesk_session_helper.exe', '$APPDATA\\ReDesk', '$LOCALAPPDATA\\ReDesk',
    ],
    "apps/desktop/src/version_checker/version_checker.cpp": [
        '#if defined(_WIN32)', 'return std::nullopt;',
    ],
}

banned_values = (
    '"CrossDeskService"', r'pipe\\CrossDesk',
    r'Global\\CrossDesk', 'L"crossdesk_service.exe"',
    'L"crossdesk_session_helper.exe"',
    'L"CrossDesk.exe"', 'L"crossdesk.exe"',
    r'SOFTWARE\\CrossDesk\\Unattended',
)

failures: list[str] = []
for path, tokens in checks.items():
    content = source(path)
    for token in tokens:
        if token not in content:
            failures.append(f"{path}: missing {token!r}")

# Only scan files which need independent runtime identity, not upstream
# C++ namespaces, component IDs, test fixtures, or copyright/license notices.
critical = [
    "apps/desktop/src/app/main.cpp",
    "apps/desktop/src/platform/windows/service/service_host.cpp",
    "apps/desktop/src/platform/windows/service/service_host.h",
    "apps/desktop/src/platform/windows/service/unattended_config.cpp",
    "apps/desktop/src/platform/windows/service/session_helper_shared.h",
    "apps/desktop/src/platform/windows/service/session_helper_main.cpp",
    "apps/desktop/src/platform/windows/service/autostart_control.cpp",
    "apps/desktop/src/platform/windows/service/autostart_state.cpp",
    "apps/desktop/src/platform/windows/updater/update_platform.cpp",
]
for path in critical:
    content = source(path)
    for old in banned_values:
        if old in content:
            failures.append(f"{path}: old runtime identity {old!r}")

nsis = source("apps/desktop/scripts/windows/nsis_script.nsi")
if "    Call un.RemoveUsbmmiddDriver" in nsis:
    failures.append("NSIS must not uninstall the shared usbmmidd display driver")
if "Copyright (C) CrossDesk contributors" not in nsis:
    failures.append("NSIS must preserve the upstream CrossDesk attribution")

if failures:
    for failure in failures:
        print("FAIL:", failure)
    raise SystemExit(1)
print(f"PASS: {len(checks)} Windows identity groups; no known old runtime names")
