# Reverse only the documented MiniRTC diagnostics patch; never reset the submodule.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$mini = Join-Path $root 'deps\submodules\minirtc'
$patch = Join-Path $root 'diagnostics\minirtc-turn-tcp.patch'
if (-not (Test-Path (Join-Path $mini '.git'))) {
    throw "MiniRTC submodule is missing."
}
if (-not (Test-Path $patch)) { throw "Patch not found: $patch" }

& git -C $mini apply --reverse --check $patch
if ($LASTEXITCODE -ne 0) {
    throw "Reverse preflight failed: patch absent or affected lines changed. No reset performed."
}
& git -C $mini apply --reverse $patch
if ($LASTEXITCODE -ne 0) { throw "Reverse application failed; inspect local changes." }
& git -C $mini diff --check
if ($LASTEXITCODE -ne 0) { throw "Whitespace validation failed; inspect working tree." }
Write-Host "TURN/TCP diagnostics removed; other local changes are preserved." -ForegroundColor Green
