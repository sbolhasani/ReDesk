#Requires -Version 5.1
<#
.SYNOPSIS
    ReDesk Windows build: reuse existing Xmake configuration, packages and objects.
.EXAMPLE
    .\scripts\build.ps1
.EXAMPLE
    .\scripts\build.ps1 -Action Prepare
.EXAMPLE
    .\scripts\build.ps1 -Action Configure
#>
[CmdletBinding()]
param(
    [ValidateSet("Build", "Configure", "Prepare", "Inspect")]
    [string]$Action = "Build",
    [string]$Target = "crossdesk"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $root

function Prepare-Dav1d {
    $recipe = Join-Path $root "deps\submodules\minirtc\thirdparty\dav1d\xmake.lua"
    if (-not (Test-Path -LiteralPath $recipe)) {
        throw "Missing MiniRTC dav1d recipe. Initialize the pinned Git submodule first."
    }
    $text = [System.IO.File]::ReadAllText($recipe)
    if ($text.Contains('#ifndef __has_include_next')) {
        Write-Host "dav1d MSVC compatibility fix already present. No changes."
        return
    }
    $needle = '        import("package.tools.meson")'
    if (-not $text.Contains($needle)) {
        throw "Unknown dav1d recipe layout; refusing to modify it."
    }
    $insert = @'
        -- dav1d 1.4.3 needs a compatibility guard with MSVC 19.52.
        -- Official upstream fix: videolan/dav1d a34e0685b9111b9eb1d7835ea7bea5b425dced73.
        if package:is_plat("windows") then
            local header = "include/compat/msvc/stdatomic.h"
            local content = io.readfile(header)
            assert(content, "dav1d MSVC stdatomic.h not found")
            if not content:find("#ifndef __has_include_next", 1, true) then
                io.replace(header,
                    "#if defined(__clang__) && __has_include_next(<stdatomic.h>)",
                    "#ifndef __has_include_next\n" ..
                    "#define __has_include_next(x) 0\n" ..
                    "#endif\n\n" ..
                    "#if defined(__clang__) && __has_include_next(<stdatomic.h>)",
                    {plain = true})
            end
        end

        import("package.tools.meson")
'@
    # This modifies only the pinned local submodule recipe, never an installed library.
    $patched = $text.Replace($needle, $insert)
    [System.IO.File]::WriteAllText($recipe, $patched, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Prepared dav1d recipe for a future cold build."
    Write-Warning "The pinned MiniRTC submodule is now locally modified. Keep this patch; do not reset the submodule."
}

function Invoke-Xmake([string[]]$arguments) {
    $cmd = Get-Command xmake -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "xmake not found in PATH. Use Developer PowerShell with Xmake installed." }
    Write-Host ("xmake " + ($arguments -join " "))
    & $cmd.Source @arguments
    if ($LASTEXITCODE -ne 0) { throw "xmake failed with exit code $LASTEXITCODE" }
}

switch ($Action) {
    "Prepare" {
        Prepare-Dav1d
        Write-Host "Dependency preparation complete. No packages were downloaded or rebuilt."
    }
    "Configure" {
        # No -c: retain local configuration and globally installed Xmake packages.
        # Run this only for a fresh checkout or a deliberately changed build configuration.
        Prepare-Dav1d
        Invoke-Xmake @("f", "-p", "windows", "-a", "x64", "-m", "release",
            "--USE_CUDA=false", "--CROSSDESK_PORTABLE=true", "-y")
    }
    "Inspect" {
        Invoke-Xmake @("show", "-t", $Target)
        & git status --short
    }
    "Build" {
        # NEVER configure, clean, or reinstall here.
        Invoke-Xmake @("b", $Target)
    }
}
