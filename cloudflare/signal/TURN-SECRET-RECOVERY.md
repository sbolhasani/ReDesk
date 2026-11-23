# One-time TURN secret recovery (diagnostics only)

The deployed ReDesk Worker already knows the existing `TURN_KEY_ID` and
`TURN_API_TOKEN`; no extra TURN key or key rotation is required.

**WARNING:** The temporary endpoint returns the *long-term* TURN API token.
Only use this on a trusted computer and remove the endpoint immediately after
the test. Never upload or paste the result into GitHub, chat or log files.
A leaked TURN API token must be rotated.

Run these commands in **Windows PowerShell** on the trusted development PC,
from `cloudflare/signal`. Ensure the current branch is
`work/redesk-turn-tcp-diagnostics-01`. Use the existing Wrangler login/account
that owns `redesk-signal`. Do not delete or recreate the Worker or Durable Object.

```powershell
cd cloudflare\signal
npm install
npm run check
npm run deploy

# Create a short-lived random bearer grant. It expires in 5 minutes.
$expires = [DateTimeOffset]::UtcNow.AddMinutes(5).ToUnixTimeSeconds()
$bytes = New-Object byte[] 32
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$hex = [BitConverter]::ToString($bytes).Replace("-", "").ToLowerInvariant()
$grant = "${expires}:${hex}"
# Install a temporary Worker Secret; don't print the grant.
$grant | npx --no-install wrangler secret put TURN_SECRET_EXPORT_GRANT --name redesk-signal

# Read the token only into memory (replace with your actual URL if customized).
$headers = @{ Authorization = "Bearer $grant" }
$turn = Invoke-RestMethod -Method Post -Uri "https://redesk-signal.redesk.workers.dev/__redesk_diag/turn-secret-export" -Headers $headers -TimeoutSec 20
if ($turn.turn_key_id -notmatch '^[0-9a-f]{32}$' -or
    $turn.turn_api_token.Length -ne 64) { throw "Unexpected TURN secret response" }

# Use the token locally to create short-lived probe credentials, or print to
# your own console only if absolutely necessary:
Write-Host "TURN Key ID: $($turn.turn_key_id)"
$turn.turn_api_token | Set-Clipboard
Write-Host "TURN API token copied to clipboard; it was not printed."
```

**Immediately remove the access grant** (this is an interactive Wrangler
command; confirm deletion when prompted):

```powershell
npx --no-install wrangler secret delete TURN_SECRET_EXPORT_GRANT --name redesk-signal
Remove-Variable turn,grant,headers -ErrorAction SilentlyContinue
```

To remove the temporary endpoint entirely, revert only its block in
`cloudflare/signal/src/worker.js` (identified by
`TEMPORARY ADMIN DIAGNOSTIC`) and delete
`cloudflare/signal/test/turn-secret-export.test.js`, then redeploy.
Keep the existing Worker, TURN key and Durable Object. Test `/health` and
`/signal` after cleanup. The route replies 404 without a valid grant, but
removing its code is still required.
