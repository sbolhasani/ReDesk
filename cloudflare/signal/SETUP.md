# ReDesk: self-hosted Cloudflare setup

The setup utility is independent of the ReDesk desktop application.
It deploys the ReDesk signaling Worker + SQLite Durable Object and provisions
Cloudflare Realtime TURN using only the owner's Cloudflare account.

## Prerequisites

- Windows PowerShell 5.1 or PowerShell 7; Node.js 20+ with npm and npx.
- A Cloudflare account with Workers and Realtime TURN available.
- A narrowly scoped Cloudflare **API Token** with Workers Scripts Edit and
  Calls Write permissions on the chosen account. Some accounts require
  additional visibility permissions to list accounts; supplying Account ID
  avoids needing Account Read. Never use your Global API Key.
- A workers.dev subdomain enabled on the account, or a custom route deployed
  through Wrangler. If Cloudflare requires initial billing or account
  activation, this must be completed through the account's own dashboard.

Do not paste any token into GitHub issues, ChatGPT, or a third-party setup
service. The script receives it privately on the machine running setup.
You may alternatively set `CLOUDFLARE_API_TOKEN` and
`CLOUDFLARE_ACCOUNT_ID` for noninteractive CI.

## Installation

From the checked-out ReDesk repository:

```powershell
cd cloudflare\signal
powershell -NoProfile -ExecutionPolicy Bypass -File .\setup-redesk-cloudflare.ps1
```

The script:

1. Validates basic dependencies and access; selects the account when unique.
2. Checks that `redesk-signal` does not already exist before first install.
3. Runs `wrangler deploy` with the existing `SignalHub` SQLite migration.
4. Creates one Realtime TURN key via `POST /accounts/{id}/calls/turn_keys`.
5. Validates that the new TURN key can issue short-lived ICE credentials.
6. Sets `TURN_KEY_ID` and `TURN_API_TOKEN` via **Worker secrets**, not
   plaintext `[vars]` or the repository.
7. Checks `/health` and prints the signal host ending in `/signal` and
   port `443`.

The provisioning API Token is used for setup only, never at runtime. The
TURN key secret is retained in Cloudflare Worker Secrets; temporary ICE
credentials are issued per authorized connection.

A nonsecret local state file is stored at
`%LOCALAPPDATA%\ReDesk\Cloudflare\<account-id>.json` with the Worker URL and
TURN key ID. This is used to avoid creating new keys on subsequent runs.

### Upgrading an existing ReDesk Worker

