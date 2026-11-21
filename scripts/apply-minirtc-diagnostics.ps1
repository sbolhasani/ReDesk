# Temporary MiniRTC diagnostics. Does not update the gitlink or dependency cache.
$ErrorActionPreference = 'Stop'
$expected = '9b1acc206b84ad8f31b9466a213ea6777ca8aa18'
$root = Split-Path -Parent $PSScriptRoot
$mini = Join-Path $root 'deps\submodules\minirtc'
$patch = Join-Path $root 'diagnostics\minirtc-turn-tcp.patch'

if (-not (Test-Path (Join-Path $mini '.git'))) {
    throw "MiniRTC submodule is not initialized. Run: git submodule update --init --recursive"
}
if (-not (Test-Path $patch)) { throw "Patch not found: $patch" }

$head = (& git -C $mini rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $head -ne $expected) {
    throw "Wrong MiniRTC commit ($head). Expected $expected. Do not reset local changes."
}
$dirty = @(& git -C $mini status --porcelain --untracked-files=no)
if ($LASTEXITCODE -ne 0) { throw "Unable to read MiniRTC status." }
if ($dirty.Count -gt 0 -and ($dirty -join '').Trim().Length -gt 0) {
    throw "MiniRTC already has tracked local changes. Preserve them before applying diagnostics."
}

& git -C $mini apply --check $patch
if ($LASTEXITCODE -ne 0) { throw "Patch preflight failed. Nothing was applied." }
& git -C $mini apply $patch
if ($LASTEXITCODE -ne 0) { throw "Patch application failed; inspect MiniRTC status." }
& git -C $mini diff --check
if ($LASTEXITCODE -ne 0) { throw "Whitespace validation failed; inspect the working tree." }

Write-Host "Temporary MiniRTC TURN/TCP diagnostic patch applied." -ForegroundColor Green
Write-Host "The submodule commit and .gitmodules remain unchanged."
Write-Host "Build ReDesk using the existing Xmake configuration: xmake -y"
