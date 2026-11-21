# Add-on to the already applied v1 MiniRTC diagnostic patch.
# No reset, stash, checkout, submodule update, or change to the upstream gitlink.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$mini = Join-Path $root 'deps\submodules\minirtc'
$v1 = Join-Path $root 'diagnostics\minirtc-turn-tcp.patch'
$v2 = Join-Path $root 'diagnostics\minirtc-turn-tcp-v2.patch'
$expected = '9b1acc206b84ad8f31b9466a213ea6777ca8aa18'
if (-not (Test-Path (Join-Path $mini '.git'))) { throw "MiniRTC submodule is not initialized." }
$head = (& git -C $mini rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $head -ne $expected) { throw "Unexpected MiniRTC HEAD: $head" }
& git -C $mini apply --reverse --check $v1
if ($LASTEXITCODE -ne 0) {
    throw "Diagnostic v1 patch must already be applied. Run scripts/apply-minirtc-diagnostics.ps1 first."
}
& git -C $mini apply --check $v2
if ($LASTEXITCODE -ne 0) {
    throw "Diagnostic v2 patch conflicts with existing files or is already applied. No changes made."
}
& git -C $mini apply $v2
if ($LASTEXITCODE -ne 0) { throw "Could not apply diagnostics v2; no reset attempted." }
Write-Host "MiniRTC TURN/TCP diagnostics v2 applied on top of v1." -ForegroundColor Green
Write-Host "Existing unrelated MiniRTC changes were preserved. Run: xmake -y"
