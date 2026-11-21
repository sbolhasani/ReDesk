# ReDesk TURN: two-ISP relay verification

The Cloudflare TURN credential API and /health returning success establish backend configuration, not reliable media delivery.

## Evidence so far (2026-11-21)

Same-LAN forced-relay connections selected ICE local=relay/udp remote=relay/udp with Traversal using relay server. This verifies that relay works on the LAN test only. A test across separate ISPs stalled at Receiving screen.

## Controlled validation

1. Use two independent internet links and Device ID mode, not LAN direct IP.
2. Set both Host and Viewer to Force Relay (UDP), save and establish a NEW session.
3. Confirm both sides select relay candidates in their MiniRTC logs. Connected without a visible first frame is NOT sufficient.
4. On the Viewer inspect No remote screen after 12s, including decoded_video, video_rtp_packets, video_bitrate, srtp_active, selected_route and configured_turn.
5. Record logs from Host and Viewer around the same connection attempt. Look for Selected ICE path, Traversal using, DTLS, SRTP, Host information send failed, and screen capture/encoder errors.
6. Repeat with Force Relay (TCP) on both peers. This exercises plain TURN over TCP, not TURN/TLS over 443.
7. Finish with Auto (P2P / Relay). Auto mode reports Relay Connected, NOT UDP or TCP, because traversal stats do not expose relay server transport.

Windows logs: %LOCALAPPDATA%\ReDesk\logs\minirtc.log. Portable logs: redesk-logs\minirtc.log alongside the executable.

PowerShell viewer log filter:

    $log = "$env:LOCALAPPDATA\ReDesk\logs\minirtc.log"
    Get-Content $log | Select-String "Selected ICE path|Traversal using|No remote screen after|Ice ready|SRTP|Host information send failed" | Select-Object -Last 70

## Interpret evidence

- local=relay/udp remote=relay/udp with working video: verified TURN/UDP media relay for that network.
- local=host or remote=srflx without relay candidates: direct path, not TURN.
- relay selected and video_rtp_packets=0: check capture, video sender and forwarding from Host to Viewer.
- video RTP packets present but decoded_video=false: inspect SRTP unprotect, RTP packet reconstruction and decoder.
- decoded_video=true but no picture: investigate Viewer renderer/frame handoff.
- Force UDP fails but Force TCP works: UDP relay traffic may be blocked or unusable on one link.
- Both forced modes fail: inspect relay allocation, DTLS and ICE on both sides.

Remove credentials, device identifiers, public addresses and other sensitive content before sharing logs.

## Status and limitations

ReDesk now labels the actual negotiated P2P/Relay route instead of calling every MiniRTC Connected event P2P Connected. After 12 seconds of receiving no picture, the Viewer reports whether a decoded frame arrived and logs inbound video RTP diagnostics. This does NOT fix the separate-ISP media failure by itself.

The current MiniRTC integration does not support turns: TURN/TLS; plain TURN/TCP is not the same as TURN/TLS on port 443. Credential renewal and ICE restart after network changes remain future work.
