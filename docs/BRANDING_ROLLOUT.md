# ReDesk branding rollout

This phase changes the user-facing **desktop window titles, About-page product name, and Windows main-executable version metadata**. It deliberately retains CrossDesk's internal identifiers, default target name, and binary filename during the first step to maximize reuse of the already-built Windows desktop and MiniRTC objects.

## Remaining work before installing ReDesk alongside CrossDesk

- The Windows service, session helper, updater, autostart, task names, IPC/mutex identity, and portable application storage still use upstream CrossDesk identifiers. Running both apps simultaneously is **not supported yet**.
- Non-portable configuration paths currently share upstream CrossDesk paths; do not use this build for parallel installations or overwrite production CrossDesk settings.
- ReDesk still shows the upstream artwork, software publisher notices and some upstream brand references. Existing third-party copyright and license attributions must be preserved.
- The upstream app updater/version checker must be fully isolated before a public ReDesk release.
- The desktop's Xmake target remains `crossdesk` and the resulting executable is `crossdesk.exe`; this is intentional for incremental build reuse. Rename all cooperating helper targets and references together in a later change.
- UI changes to `main_window.slint`, `stream_window.slint`, and `server_window.slint` necessarily regenerate Slint output and rebuild dependent GUI objects.

## Testing

On the existing Windows checkout, without resetting build/cache:

```powershell
git fetch origin
git switch --track -c work/redesk-branding-01 origin/work/redesk-branding-01
.\scripts\build.ps1
xmake r crossdesk
```

Verify: main window title, remote stream window, server window, About page, and Windows Explorer executable properties now display ReDesk. Verify existing connection remains usable.

Do not merge until runtime verification. A separate identity/isolation PR will change Windows service and configuration identifiers together.
