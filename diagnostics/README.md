# Temporary TURN/TCP diagnostics for ReDesk

Branch: `work/redesk-turn-tcp-diagnostics-01` (based on `work/redesk-cloudflare-signal-01`).

## Scope

This is a **temporary, compile-time diagnostic patch**, not a MiniRTC fork or permanent transport change. ReDesk keeps its upstream MiniRTC submodule URL and gitlink at `9b1acc206b84ad8f31b9466a213ea6777ca8aa18`.

The patch changes only MiniRTC source files **in the local submodule working tree**:

- `src/ice/ice_agent.cpp`: reports registered TURN transport and IPv4/IPv6 family/port, endpoint counts, and DTLS packet/byte counters on success or failure.
- `src/ice/ice_agent.h`: temporary atomic DTLS send/receive counters.
- `src/ice/ice_server_resolver.h`: reports how many IPv4 and IPv6 addresses DNS returned for each TURN hostname.

It **does not** log TURN passwords, usernames, auth tokens, SDP, media contents, or endpoint IPs. The new log lines are marked `TURN_DIAG`. Counts reflect MiniRTC's DTLS BIO send results and received DTLS-looking records—not TURN server allocation acknowledgements, on-wire packet delivery, or socket-level TCP transport verification. Those require libnice logs/network capture if ambiguity remains. The patch does not modify TIMEOUTs, ICE behavior, transport settings, Cloudflare Worker, or libnice.

## Install on Windows

Use a clean checkout of the named base branch or switch directly to this diagnostic branch **only if your local work is committed or stashed first**:

```powershell
cd C:\Users\Saeid\source\sbolhasani\CrossDesk-Upstream
git fetch origin
git switch work/redesk-turn-tcp-diagnostics-01
git pull --ff-only
git submodule update --init --recursive
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\apply-minirtc-diagnostics.ps1
xmake -y
```

`apply` rejects non-clean tracked MiniRTC files and a mismatched pinned commit; it never deletes, checks out, or resets a user file. It is not intended to run twice without removal.

To undo the patch without wiping unrelated MiniRTC edits:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\remove-minirtc-diagnostics.ps1
xmake -y
```

The reverse script performs `git apply --reverse --check` before changing anything. Do **not** use `git submodule update --force` or `git reset --hard` to remove this patch.

## Comparison test: two separate ISPs

1. Run **the same diagnostic ReDesk executable build** on Host and Viewer. Use device-ID/Internet mode, not direct LAN IP.
2. For baseline, select **Force Relay (TCP)** on both devices, using public CrossDesk servers; verify screen **and** input. Collect both MiniRTC logs.
3. Select **Force Relay (TCP)** on both devices using your custom Cloudflare signaling Worker. New sessions only. Collect both logs until a success or explicit failure.
4. Keep Force Relay (UDP) as the known-working fallback; no code change to UDP is included.
5. Compare `TURN_DIAG` lines (endpoint count, IPv4/IPv6, port, TX/RX DTLS packets), `Selected ICE path`, `ICE state change`, `DTLS handshake done`, and the Viewer no-video diagnostic.

Log location (installed): `%LOCALAPPDATA%\ReDesk\logs\minirtc.log`. Portable: `redesk-logs\minirtc.log` alongside the executable.

Example filter:

```powershell
Get-Content "$env:LOCALAPPDATA\ReDesk\logs\minirtc.log" |
  Select-String 'TURN_DIAG|ICE gathering done|Selected ICE path|ICE state change|DTLS handshake|No remote screen after'
```

**Interpreting results**:

- `tx_packets > 0, rx_packets = 0` on both peers: DTLS is emitted into libnice locally, but no DTLS packets were observed inbound. Investigate TURN allocation, permissions, forwarding, or routing. This does *not* establish that the datagrams reached the TURN server.
- `rx_packets > 0` with handshake failure: inspect DTLS progress, retransmissions, and ICE pair stability.
- CrossDesk succeeds but Cloudflare fails while using identical binaries: focus on different relay endpoint families/ports and Cloudflare TURN behavior before modifying MiniRTC's transport logic.
- Successful DTLS without a visible frame: inspect SRTP/RTP and Viewer decoding, not merely ICE Ready.

Do not publish raw logs without checking for device identifiers, public IPs, or credentials logged by **other** non-`TURN_DIAG` components.
