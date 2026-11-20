param(
  [Parameter(Mandatory = $true)]
  [string]$Version,
  # Absolute output folder, e.g. CrossDesk-Upstream-installer-build\windows\x64\release.
  [string]$ReleaseDirectory = ""
)

$ErrorActionPreference = "Stop"

function Normalize-AppVersion {
  param(
    [Parameter(Mandatory = $true)]
    [string]$InputVersion
  )

  $prefix = ""
  $body = $InputVersion

  if ($body.StartsWith("v")) {
    $prefix = "v"
    $body = $body.Substring(1)
  }

  if ($body -match '^([0-9]+(\.[0-9]+){1,3})-([0-9]{8})-([0-9]+)$') {
    return "${prefix}$($Matches[1])-$($Matches[4])-$($Matches[3])"
  }

  return $InputVersion
}

function Convert-To-WindowsVersion {
  param(
    [Parameter(Mandatory = $true)]
    [string]$InputVersion
  )

  $body = $InputVersion -replace '^v', ''
  $baseMatch = [regex]::Match($body, '^[0-9]+(?:\.[0-9]+){0,3}')
  if (!$baseMatch.Success) {
    return "0.0.0.0"
  }

  $parts = [System.Collections.Generic.List[int]]::new()
  foreach ($part in $baseMatch.Value.Split('.')) {
    $value = [int]$part
    if ($value -gt 65535) {
      $value = 0
    }
    $parts.Add($value)
  }
  while ($parts.Count -lt 4) {
    $parts.Add(0)
  }

  return ($parts -join '.')
}

$normalizedVersion = Normalize-AppVersion -InputVersion $Version
$windowsVersion = Convert-To-WindowsVersion -InputVersion $normalizedVersion
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = (Resolve-Path (Join-Path $scriptDir "..\..\..\..")).Path
$releaseDir = if ([string]::IsNullOrWhiteSpace($ReleaseDirectory)) {
  Join-Path $projectRoot "build\windows\x64\release"
} elseif ([System.IO.Path]::IsPathRooted($ReleaseDirectory)) {
  $ReleaseDirectory
} else {
  Join-Path $projectRoot $ReleaseDirectory
}
$releaseDir = [System.IO.Path]::GetFullPath($releaseDir)

# Validate packaging inputs before invoking NSIS. A portable build deliberately
# omits the updater and cannot be packaged as a non-portable installation.
$requiredFiles = @(
  "ReDesk.exe",
  "redesk_updater.exe",
  "redesk_service.exe",
  "redesk_session_helper.exe",
  "redesk_privacy_cursor_helper.exe",
  "redesk_privacy_window.dll",
  "slint_cpp.dll"
)
$missingFiles = @(
  foreach ($name in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $releaseDir $name) -PathType Leaf)) {
      $name
    }
  }
)
if (-not (Test-Path -LiteralPath (Join-Path $releaseDir "usbmmidd_v2") -PathType Container)) {
  $missingFiles += "usbmmidd_v2 (directory)"
}
if ($missingFiles.Count -gt 0) {
  throw ("Cannot package the ReDesk installer. Missing build outputs in {0}: {1}. " +
    "If redesk_updater.exe is missing, the current Xmake configuration is probably Portable. " +
    "Do not run xmake f -c on your working Portable build; use a separate Non-Portable configuration.") -f
      $releaseDir, ($missingFiles -join ", ")
}

# NSIS may be installed but not in PATH (common with winget installs).
$makensisCommand = Get-Command makensis.exe -ErrorAction SilentlyContinue
$makensisPath = if ($makensisCommand) { $makensisCommand.Source } else { $null }
if (-not $makensisPath) {
  foreach ($parent in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
    if ([string]::IsNullOrWhiteSpace($parent)) { continue }
    $candidate = Join-Path (Join-Path $parent "NSIS") "makensis.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      $makensisPath = $candidate
      break
    }
  }
}
if (-not $makensisPath) {
  throw "makensis.exe was not found in PATH, Program Files (x86)\\NSIS, or Program Files\\NSIS. Install NSIS or add its installation directory to PATH."
}

Write-Host "NSIS compiler: $makensisPath"
Write-Host "ReDesk release inputs: $releaseDir"
Push-Location $scriptDir
try {
  & $makensisPath "/DVERSION=$normalizedVersion" "/DVERSION_NUMERIC=$windowsVersion" "/DREDESK_RELEASE_DIR=$releaseDir" "nsis_script.nsi"
  if ($LASTEXITCODE -ne 0) {
    throw "NSIS packaging failed with exit code $LASTEXITCODE"
  }
} finally {
  Pop-Location
}
