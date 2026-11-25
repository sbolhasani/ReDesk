#include "signal_peer.h"
#include <stream_names.h>
#include <display_stream_id.h>
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
    if (!registration_password_.empty()) OPENSSL_cleanse(registration_password_.data(), registration_password_.size());
}
void SignalPeer::start() {
    if (started_) throw std::logic_error("Signaling peer is single-use");
    started_ = true;
    peer_ = MiniRtcCreatePeer(&params_);
    if (!peer_) throw std::runtime_error("Cannot allocate signaling peer");
    if (!expected_viewer_.empty()) {
        // Keep wire-compatible negotiation; only opt-in input is received.
        // No audio, file or clipboard operations are implemented.
        if (MiniRtcAddVideoStream(peer_, "Display1") != 0 ||
            MiniRtcAddAudioStream(peer_, crossdesk::kAudioStream) != 0 ||
            MiniRtcAddDataStream(peer_, crossdesk::kDataStream, false) != 0 ||
            MiniRtcAddDataStream(peer_, crossdesk::kControlStream, true) != 0 ||
            MiniRtcAddDataStream(peer_, crossdesk::kMouseStream, true) != 0 ||
            MiniRtcAddDataStream(peer_, crossdesk::kKeyboardStream, true) != 0 ||
            MiniRtcAddDataStream(peer_, crossdesk::kFileStream, true) != 0 ||
            MiniRtcAddDataStream(peer_, crossdesk::kFileFeedbackStream, true) != 0 ||
            MiniRtcAddDataStream(peer_, crossdesk::kClipboardStream, true) != 0) {
            stop();
            throw std::runtime_error("Cannot register diagnostic streams");
        }
    }
    if (MiniRtcInit(peer_) != 0) {
        stop();
        throw std::runtime_error("Cannot initialize signaling peer");
    }
}
void SignalPeer::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        approved_ = false;
        status_.revoked = true;
        status_.viewer_connected = false;
        input_.clear();
        clipboard_.reset();
    }
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
        const auto msg = nlohmann::json::parse(data, data + size, nullptr, false);
        if (!msg.is_object()) return -1;
        if (stage == MiniRtcSignalExchangeStage::IncomingMessage) {
            const auto type = msg.value("type", std::string{});
            if (type == "change_password") {
                std::lock_guard<std::mutex> lock(self.mutex_);
                if (self.password_result_ == PasswordResult::Pending &&
                    msg.value("request_id", std::string{}) == "redesk-host-probe-password" &&
                    msg.value("user_id", std::string{}) == self.status_.device_id)
                    self.password_result_ = msg.value("status", std::string{}) == "success"
                        ? PasswordResult::Accepted : PasswordResult::Rejected;
                return 0;
            }
            if (type == "offer" || type == "user_join_transmission") {
                std::lock_guard<std::mutex> lock(self.mutex_);
                if (type != "user_join_transmission" || self.expected_viewer_.empty() ||
                    self.login_.Pending() || self.status_.state != MiniRtcSignalConnected ||
                    self.status_.revoked || self.join_seen_ ||
                    self.password_result_ == PasswordResult::Pending ||
                    self.password_result_ == PasswordResult::Rejected ||
                    msg.value("status", std::string{}) != "success" ||
                    msg.value("user_id", std::string{}) != self.expected_viewer_ ||
                    msg.value("transmission_id", std::string{}) != self.status_.device_id) {
                    self.approved_ = false;
                    self.status_.revoked = true;
                    return -1;
                }
                self.join_seen_ = true;
                return 0;
            }
            if (!self.login_.Pending()) return 0;
        }
        const auto result = self.login_.ExchangeWithSigner(stage, msg,
            self.key_.PublicKey(), [&](std::string_view text) { return self.key_.Sign(text); },
            response, capacity);
        if (result == 0 && stage == MiniRtcSignalExchangeStage::IncomingMessage &&
            !self.expected_viewer_.empty() && msg.value("type", std::string{}) == "login" &&
            msg.value("status", std::string{}) == "success") {
            const auto assigned = msg.at("user_id").get<std::string>();
            const auto at = assigned.find('@');
            std::lock_guard<std::mutex> lock(self.mutex_);
            if (at != std::string::npos && valid_password(std::string_view(assigned).substr(at+1)))
                self.registration_password_ = assigned.substr(at+1);
        }
        return result;
    } catch (...) { return -1; }
}
void SignalPeer::signal_status(MiniRtcSignalStatus state, const char* id, size_t size,
                                void* context) noexcept {
    try {
        if (!context) return;
        auto& self = *static_cast<SignalPeer*>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (!self.expected_viewer_.empty() &&
            self.status_.state == MiniRtcSignalConnected && state != MiniRtcSignalConnected) {
            self.approved_ = false;
            self.status_.revoked = true;
            self.status_.viewer_connected = false;
        }
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
void SignalPeer::connection_status(MiniRtcConnectionStatus state, const char* id, size_t size,
                                    void* context) noexcept {
    try {
        if (!context) return;
        auto& self = *static_cast<SignalPeer*>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (self.expected_viewer_.empty()) {
            if (state == MiniRtcConnectionConnected) self.status_.state = MiniRtcSignalFailed;
            return;
        }
        if (!id || std::string_view(id, size) != self.expected_viewer_ ||
            !self.join_seen_ || self.status_.revoked) {
            self.approved_ = false;
            self.status_.revoked = true;
            self.status_.viewer_connected = false;
            return;
        }
        if (state == MiniRtcConnectionConnected) {
            self.status_.viewer_connected = true;
        } else if (self.status_.viewer_connected ||
                   (state != MiniRtcConnectionConnecting && state != MiniRtcConnectionGathering)) {
            self.approved_ = false;
            self.status_.revoked = true;
            self.status_.viewer_connected = false;
        }
    } catch (...) {}
}
void SignalPeer::net_status(const char*, size_t, MiniRtcTraversalMode, const MiniRtcNetTrafficStats*,
                             const char*, size_t, void*) noexcept {
    // The initial report may carry ID@password. Do not log or expose it.
}
void SignalPeer::enable_view_probe(std::string viewer_id, bool allow_input, bool allow_clipboard, bool allow_audio) {
    if (started_ || !login_id_.empty() || viewer_id.rfind("C-",0) != 0 ||
        viewer_id.size() <= 2 || !valid_id(std::string_view(viewer_id).substr(2)))
        throw std::invalid_argument("Expected C-<viewer numeric ID> for a fresh test registration");
    expected_viewer_ = std::move(viewer_id);
    allow_input_ = allow_input;
    allow_clipboard_ = allow_input && allow_clipboard;
    allow_audio_ = allow_audio;
    audio_requested_ = allow_audio;
    if (allow_input_ || allow_audio_) params_.on_receive_data_buffer = receive_input;
}
std::string SignalPeer::take_registration_password() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(registration_password_, {});
}
bool SignalPeer::approve_viewer() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expected_viewer_.empty() || status_.revoked || !status_.viewer_connected ||
        status_.state != MiniRtcSignalConnected) return false;
    approved_ = true;
    return true;
}
bool SignalPeer::view_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return approved_ && !status_.revoked && status_.viewer_connected &&
           status_.state == MiniRtcSignalConnected;
}
bool SignalPeer::send_host_info(const std::string& message) {
    if (!peer_ || !view_active()) return false;
    return MiniRtcSendReliableDataFrameToPeer(peer_, message.data(), message.size(),
        crossdesk::kControlStream, expected_viewer_.data(), expected_viewer_.size()) == 0;
}
bool SignalPeer::send_frame(const MiniRtcVideoFrame& frame) {
    if (!peer_ || !view_active()) return false;
    auto outgoing = frame;
    const auto now = MiniRtcGetSystemTimeMicros(peer_);
    outgoing.captured_timestamp = now > 0 ? static_cast<uint64_t>(now) : 0;
    return MiniRtcSendVideoFrameToPeer(peer_, &outgoing, "Display1",
        expected_viewer_.data(), expected_viewer_.size()) == 0;
}
void SignalPeer::receive_input(const char* data, size_t size, const char* id, size_t id_size,
                               const char* source, size_t source_size, void* context) noexcept {
    try {
        if (!context || !data || !id || !source || !size || size > crossdesk::kMaxClipboardBytes) return;
        auto& self = *static_cast<SignalPeer*>(context);
        if (id_size != self.expected_viewer_.size() ||
            std::string_view(id,id_size) != self.expected_viewer_) return;
        const std::string_view stream(source, source_size);
        const bool clip = stream == crossdesk::kClipboardStream;
        const bool audio_control = stream == crossdesk::kControlStream;
        if (!clip && (size > 32768 || (!audio_control && stream != crossdesk::kMouseStream && stream != crossdesk::kKeyboardStream))) return;
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (!self.approved_ || self.status_.revoked ||
            !self.status_.viewer_connected || self.status_.state != MiniRtcSignalConnected) return;
        if (audio_control) {
            // This probe advertises no preview negotiation. Only the legacy
            // audio_capture command is actionable; ignore preview subscriptions.
            if (!self.allow_audio_ || size > 1024) return;
            const auto msg = nlohmann::json::parse(data,data+size,nullptr,false);
            if (msg.is_object() && msg.contains("type") && msg["type"] == 2 &&
                msg.contains("audio_capture") && msg["audio_capture"].is_boolean())
                self.audio_requested_ = msg["audio_capture"].get<bool>();
            return;
        }
        if (!self.allow_input_) return;
        if (clip) {
            if (self.allow_clipboard_ && !std::memchr(data, '\0', size))
                self.clipboard_ = std::string(data, size);
            return;
        }
        if (self.input_.size() >= 128) {
            // Fail closed on overload; the owner releases held input.
            self.approved_ = false;
            self.status_.revoked = true;
            self.input_.clear();
            return;
        }
        self.input_.push_back({std::string(stream), std::string(data,size)});
    } catch (...) {
        // The callback never injects input or logs payloads.
    }
}
std::vector<SignalPeer::InputMessage> SignalPeer::take_input() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InputMessage> result;
    if (allow_input_ && approved_ && !status_.revoked && status_.viewer_connected &&
        status_.state == MiniRtcSignalConnected) {
        result.reserve(input_.size());
        while (!input_.empty()) {
            result.push_back(std::move(input_.front()));
            input_.pop_front();
        }
    } else {
        input_.clear();
    }
    return result;
}
std::optional<std::string> SignalPeer::take_clipboard() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!allow_clipboard_ || !approved_ || status_.revoked || !status_.viewer_connected ||
        status_.state != MiniRtcSignalConnected) {
        clipboard_.reset();
        return std::nullopt;
    }
    return std::exchange(clipboard_, std::nullopt);
}
bool SignalPeer::send_clipboard(const std::string& text) {
    if (!allow_clipboard_ || !peer_ || !view_active() || text.empty() ||
        text.size() > crossdesk::kMaxClipboardBytes || text.find('\0') != std::string::npos) return false;
    return MiniRtcSendReliableDataFrameToPeer(peer_, text.data(), text.size(),
        crossdesk::kClipboardStream, expected_viewer_.data(), expected_viewer_.size()) == 0;
}
bool SignalPeer::audio_requested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allow_audio_ && audio_requested_ && approved_ && !status_.revoked &&
           status_.viewer_connected && status_.state == MiniRtcSignalConnected;
}
bool SignalPeer::send_audio(const char* data, std::size_t size) {
    if (!peer_ || !audio_requested() || !data || size != 960) return false;
    MiniRtcAudioFrame frame{};
    frame.data = data;
    frame.size = size;
    const auto now = MiniRtcGetSystemTimeMicros(peer_);
    frame.captured_timestamp = now > 0 ? static_cast<uint64_t>(now) : 0;
    return MiniRtcSendAudioFrameToPeer(peer_, &frame, crossdesk::kAudioStream,
        expected_viewer_.data(), expected_viewer_.size()) == 0;
}
bool SignalPeer::request_test_password(const std::string& password) {
    if (!peer_ || expected_viewer_.empty() || !login_id_.empty() ||
        password.size() != 6 || !valid_id(password)) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != MiniRtcSignalConnected || status_.revoked ||
            join_seen_ || password_result_ != PasswordResult::NotRequested) return false;
        password_result_ = PasswordResult::Pending;
    }
    auto message = nlohmann::json{{"type","change_password"},
        {"request_id","redesk-host-probe-password"},{"new_password",password}}.dump();
    const auto result = MiniRtcSendSignalMessage(peer_,message.data(),message.size());
    OPENSSL_cleanse(message.data(),message.size());
    if (result != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        password_result_ = PasswordResult::Rejected;
    }
    return result == 0;
}
SignalPeer::PasswordResult SignalPeer::password_result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return password_result_;
}
}
