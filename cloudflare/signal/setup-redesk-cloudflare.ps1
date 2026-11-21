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
    [ValidateSet("Auto","Install","Update","Turn","Check","CreateToken")] [string]$Mode="Auto",
    [string]$AccountId="",
    [switch]$AdoptExistingWorker,
    [ValidateSet("Auto","Wrangler","Token")] [string]$DeployAuth="Auto",
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
# Save inherited environment variables and restore them when setup exits.
$oldCloudToken=$env:CLOUDFLARE_API_TOKEN
$oldCloudAccount=$env:CLOUDFLARE_ACCOUNT_ID
$oldCloudKey=$env:CLOUDFLARE_API_KEY
$oldCloudEmail=$env:CLOUDFLARE_EMAIL


# Cloudflare API Token template URL format is documented by Cloudflare:
# https://developers.cloudflare.com/fundamentals/api/how-to/account-owned-token-template/
# Account-owned tokens accept exact permission-group IDs, unlike user tokens.
# The documented group IDs can be resolved in Cloudflare's permission-groups API.
function Get-TokenTemplateUrl([string]$Purpose) {
    # Calls Write, Workers Scripts Write, Account Settings Read.
    # IDs are Cloudflare permission group IDs; account-token URLs accept IDs.
    $callsWrite="b711942448db4b0aace44d1312f9fdb0"
    $workersWrite="e086da7e2179491d91ee5f35b3ca210a"
    $accountRead="c1fde68c7bcc44588cbb6ddbc16d6480"
    $perms=@(@{key=$callsWrite;type="edit"})
    $name="ReDesk TURN Setup"
    if ($Purpose -eq "Full") {
        $perms+=@{key=$workersWrite;type="edit"}
        $perms+=@{key=$accountRead;type="read"}
        $name="ReDesk Backend Setup"
    }
    $json=ConvertTo-Json -InputObject @($perms) -Depth 4 -Compress
    $encoded=[Uri]::EscapeDataString($json)
    return "https://dash.cloudflare.com/?to=/:account/api-tokens&permissionGroupKeys=$encoded&name=$([Uri]::EscapeDataString($name))"
}
function Open-TokenTemplate([string]$Purpose) {
    $url=Get-TokenTemplateUrl $Purpose
    Write-Host ""
    Write-Host "Opening Cloudflare's official account API-token creation form..."
    if ($Purpose -eq "Turn") {
        Write-Host "Required permission: Account / Calls / Write."
    } else {
        Write-Host "Required permissions: Account / Calls / Write, Workers Scripts / Write, Account Settings / Read."
    }
    Write-Host "Review the preselected permissions and account in Cloudflare before creating."
    Write-Host "Cloudflare must still show the token once; copy it privately into this setup tool."
    Write-Host $url
    try { Start-Process -FilePath $url -ErrorAction Stop | Out-Null }
    catch { Write-Host "Could not open browser. Copy the printed URL instead." }
}
function Show-SetupMenu {
    while ($true) {
        Write-Host ""
        Write-Host "========== ReDesk Cloudflare Setup ==========" -ForegroundColor Cyan
        Write-Host "1. Install / update Worker and TURN"
        Write-Host "2. Configure TURN on an already deployed ReDesk Worker (no deploy)"
        Write-Host "3. Create Cloudflare API Token - TURN only (opens browser)"
        Write-Host "4. Create Cloudflare API Token - full setup (opens browser)"
        Write-Host "5. Check deployed backend"
        Write-Host "0. Exit"
        $selection=Read-Host "Select"
        switch ($selection.Trim()) {
            "1" { return "Install" }
            "2" { return "Turn" }
            "3" { Open-TokenTemplate "Turn"; continue }
            "4" { Open-TokenTemplate "Full"; continue }
            "5" { return "Check" }
            "0" { return "Exit" }
            default { Write-Host "Invalid selection." }
        }
    }
}

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

