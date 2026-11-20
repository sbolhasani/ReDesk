# ReDesk desktop identity: first isolation step

This change separates ReDesk's desktop application identity from CrossDesk without modifying MiniRTC or rebuilding dependency packages.

## Scope

- Default desktop single-instance mutex application: `ReDesk`
- Windows activation window class/message and per-user activation name: `ReDesk`
- Per-user configuration/cache/log storage app name passed to `PathManager`: `ReDesk`
- Tray branding: `ReDesk`
- Windows autostart task/Run-entry application name: `ReDesk` on paths already using the app name

The internal `crossdesk` C++ namespace, binary target, and filenames are deliberately unchanged to minimize impact on existing build artifacts.

## Critical limitation

**Service integration is not yet isolated.** Windows service name, service IPC, ProgramData machine credentials, unattended host, and helper process names still belong to CrossDesk. Do not install or enable the ReDesk Windows service or unattended mode, and do not attempt side-by-side installations, until the coordinated service identity change has been completed and tested. For this first step, run the already-tested portable desktop build without enabling the service.

User configuration of a non-portable ReDesk now resides under the separate ReDesk AppData path. This is an intentional fresh profile; CrossDesk settings should not be auto-migrated.

Portable builds still use `data` and `logs` beside the executable. Running different binaries from the same directory would still share that portable data; use separate directories if running separate products.

## Windows validation

```powershell
git fetch origin
git switch --track -c work/redesk-identity-01 origin/work/redesk-identity-01
.\scripts\build.ps1
xmake r crossdesk
```

Check fresh desktop startup and English language, open a second copy and verify it activates the first instance, verify tray behavior, and confirm that no service was installed or enabled. Do not reset the MiniRTC submodule or Xmake cache.

The build/runtime validation of these changes has not yet been performed on the user's machine.
