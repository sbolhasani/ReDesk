# Remove only the v2 additions. v1 diagnostics and unrelated edits stay intact.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$mini = Join-Path $root 'deps\submodules\minirtc'
$v2 = Join-Path $root 'diagnostics\minirtc-turn-tcp-v2.patch'
if (-not (Test-Path (Join-Path $mini '.git'))) { throw "MiniRTC submodule is not initialized." }
& git -C $mini apply --reverse --check $v2
if ($LASTEXITCODE -ne 0) {
    throw "Reverse check failed: v2 absent or edited further. Nothing was reset."
}
& git -C $mini apply --reverse $v2
if ($LASTEXITCODE -ne 0) { throw "Reverse apply failed; inspect the local working tree." }
Write-Host "MiniRTC diagnostics v2 removed. Version 1 remains applied." -ForegroundColor Green