If `redesk-signal` is already deployed but there is no local setup state
(e.g. an early ReDesk Cloudflare beta installation), run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\setup-redesk-cloudflare.ps1 -Mode Install -AdoptExistingWorker -AccountId YOUR_ACCOUNT_ID
```

**Only adopt your existing ReDesk Worker**. Keeping its original name and
SQLite Durable Object binding preserves current device IDs and credentials.
Do not delete the Worker, Durable Object or client identity caches.

On normal installations, rerun the script with no flags to update the code
while keeping existing TURN keys and Worker Secrets. Explicit commands:

```powershell
.\setup-redesk-cloudflare.ps1 -Mode Update -AccountId YOUR_ACCOUNT_ID
.\setup-redesk-cloudflare.ps1 -Mode Check -AccountId YOUR_ACCOUNT_ID
```

The setup file can be rerun, but an interrupted first installation may leave
an orphaned TURN key, because Cloudflare only returns its secret on creation.
Recover deliberately from Cloudflare Dashboard instead of repeatedly creating
keys; the script does not silently replace an existing Worker.

## Runtime and testing

ReDesk's default TURN mode is Auto UDP/TCP. Its existing MiniRTC/libnice ICE
engine chooses a working candidate pair; direct P2P can still win over TURN.
To prove actual relay usage, set **Force UDP** on both devices and connect.
Inspect the selected ICE candidate pair or network telemetry; a connected
screen in Auto mode **does not prove** TURN was used. For TCP relay, try
**Force TCP** on both devices as well.

Newer Cloudflare credentials return `turns:` TLS endpoints. The pinned
MiniRTC implementation currently does not support `turns:`, so the Worker
forwards only compatible `turn:` UDP/TCP endpoints. TURN/TLS on port 443 is
**not** implemented yet. TURN credentials currently last 6 hours and
in-session credential refresh is not yet implemented; long sessions and
network changes require future ICE renewal handling.

If TURN credentials cannot be fetched, the Worker logs only an error category
and sends the previous STUN-only ICE snapshot. This preserves existing P2P
behavior, but **Force TURN** must fail rather than silently fall back to
direct transport.

`/health` reports `turn_configured: true` only when both Worker secrets
are present. It does **not** prove a real TURN allocation, relay media path,
network compatibility, or that the Cloudflare Realtime plan is active.

## Cloudflare APIs

- TURN key creation:
  https://developers.cloudflare.com/api/resources/calls/subresources/turn/methods/create/
- Short-lived ICE credentials:
  https://developers.cloudflare.com/realtime/turn/generate-credentials/
- Wrangler Worker secrets:
  https://developers.cloudflare.com/workers/wrangler/commands/

## Important limitations

This is a self-hosted development release, not yet an audited production
remote-access service. Worker account security and password-hash migration,
rate limits, session recovery, and ICE credential rotation need separate
hardening and tests. Existing durable device data must not be reset to fix
a TURN provisioning failure.

## Troubleshooting: Wrangler deployment permissions

ReDesk's provisioning token and Wrangler's local OAuth authentication are
separate. When `CLOUDFLARE_API_TOKEN` is exported, Wrangler **prefers that
token** over a previous `wrangler login`. A token sufficient for TURN
`Calls Write` might not have Workers permissions, and deployment can fail at
`/accounts/<id>/workers/services/redesk-signal`.

The setup script now checks for a matching Wrangler OAuth account (by Account
ID), prefers it for **deployment and Worker secrets**, and uses the entered
scoped token for **Realtime TURN Key creation**. It preserves the caller's
environment variables and displays Cloudflare error codes without printing
credentials. It will never silently use an OAuth login from a different account.

When you have already deployed via Wrangler, run:

```powershell
cd cloudflare\signal
npx wrangler login
npx wrangler whoami
powershell -NoProfile -ExecutionPolicy Bypass -File .\setup-redesk-cloudflare.ps1 -Mode Install -AdoptExistingWorker -DeployAuth Wrangler -AccountId YOUR_ACCOUNT_ID
```

You may also pass `-DeployAuth Token` to require the supplied API token for
deployment. In that case, grant the token the necessary Workers deployment
permissions in addition to `Calls Write`; Wrangler may need Account Settings
Read, depending on your account and configuration. Cloudflare account billing
and initial Workers/Realtime activation remain outside the setup script.

**Never delete an existing Worker or its Durable Object to fix a permission
error.** Your device IDs and authenticated records are stored in its SQLite DO.

## Interactive setup and Cloudflare token templates

Run the script **without flags** to see a console menu. The menu offers:
- Deploy / update Worker and TURN
- Configure TURN on an **already deployed Worker without redeploying** code
- Open Cloudflare's account-token creation page for TURN-only or full setup
- Check the health of an existing Worker

The browser links use Cloudflare's official *API Token Template URLs*
(`permissionGroupKeys`) populated with account-scoped permission group IDs:
`Calls Write` (`b711942448db4b0aace44d1312f9fdb0`),
`Workers Scripts Write` (`e086da7e2179491d91ee5f35b3ca210a`),
and `Account Settings Read` (`c1fde68c7bcc44588cbb6ddbc16d6480`).
These IDs were cross-checked against a captured Cloudflare account permission
group listing; the Cloudflare dashboard remains the final authority. Review
the selected permissions before creating the token. If the dashboard warns
that a permission cannot be resolved, **do not create the incomplete token**.
No token or password is passed in the browser URL.

When upgrading a known ReDesk account that already runs the signaling Worker,
select **Configure TURN on existing Worker**. It checks `/health`, skips
`wrangler deploy`, creates/validates a TURN Key and puts secrets into the
existing Worker. The nonsecret local state tracks the key ID for future updates.

Cloudflare may reject requests for reasons other than missing permissions
(e.g. product availability or account policy). The setup now extracts the
HTTP status and up to three sanitized Cloudflare error codes/messages.
Avoid pasting raw bearer tokens, full API responses, or local Wrangler
credential files into support conversations.

Documentation: https://developers.cloudflare.com/fundamentals/api/how-to/account-owned-token-template/