function Run-Wrangler([string[]]$CommandArgs, [string]$StdIn="") {
    # PS5.1 turns native stderr into an ErrorRecord under 2>&1.
    # Do not treat that as a PowerShell terminating error; inspect exit code.
    $oldPreference=$ErrorActionPreference
    try {
        $ErrorActionPreference="Continue"
        if ($StdIn) { $lines=@($StdIn | & npx --no-install wrangler @CommandArgs 2>&1) }
        else { $lines=@(& npx --no-install wrangler @CommandArgs 2>&1) }
        $code=$LASTEXITCODE
    } finally { $ErrorActionPreference=$oldPreference }
    return [pscustomobject]@{
        Code=[int]$code
        Lines=@($lines | ForEach-Object { [string]$_ })
    }
}
function Explain-WranglerError($result) {
    # Display provider error codes and permission details without credentials.
    $msg=($result.Lines | Select-Object -Last 25) -join [Environment]::NewLine
    foreach ($secret in @($apiToken, $turnSecret)) {
        if (-not [string]::IsNullOrEmpty($secret)) { $msg=$msg.Replace($secret,"[redacted]") }
    }
    $msg=[regex]::Replace($msg,'(?i)Bearer\s+[A-Za-z0-9._~-]+','Bearer [redacted]')
    if ($msg) { Write-Host $msg }
}
function Resolve-DeployAuth([string]$Choice,[string]$Account) {
    if ($Choice -eq "Token") { return "Token" }
    # Probe the local OAuth login, ignoring a separate Calls TURN API token.
    Remove-Item Env:CLOUDFLARE_API_TOKEN -ErrorAction SilentlyContinue
    Remove-Item Env:CLOUDFLARE_API_KEY -ErrorAction SilentlyContinue
    Remove-Item Env:CLOUDFLARE_EMAIL -ErrorAction SilentlyContinue
    try { $probe=Run-Wrangler @("whoami") }
    finally {
        if ($null -ne $oldCloudToken) { $env:CLOUDFLARE_API_TOKEN=$oldCloudToken }
        if ($null -ne $oldCloudKey) { $env:CLOUDFLARE_API_KEY=$oldCloudKey }
        if ($null -ne $oldCloudEmail) { $env:CLOUDFLARE_EMAIL=$oldCloudEmail }
    }
    $output=$probe.Lines -join " "
    if ($probe.Code -eq 0 -and $output -match "OAuth Token" -and
        $output.Contains($Account)) { return "Wrangler" }
    if ($Choice -eq "Wrangler") {
        throw "No Wrangler OAuth login matching this Account ID. Run 'npx wrangler login' and check 'npx wrangler whoami'."
    }
    return "Token"
}

function WorkerSecret([string]$name,[string]$value) {
    if (-not $value) { throw "Refusing to put an empty secret." }
    # Passing via stdin avoids placing credentials in command line args.
    $result=Run-Wrangler @("secret","put",$name) $value
    if ($result.Code -ne 0) {
        Explain-WranglerError $result
        throw "Wrangler could not configure $name (exit $($result.Code))."
    }
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
    if ($Mode -eq "Auto") { $Mode=Show-SetupMenu }
    if ($Mode -eq "Exit") { return }
    if ($Mode -eq "CreateToken") { Open-TokenTemplate "Full"; return }
    if (-not (Test-Path (Join-Path $root "wrangler.toml"))) { throw "Run the bundled cloudflare/signal setup script." }
    foreach($cmd in @("node","npm","npx")) {
        if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { throw "Node.js 20+ is required." }
    }
    if ([int](& node -p "process.versions.node.split('.')[0]") -lt 20) { throw "Node.js 20+ required." }
    if ($Mode -in @("Install","Turn") -or $DeployAuth -eq "Token") {
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
    # Menu selection is explicit. Install is safely repeatable and retains state.
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
        $auth=Resolve-DeployAuth $DeployAuth $AccountId
        $env:CLOUDFLARE_ACCOUNT_ID=$AccountId
        if($auth -eq "Wrangler") {
            Remove-Item Env:CLOUDFLARE_API_TOKEN -ErrorAction SilentlyContinue
            Remove-Item Env:CLOUDFLARE_API_KEY -ErrorAction SilentlyContinue
            Remove-Item Env:CLOUDFLARE_EMAIL -ErrorAction SilentlyContinue
            Write-Host "Deploy authentication: existing Wrangler OAuth login."
        } else {
            $env:CLOUDFLARE_API_TOKEN=$apiToken
            Write-Host "Deploy authentication: scoped API Token."
        }
        Write-Host "Deploying Worker and Durable Object migration (without deleting existing data)..."
        $deployment=Run-Wrangler @("deploy")
        if ($deployment.Code -ne 0) {
            Explain-WranglerError $deployment
            throw "wrangler deploy failed. The token may lack Workers Scripts Edit or Account Settings Read. Try -DeployAuth Wrangler if you have already run wrangler login."
        }
        $match=[regex]::Match(($deployment.Lines -join [Environment]::NewLine),'https://[a-zA-Z0-9.-]+\.workers\.dev')
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
    if ($null -ne $oldCloudToken) { $env:CLOUDFLARE_API_TOKEN=$oldCloudToken }
    else { Remove-Item Env:CLOUDFLARE_API_TOKEN -ErrorAction SilentlyContinue }
    if ($null -ne $oldCloudAccount) { $env:CLOUDFLARE_ACCOUNT_ID=$oldCloudAccount }
    else { Remove-Item Env:CLOUDFLARE_ACCOUNT_ID -ErrorAction SilentlyContinue }
    if ($null -ne $oldCloudKey) { $env:CLOUDFLARE_API_KEY=$oldCloudKey }
    else { Remove-Item Env:CLOUDFLARE_API_KEY -ErrorAction SilentlyContinue }
    if ($null -ne $oldCloudEmail) { $env:CLOUDFLARE_EMAIL=$oldCloudEmail }
    else { Remove-Item Env:CLOUDFLARE_EMAIL -ErrorAction SilentlyContinue }
}
