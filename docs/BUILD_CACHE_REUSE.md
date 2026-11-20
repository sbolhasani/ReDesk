# ReDesk Windows builds: reusing CrossDesk work

The initial ReDesk baseline is exactly CrossDesk `v1.7.6-20261112` including MiniRTC `9b1acc206b84ad8f31b9466a213ea6777ca8aa18`.

## Existing development machine (fastest)

Keep working in the **same directory** used for the successful CrossDesk build. The directory may remain named `CrossDesk-Upstream`; it now tracks ReDesk's `origin` and CrossDesk's `upstream`.

In Developer PowerShell:

```powershell
git status --short
git pull --ff-only origin work/redesk-foundation-01
.\scripts\build.ps1
```

The default action is equivalent to `xmake b crossdesk`, with **no** `xmake f -c`, package reinstall, cache clearing, or forced rebuild. Keep the existing `.xmake`, `build`, and `%LOCALAPPDATA%\.xmake\packages` directories.

An edited MiniRTC `thirdparty/dav1d/xmake.lua` is expected on the original development machine. Do not discard that local edit; the `Prepare` action is idempotent and leaves it untouched if the known guard exists.

An updated project-level Xmake configuration can trigger a small reconfiguration/resolution step on first use. Existing packages and valid C/C++ objects are still eligible for reuse; **zero** new compilation or downloads cannot be guaranteed.

## Clean Windows machine

```powershell
git clone --recursive --branch work/redesk-foundation-01 https://github.com/sbolhasani/ReDesk.git
cd ReDesk
.\scripts\build.ps1 -Action Configure
.\scripts\build.ps1
```

The `Configure` action makes only necessary source compatibility changes in the local MiniRTC recipe, and configures Release / x64 / Portable / no CUDA **without using `-c`**. The `Build` action does not configure at all.

On Windows, the project registers a project-owned Xmake recipe for `usrsctp 0.9.5+0`, with the upstream source archive checksum unchanged, but without two Apple/iOS-only patch downloads. No checksum checks are globally disabled. Other platforms continue to use their upstream recipes.

If you deliberately need new build options, choose `-Action Configure`. Do not switch Debug/Release, CRT, compiler version, architecture or dependency versions unnecessarily: those legitimately change binary compatibility and may require recompilation.

## Safety and maintenance

- **Never** run `git submodule update --force` or `git reset --hard` to clean local MiniRTC compatibility patches.
- Do not delete either Xmake's installed package directory or its download cache to solve an ordinary compilation error.
- Do not share the same mutable `build` output directory between two parallel Xmake processes.
- The Windows `usrsctp` override and dav1d preparation are deliberately scoped to the initial pinned CrossDesk baseline. Review these when importing newer MiniRTC, which already includes bundled usrsctp patches.
- The exact impact of the overrides and the clean-machine path still require Windows build validation. The original machine has already built successfully with equivalent local fixes.

## Testing

```powershell
git status --short
.\scripts\build.ps1 -Action Inspect
.\scripts\build.ps1
xmake r crossdesk
```

Avoid `xmake f -c` unless intentionally discarding the local build configuration.
