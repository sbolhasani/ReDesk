<#
ReDesk Cloudflare Bootstrapper. PowerShell 5.1+, Node.js 20+.
Run locally; do NOT send your token to the developer or to a chat.
Examples:
  .\setup-redesk-cloudflare.ps1
  .\setup-redesk-cloudflare.ps1 -Mode Update -AccountId <account>
  .\setup-redesk-cloudflare.ps1 -Mode Check -AccountId <account>
#>
[CmdletBinding()]
param(
    [ValidateSet("Auto","Install","Update","Check")] [string]$Mode="Auto",
    [string]$AccountId="",
    [switch]$AdoptExistingWorker,
    [string]$WorkerUrl=""
)
$ErrorActionPreference="Stop"
$ProgressPreference="SilentlyContinue"
$workerName="redesk-signal"
$root=$PSScriptRoot
$api="https://api.cloudflare.com/client/v4"
$stateDir=Join-Path $env:LOCALAPPDATA "ReDesk\Cloudflare"
$apiToken=$null
$turnSecret=$null

function Read-Private([string]$prompt) {
    $sec=Read-Host $prompt -AsSecureString
    $ptr=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try { [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}
function Cf([string]$method,[string]$path,$body=$null) {
    $args=@{Uri="$api$path";Method=$method;Headers=@{Authorization="Bearer $apiToken"};ErrorAction="Stop"}
    if ($null -ne $body) {
        $args.ContentType="application/json"
        $args.Body=($body|ConvertTo-Json -Compress -Depth 5)
    }
    try { $response=Invoke-RestMethod @args }
    catch { throw "Cloudflare API call failed ($method $path): check token rights and account availability." }
    if (-not $response.success) { throw "Cloudflare API rejected $method $path." }
    return $response.result
}
function Health([string]$url) {
    if ($url -notmatch '^https://[a-zA-Z0-9.-]+(/.*)?$') { throw "Supply a valid HTTPS Worker URL via -WorkerUrl." }
    $base=$url.TrimEnd('/') -replace '/signal$',''
    try { $reply=Invoke-RestMethod -Uri "$base/health" -TimeoutSec 15 }
    catch { throw "The deployed Worker /health endpoint is unreachable." }
    if (-not $reply.ok -or $reply.service -ne "ReDesk signaling") {
        throw "Unexpected health response: do not overwrite unrelated Workers."
    }
    Write-Host "Worker health OK; TURN configured: $($reply.turn_configured)" -ForegroundColor Green
    return $base
}
function WorkerSecret([string]$name,[string]$value) {
    if (-not $value) { throw "Refusing to put an empty secret." }
    # Passing via stdin avoids placing credentials in command line args.
    $value | & npx --no-install wrangler secret put $name 1>$null
    if ($LASTEXITCODE -ne 0) { throw "Wrangler could not configure $name." }
}
function Test-Turn([string]$id,[string]$secret) {
    $endpoint="https://rtc.live.cloudflare.com/v1/turn/keys/$id/credentials/generate-ice-servers"
    $args=@{Uri=$endpoint;Method="Post";Headers=@{Authorization="Bearer $secret"};ContentType="application/json";Body='{"ttl":3600}';ErrorAction="Stop"}
    try {
        $response=Invoke-RestMethod @args
        $turns=@($response.iceServers | Where-Object { (@($_.urls | Where-Object { $_ -like 'turn:*' })).Count -gt 0 })
        if ($turns.Count -eq 0) { throw "TURN endpoints unavailable" }
    } catch { throw "TURN credential test failed; verify Cloudflare Realtime access and billing." }
    Write-Host "Cloudflare TURN credentials: OK" -ForegroundColor Green
}

try {
    if (-not (Test-Path (Join-Path $root "wrangler.toml"))) { throw "Run the bundled cloudflare/signal setup script." }
    foreach($cmd in @("node","npm","npx")) {
        if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { throw "Node.js 20+ is required." }
    }
    if ([int](& node -p "process.versions.node.split('.')[0]") -lt 20) { throw "Node.js 20+ required." }
    if ($Mode -ne "Check") {
        $apiToken=$env:CLOUDFLARE_API_TOKEN
        if (-not $apiToken) { $apiToken=Read-Private "Cloudflare API Token (Workers Scripts Edit + Calls Write)" }
        if (-not $apiToken) { throw "API Token required." }
    }
    if (-not $AccountId) { $AccountId=$env:CLOUDFLARE_ACCOUNT_ID }
    if (-not $AccountId -and $Mode -ne "Check") {
        try {
            $accounts=@(Cf "GET" "/accounts?per_page=50")
            if ($accounts.Count -eq 1) { $AccountId=$accounts[0].id }
        } catch { }
    }
    if (-not $AccountId) { $AccountId=Read-Host "Cloudflare Account ID (32 hex chars)" }
    if ($AccountId -notmatch '^[a-fA-F0-9]{32}$') { throw "Invalid Account ID." }
    $statePath=Join-Path $stateDir "$AccountId.json"
    $state=$null
    if (Test-Path $statePath) {
        $state=Get-Content $statePath -Raw | ConvertFrom-Json
        if ($state.account_id -ne $AccountId -or $state.worker_name -ne $workerName) { throw "Stored account/Worker mismatch." }
    }
    if ($Mode -eq "Auto") { if($state){$Mode="Update"}else{$Mode="Install"} }
    if ($Mode -eq "Check") {
        $url=if($WorkerUrl){$WorkerUrl}elseif($state){$state.worker_url}else{""}
        $verified=Health $url
        Write-Host "Signal host: $($verified -replace '^https://','')/signal; port 443"
        return
    }
    if ($Mode -eq "Update" -and -not $state) {
        throw "No local state. Use -Mode Install -AdoptExistingWorker to explicitly adopt an existing ReDesk Worker."
    }
    if ($Mode -eq "Install" -and -not $state -and -not $AdoptExistingWorker) {
        $present=$false
        try {
            $url="$api/accounts/$AccountId/workers/scripts/$workerName"
            Invoke-WebRequest -Uri $url -Headers @{Authorization="Bearer $apiToken"} -UseBasicParsing -ErrorAction Stop | Out-Null
            $present=$true
        } catch {
            $status=0
            if ($_.Exception.Response) { $status=[int]$_.Exception.Response.StatusCode }
            if ($status -ne 404) { throw "Existing Worker check failed; cannot safely continue. Use -AdoptExistingWorker only for a known ReDesk backend." }
        }
        if ($present) { throw "Worker already exists. Use -AdoptExistingWorker to preserve its device IDs." }
    }
    Push-Location $root
    try {
        if (-not (Test-Path "node_modules/.bin/wrangler.cmd")) {
            Write-Host "Installing local Wrangler dependencies..."
            & npm install --no-audit --no-fund
            if($LASTEXITCODE -ne 0){throw "npm install failed."}
        }
        $env:CLOUDFLARE_API_TOKEN=$apiToken
        $env:CLOUDFLARE_ACCOUNT_ID=$AccountId
        Write-Host "Deploying Worker and Durable Object migration (without deleting existing data)..."
        $out= & npx --no-install wrangler deploy 2>&1
        if($LASTEXITCODE -ne 0){throw "wrangler deploy failed; inspect Cloudflare project and token permissions."}
        $match=[regex]::Match(($out | Out-String),'https://[a-zA-Z0-9.-]+\.workers\.dev')
        $url=if($WorkerUrl){$WorkerUrl}elseif($match.Success){$match.Value}elseif($state){$state.worker_url}else{""}
        $turnId=if($state){$state.turn_key_id}else{""}
        if ($Mode -eq "Install" -and -not $turnId) {
            Write-Host "Creating a dedicated Cloudflare Realtime TURN Key..."
            $turn=Cf "POST" "/accounts/$AccountId/calls/turn_keys" @{name="redesk-signal"}
            if ($turn.uid -notmatch '^[a-fA-F0-9]{32}$' -or -not $turn.key) { throw "TURN Key creation returned incomplete data." }
            $turnId=$turn.uid
            $turnSecret=$turn.key
            Test-Turn $turnId $turnSecret
            WorkerSecret "TURN_KEY_ID" $turnId
            WorkerSecret "TURN_API_TOKEN" $turnSecret
            Write-Host "TURN Secrets installed." -ForegroundColor Green
        } elseif ($Mode -eq "Update") {
            Write-Host "Keeping existing TURN Key and Worker Secrets (no rotation)."
        }
        $url=Health $url
        if ($Mode -eq "Install") {
            $h=Invoke-RestMethod -Uri "$url/health" -TimeoutSec 15
            if(-not $h.turn_configured){throw "TURN secrets are not yet visible to the deployed Worker."}
        }
        if(-not(Test-Path $stateDir)){New-Item -Path $stateDir -ItemType Directory -Force | Out-Null}
        [pscustomobject]@{
            account_id=$AccountId
            worker_name=$workerName
            worker_url=$url
            turn_key_id=$turnId
        } | ConvertTo-Json -Depth 4 | Set-Content -Path $statePath -Encoding UTF8
        Write-Host "Setup completed. Signal host: $($url -replace '^https://','')/signal; port: 443" -ForegroundColor Green
        Write-Host "State path: $statePath (contains no secrets)."
    } finally { Pop-Location }
} finally {
    $apiToken=$null
    $turnSecret=$null
    Remove-Item Env:CLOUDFLARE_API_TOKEN -ErrorAction SilentlyContinue
    Remove-Item Env:CLOUDFLARE_ACCOUNT_ID -ErrorAction SilentlyContinue
}
