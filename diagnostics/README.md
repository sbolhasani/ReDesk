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

Switch to the diagnostic branch only after preserving any uncommitted **ReDesk parent-repository** changes that Git could otherwise overwrite. Existing MiniRTC submodule edits do **not** have to be discarded or stashed: the apply script preserves them when the patch can be applied cleanly.

```powershell
cd C:\Users\Saeid\source\sbolhasani\CrossDesk-Upstream
git fetch origin
git switch work/redesk-turn-tcp-diagnostics-01
git pull --ff-only
git submodule update --init --recursive
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\apply-minirtc-diagnostics.ps1
xmake -y
```

`apply` checks the pinned MiniRTC commit and calls `git apply --check` against the **current working-tree contents**, even if MiniRTC already has tracked edits. If every hunk can apply cleanly, other edits remain intact. If there is a conflict, the script stops **without** resetting or stashing anything. It rejects a second application of the same patch. The script does not create a Git commit or modify the recorded submodule revision. You can inspect existing edits safely with `git -C deps/submodules/minirtc status --short` and `git -C deps/submodules/minirtc diff --stat`.

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


## Phase 2: isolate ICE failures without changing the transport defaults

After applying Phase 1 (already applied on the original test machines), pull the updated
diagnostics branch and apply the **incremental** Phase 2 patch:

```powershell
git pull --ff-only
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\apply-minirtc-diagnostics-v2.ps1
xmake -y
```

The Phase 2 patch edits only MiniRTC's `src/ice/ice_agent.cpp` and
`src/ice/ice_server_resolver.h`, leaving Phase 1 intact. It adds:
- Redacted ICE gathering totals split by relayed-candidate IPv4 vs IPv6;
- Selected-pair candidate types, address families, and candidate ports (not IPs);
- **Opt-in** family and TURN server port filters for `Force Relay TCP` only;
- An **optional, sampled** GLib/libnice message classifier for TURN allocation
  and ICE connectivity-check activity (no raw libnice messages, IPs or tokens).

The filters are off by default and never affect Force UDP or other TURN modes.
They intentionally filter **TURN server endpoint addresses**, not Windows
network interface selection or the relay candidates' allocated address family.

### Set a test configuration (important: dot-source)

From a PowerShell terminal, **dot-source** the helper (note the initial dot
followed by a space). Do **not** run it via a child `powershell.exe -File`,
because then the environment changes disappear when that process exits:

```powershell
# First establish the default behavior (no endpoint restriction).
. .\scripts\set-minirtc-turn-test.ps1 -Family any -Port any

# Restrict just Force Relay TCP to IPv4 TURN servers on port 3478.
. .\scripts\set-minirtc-turn-test.ps1 -Family ipv4 -Port 3478 -Libnice

# Then launch the newly-built ReDesk EXE from the SAME PowerShell terminal.
# Use your actual build output path, or run via the existing Xmake run target.
```

All switches are process-local; they do not modify the registry or permanent
environment. A running GUI instance or Windows service cannot inherit new
variables. Close those processes before retesting. Never test the original
running Windows service and assume it received an environment setting.

Repeat on Host and Viewer with **the same** family/port settings and independent
Internet links. Suggested progression:

| Run | Family | Port | Purpose |
| --- | --- | --- | --- |
| A | any | any | Reproduce original Cloudflare failure with Phase 2 logging |
| B | ipv4 | 3478 | Isolate IPv4 TURN connection |
| C | ipv4 | 80 | Compare other TCP port on IPv4 |
| D | ipv6 | 3478 | Isolate IPv6 TURN connection |
| E | ipv6 | 80 | Compare other TCP port on IPv6 |

Use `-Libnice` on one of the failing cases when you need deeper internal
evidence. The libnice classifier depends on the upstream GLib log domains
and `NICE_DEBUG` and may not see every allocation or check. It is **not**
a definitive packet-level trace. For reliable allocation/permission/packet
details, collect an independent transport trace or implement dedicated
instrumentation in libnice as a separately reviewed diagnostic step.

### Remove Phase 2 before Phase 1

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\remove-minirtc-diagnostics-v2.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\remove-minirtc-diagnostics.ps1
xmake -y
```

Both scripts use preflight checks and never reset unrelated edits such as
`thirdparty/dav1d/xmake.lua`. In particular, **do not** try to remove
Phase 1 while Phase 2 is still applied.
