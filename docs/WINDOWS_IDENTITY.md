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

## Localization regression after renaming the Windows executable

The first Windows portable test confirmed the renamed executables build and run,
but the main UI opened in Chinese. Two root causes were found:

1. The brand-independent portable path manager pointed **both CrossDesk and
   ReDesk** at the same `data/config.ini` directory alongside the executable.
   The newly named `ReDesk.exe` could therefore inherit an old CrossDesk
   `language=0` (Chinese) preference.
2. On a first run with no cached credential identity, `SettingsManager::Load()`
   returns early before transferring the configured language into the GUI's
   language-button state (which also defaulted to Chinese).

Windows ReDesk portable builds now use **`redesk-data`** and
**`redesk-logs`** beside the executable, and create these directories before
reading or writing settings. Existing `data` and `logs` are not moved,
overwritten or deleted. The GUI initializes its language preference before the
credential cache early return. English remains the default on a fresh install,
while a user-selected existing ReDesk language remains honored.

### Targeted regression test

```powershell
git pull --ff-only origin work/redesk-windows-identity-02
.\scripts\build.ps1
xmake r crossdesk
```

Confirm **English** with no prior `redesk-data/config.ini` file. The new
`redesk-data/config.ini` should contain `language=1` when saved, and the
old `data/config.ini` should be unchanged. Then select Chinese in ReDesk,
exit using the tray menu and restart: the selected language should persist.
Restore English afterwards if preferred.

No dependency cache reset, package rebuild, service install or NSIS
installation is required. If you had already saved useful ReDesk credentials
or settings to the legacy shared `data` directory, export or back those up
first; this patch **does not silently copy sensitive identity files**.
