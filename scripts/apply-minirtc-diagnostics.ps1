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
    Write-Warning "MiniRTC contains existing tracked changes. They will be preserved; applying only if the patch preflight succeeds."
    # A clean working tree is not required. git apply --check verifies every
    # hunk against the CURRENT working-tree files before making any changes.
    # If earlier edits overlap a hunk, this script stops instead of resetting.
}

& git -C $mini apply --check $patch
if ($LASTEXITCODE -ne 0) {
    & git -C $mini apply --reverse --check $patch 2>$null
    if ($LASTEXITCODE -eq 0) {
        throw "Diagnostic patch appears to be already applied. No changes made."
    }
    throw "Patch conflicts with existing MiniRTC edits or source version. Nothing was applied. Inspect: git -C deps/submodules/minirtc diff --stat"
}
& git -C $mini apply $patch
if ($LASTEXITCODE -ne 0) { throw "Patch application failed; inspect MiniRTC status. No reset was attempted." }
& git -C $mini diff --check
if ($LASTEXITCODE -ne 0) {
    Write-Warning "git diff --check reported whitespace issues; these may come from pre-existing changes. Diagnostics patch was applied."
}

Write-Host "Temporary MiniRTC TURN/TCP diagnostic patch applied." -ForegroundColor Green
Write-Host "The submodule commit and .gitmodules remain unchanged."
Write-Host "Build ReDesk using the existing Xmake configuration: xmake -y"
