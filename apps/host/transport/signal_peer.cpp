#include "signal_peer.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace redesk::host {
namespace {
bool valid_id(std::string_view id) {
    return id.size() <= 128 && std::all_of(id.begin(), id.end(),
        [](unsigned char c) { return c >= '0' && c <= '9'; });
}
bool valid_password(std::string_view password) {
    return password.size() <= 128 && std::all_of(password.begin(), password.end(),
        [](unsigned char c) { return c >= 33 && c <= 126 && c != '@'; });
}
}
SignalPeer::SignalPeer(std::string endpoint, std::string device_id, std::string password, DeviceKey key)
    : key_(std::move(key)) {
    // Require explicit secure endpoints and reject embedded NUL/control bytes.
    if (endpoint.rfind("wss://", 0) != 0 || endpoint.size() >= sizeof(params_.signal_server_ip) ||
        std::any_of(endpoint.begin(), endpoint.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
        throw std::invalid_argument("Expected a secure WSS signaling endpoint");
    MiniRtcSignalServerAddress parsed{};
    if (MiniRtcParseSignalServerAddress(endpoint.data(), endpoint.size(), &parsed) != 0)
        throw std::invalid_argument("Invalid signaling endpoint");
    if (!valid_id(device_id) || !valid_password(password) || (device_id.empty() && !password.empty()))
        throw std::invalid_argument("Invalid login identity");
    login_id_ = std::move(device_id);
    if (!password.empty()) login_id_ += "@" + password;
    if (!password.empty()) OPENSSL_cleanse(password.data(), password.size());

    std::memcpy(params_.signal_server_ip, endpoint.data(), endpoint.size());
    params_.signal_server_port = parsed.port ? parsed.port : 443;
    params_.user_id = login_id_.c_str();
    params_.user_data = this;
    params_.hardware_acceleration = true;
    params_.enable_srtp = true;
    params_.turn_mode = MiniRtcTurnAutoUdpTcp;
    params_.video_content_type = MiniRtcVideoContentType::ScreenContent;
    params_.video_quality = MiniRtcVideoQualityHigh;
    params_.video_frame_rate = 30;
    params_.video_degradation_preference = MiniRtcVideoDegradationPreference::MaintainResolution;
    params_.on_signal_exchange = exchange;
    params_.on_signal_status = signal_status;
    params_.on_connection_status = connection_status;
    params_.on_net_status_report = net_status;
}
SignalPeer::~SignalPeer() {
    stop();
    if (!login_id_.empty()) OPENSSL_cleanse(login_id_.data(), login_id_.size());
}
void SignalPeer::start() {
    if (started_) throw std::logic_error("Signaling peer is single-use");
    started_ = true;
    peer_ = MiniRtcCreatePeer(&params_);
    if (!peer_) throw std::runtime_error("Cannot allocate signaling peer");
    if (MiniRtcInit(peer_) != 0) {
        stop();
        throw std::runtime_error("Cannot initialize signaling peer");
    }
}
void SignalPeer::stop() noexcept {
    if (peer_) MiniRtcDestroyPeer(&peer_);
}
SignalPeer::Status SignalPeer::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}
int SignalPeer::exchange(MiniRtcSignalExchangeStage stage, const char* data, size_t size,
                         char* response, size_t capacity, void* context) noexcept {
    try {
        if (!context || !data || !response || size == 0 || size > 65536) return -1;
        auto& self = *static_cast<SignalPeer*>(context);
        if (stage == MiniRtcSignalExchangeStage::IncomingMessage && !self.login_.Pending()) {
            // This diagnostic must not accept incoming sessions. Return a
            // protocol failure for a join notification rather than opening ICE.
            const auto msg = nlohmann::json::parse(data, data + size, nullptr, false);
            if (!msg.is_object()) return -1;
            const auto type = msg.value("type", std::string{});
            if (type == "user_join_transmission" || type == "offer") return -1;
            return 0;
        }
        return self.login_.ExchangeWithSigner(stage, nlohmann::json::parse(data, data + size),
            self.key_.PublicKey(), [&](std::string_view text) { return self.key_.Sign(text); },
            response, capacity);
    } catch (...) { return -1; }
}
void SignalPeer::signal_status(MiniRtcSignalStatus state, const char* id, size_t size,
                                void* context) noexcept {
    try {
        if (!context) return;
        auto& self = *static_cast<SignalPeer*>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.status_.state = state;
        if (state == MiniRtcSignalConnected) {
            if (!id || !size || size > 128 || !valid_id({id,size})) {
                self.status_.state = MiniRtcSignalFailed;
                self.status_.device_id.clear();
            } else {
                self.status_.device_id.assign(id,size);
            }
        }
    } catch (...) {}
}
void SignalPeer::connection_status(MiniRtcConnectionStatus state, const char*, size_t,
                                    void* context) noexcept {
    // Never authorize application data or publish a session from this probe.
    if (state == MiniRtcConnectionConnected)
        signal_status(MiniRtcSignalFailed, nullptr, 0, context);
}
void SignalPeer::net_status(const char*, size_t, MiniRtcTraversalMode, const MiniRtcNetTrafficStats*,
                             const char*, size_t, void*) noexcept {
    // The initial report may carry ID@password. Do not log or expose it.
}
}
