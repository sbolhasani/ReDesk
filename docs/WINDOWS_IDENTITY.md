# ReDesk Windows executable and service identity (phase 2b)

The purpose of this change is to give Windows ReDesk an independent executable,
SCM service, session helper, secure-desktop IPC namespace and user/machine
configuration identity while **preserving the Xmake target names and installed
dependency caches**.

## Windows artifacts

The Xmake targets remain `crossdesk`, `crossdesk_service`,
`crossdesk_session_helper`, `crossdesk_updater`,
`crossdesk_privacy_window` and `crossdesk_privacy_cursor_helper`, but
Windows target basenames now produce `ReDesk.exe`,
`redesk_service.exe`, `redesk_session_helper.exe`,
`redesk_updater.exe`, `redesk_privacy_window.dll`, and
`redesk_privacy_cursor_helper.exe`, respectively. The shared
`wgc_plugin.dll` and Slint dependencies remain in the application's own
directory, with their upstream binary names.

## Identity boundaries

- Windows SCM service: `ReDeskService`
- Registry unattended configuration: `HKLM\\SOFTWARE\\ReDesk\\Unattended`
- Privileged machine profile: `%ProgramData%\\ReDeskUnattended`
- User profile: `ReDesk` under AppData (already changed in the previous PR)
- Service pipes, session-helper pipes, named events, named mappings, local host
  channel, privacy service helpers and window/taskbar identity: ReDesk
- NSIS installer: separate `ReDesk` product and uninstall registration
- Existing CrossDesk data, credentials and registered service are **not migrated**
  or modified.

The C++ `crossdesk` namespace, API symbols, Xmake target names, protocol
labels and upstream source attribution remain unchanged intentionally. Changing
them would create unnecessary binary and wire-protocol incompatibility.

## Important release safeguards

**Windows update checks are intentionally disabled** until ReDesk owns its
release manifest, package hosting and signing key. The old CrossDesk update
manifest must never trigger installing upstream binaries over ReDesk.

**usbmmidd is a shared Windows virtual-display driver.** ReDesk's NSIS
uninstaller intentionally does not remove this driver, to avoid breaking any
remaining CrossDesk installation. Shared driver lifetime and installation
ownership are not fully implemented. Use separate program directories.

**Do not install or uninstall ReDesk via NSIS until Windows tests of this PR
have passed.** Neither compiled service behavior nor NSIS packaging has been
tested through the GitHub connection; side-by-side installation safety is not
yet confirmed.

ReDesk currently retains the upstream icon and some in-app upstream URLs.
Those are separate branding tasks and do not authorize replacing GPL notices.

## Windows tests

On the existing development checkout, with all ReDesk instances closed:

```powershell
git fetch origin
git switch --track -c work/redesk-windows-identity-02 origin/work/redesk-windows-identity-02
python scripts/check_windows_identity.py
.\\scripts\\build.ps1
xmake r crossdesk
```

`xmake b crossdesk` is still the normal incremental build. Renaming the Windows
target *basename* will require relinking some executables but should not force
downloads or recompilation of already-compatible third-party packages.
Never use `xmake f -c` or remove the package cache merely to test this change.

After a successful portable build, inspect `build\\windows\\x64\\release`
and confirm that `ReDesk.exe` and the new helper executables are present.
Check fresh startup, English, second-instance activation, tray behavior,
connections, and user AppData isolation.

A separate **privileged test** must verify service install, registration,
start, status, secure input/session helper operation and uninstall without
affecting an existing CrossDesk service. Test NSIS install/uninstall on a
throwaway Windows VM/snapshot before public release.

The previously observed RDP-only Slint repaint problem remains separately
documented and unresolved.
