#pragma once

#include "device_key.h"
#include "runtime/device_login.h"
#include <minirtc.h>
#include <mutex>
#include <deque>
#include <optional>
#include <vector>
#include <string>

namespace redesk::host {

// Owns a single signaling peer. The owner thread is the only thread allowed
// to start/stop or call MiniRTC APIs. Callbacks only publish bounded snapshots.
// Signaling-only by default. The opt-in, single-viewer diagnostic can send
// primary-display video and optional input only after local confirmation.
class SignalPeer {
public:
    struct Status {
        MiniRtcSignalStatus state = MiniRtcSignalConnecting;
        std::string device_id;
        bool viewer_connected = false;
        bool revoked = false;
    };
    SignalPeer(std::string endpoint, std::string device_id, std::string password, DeviceKey key);
    ~SignalPeer();
    // Before start(), restrict a temporary registration to a specific desktop
    // viewer identity. Only one join is accepted in this peer's lifetime.
    void enable_view_probe(std::string viewer_id, bool allow_input = false, bool allow_clipboard = false, bool allow_audio = false);
    struct InputMessage { std::string stream, payload; };
    std::vector<InputMessage> take_input();
    std::optional<std::string> take_clipboard();
    bool send_clipboard(const std::string& text);
    bool audio_requested() const;
    bool send_audio(const char* data, std::size_t size);
    std::string take_registration_password();
    bool approve_viewer();
    bool send_host_info(const std::string& message);
    bool send_frame(const MiniRtcVideoFrame& frame);
    bool view_active() const;
    SignalPeer(const SignalPeer&) = delete;
    SignalPeer& operator=(const SignalPeer&) = delete;
    void start();
    void stop() noexcept;
    Status status() const;

private:
    static int exchange(MiniRtcSignalExchangeStage, const char*, size_t, char*, size_t, void*) noexcept;
    static void signal_status(MiniRtcSignalStatus, const char*, size_t, void*) noexcept;
    static void connection_status(MiniRtcConnectionStatus, const char*, size_t, void*) noexcept;
    static void receive_input(const char*, size_t, const char*, size_t, const char*, size_t, void*) noexcept;
    static void net_status(const char*, size_t, MiniRtcTraversalMode, const MiniRtcNetTrafficStats*,
                           const char*, size_t, void*) noexcept;
    std::string login_id_;
    DeviceKey key_;
    MiniRtcParams params_{};
    MiniRtcPeerPtr* peer_ = nullptr;
    bool started_ = false;
    crossdesk::DeviceLogin login_;
    mutable std::mutex mutex_;
    Status status_;
    std::string expected_viewer_;
    std::string registration_password_;
    bool join_seen_ = false;
    bool approved_ = false;
    bool allow_input_ = false;
    std::deque<InputMessage> input_;
    bool allow_clipboard_ = false;
    bool allow_audio_ = false;
    bool audio_requested_ = false;
    std::optional<std::string> clipboard_;
};
}
