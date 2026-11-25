#Requires -Version 5.1
<#
.SYNOPSIS
  Build the independent Non-Portable ReDesk Windows installer without touching
  the already-configured Portable Xmake project or its compiled object files.
.DESCRIPTION
  Uses Xmake external work directory (-P) and separate build directory (-o).
  Shares the user's globally installed Xmake packages/cache.
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1 -Version 1.7.6
.EXAMPLE
  .\scripts\build-installer.ps1 -Version 1.7.6 -BuildOnly
#>
[CmdletBinding()]
param(
  [string]$Version = "1.7.6",
  [switch]$BuildOnly,
  [switch]$Reconfigure
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$projectParent = Split-Path -Parent $projectRoot
$projectLeaf = Split-Path -Leaf $projectRoot

# Both directories are siblings of the source checkout, never inside its
# existing .xmake or build folder. Do not change XMAKE_GLOBALDIR.
$workDir = Join-Path $projectParent ($projectLeaf + "-installer-work")
$buildRoot = Join-Path $projectParent ($projectLeaf + "-installer-build")
$releaseDir = Join-Path $buildRoot "windows\x64\release"
$configMarker = Join-Path $workDir ".redesk-installer-config.txt"
$configDir = Join-Path $workDir ".xmake"

$expectedConfig = @(
  "project=$projectRoot",
  "build=$buildRoot",
  "version=$Version",
  "plat=windows",
  "arch=x64",
  "mode=release",
  "portable=false",
  "cuda=false"
) -join "`n"

$xmake = Get-Command xmake -ErrorAction SilentlyContinue
if (-not $xmake) { throw "xmake is not available in PATH." }

if (-not (Test-Path -LiteralPath $workDir)) {
  New-Item -ItemType Directory -Path $workDir -Force | Out-Null
}
if ((Test-Path -LiteralPath $configDir) -and
    -not (Test-Path -LiteralPath $configMarker)) {
  throw "Existing Xmake configuration at $workDir has no ReDesk installer marker. Refusing to overwrite it."
}

$mustConfigure = $Reconfigure -or -not (Test-Path -LiteralPath $configMarker)
if (-not $mustConfigure) {
  $previous = [System.IO.File]::ReadAllText($configMarker).Trim()
  if ($previous -ne $expectedConfig.Trim()) {
    throw "Installer Xmake configuration changed. Use -Reconfigure if the new version/settings are intentional. Existing Portable build is unchanged."
  }
}

Write-Host "ReDesk source:            $projectRoot"
Write-Host "Installer Xmake workdir:  $workDir"
Write-Host "Non-Portable build:       $buildRoot"
Write-Host "Shared Xmake packages:    existing user-level installation/cache"

Push-Location $workDir
try {
  if ($mustConfigure) {
    $settings = @(
      "f", "-P", $projectRoot, "-o", $buildRoot,
      "-p", "windows", "-a", "x64", "-m", "release",
      "--CROSSDESK_VERSION=$Version", "--USE_CUDA=false",
      "--CROSSDESK_PORTABLE=false", "-y"
    )
    Write-Host ("xmake " + ($settings -join " "))
    & $xmake.Source @settings
    if ($LASTEXITCODE -ne 0) {
      throw "Installer Xmake configuration failed with exit code $LASTEXITCODE"
    }
    [System.IO.File]::WriteAllText(
      $configMarker, $expectedConfig,
      (New-Object System.Text.UTF8Encoding($false)))
  }

  # Xmake remembers the -P source root in the external work directory.
  # This target builds the updater and service dependencies as Non-Portable.
  & $xmake.Source b crossdesk
  if ($LASTEXITCODE -ne 0) {
    throw "Non-Portable ReDesk compilation failed with exit code $LASTEXITCODE"
  }
}
finally {
  Pop-Location
}

if (-not (Test-Path -LiteralPath (Join-Path $releaseDir "redesk_updater.exe") -PathType Leaf)) {
  throw "Non-Portable build completed but redesk_updater.exe is missing from $releaseDir"
}

if ($BuildOnly) {
  Write-Host "Non-Portable build OK. Installer packaging skipped (-BuildOnly)."
  Write-Host "Build output: $releaseDir"
  return
}

$packageScript = Join-Path $projectRoot "apps\desktop\scripts\windows\pkg_x64.ps1"
& $packageScript -Version $Version -ReleaseDirectory $releaseDir
$packageName = "redesk-win-x64-$Version.exe"
$packageFile = Join-Path (Split-Path -Parent $packageScript) $packageName
if (-not (Test-Path -LiteralPath $packageFile -PathType Leaf)) {
  throw "NSIS exited successfully, but expected installer does not exist: $packageFile"
}
Write-Host "Installer package output: $packageFile"
