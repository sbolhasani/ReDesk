#pragma once

#include "device_key.h"
#include "runtime/device_login.h"
#include <minirtc.h>
#include <mutex>
#include <string>

namespace redesk::host {

// Owns a single signaling peer. The owner thread is the only thread allowed
// to start/stop or call MiniRTC APIs. Callbacks only publish bounded snapshots.
// Deliberately no media streams or incoming data callbacks in this diagnostic.
class SignalPeer {
public:
    struct Status {
        MiniRtcSignalStatus state = MiniRtcSignalConnecting;
        std::string device_id;
    };
    SignalPeer(std::string endpoint, std::string device_id, std::string password, DeviceKey key);
    ~SignalPeer();
    SignalPeer(const SignalPeer&) = delete;
    SignalPeer& operator=(const SignalPeer&) = delete;
    void start();
    void stop() noexcept;
    Status status() const;

private:
    static int exchange(MiniRtcSignalExchangeStage, const char*, size_t, char*, size_t, void*) noexcept;
    static void signal_status(MiniRtcSignalStatus, const char*, size_t, void*) noexcept;
    static void connection_status(MiniRtcConnectionStatus, const char*, size_t, void*) noexcept;
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
};
}
