/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-02
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DEVICE_LOGIN_H_
#define _DEVICE_LOGIN_H_

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <functional>
#include <algorithm>
#include <cstring>

#include "minirtc.h"

namespace crossdesk {
class TrustedDevices;

// One signaling connection's application authentication, owned by its callbacks.
class DeviceLogin {
 public:
  int Exchange(MiniRtcSignalExchangeStage stage, const nlohmann::json& message,
               const TrustedDevices* identity, char* response, size_t capacity);
  using Signer = std::function<std::string(std::string_view)>;
  // The same wire protocol is used by the desktop and the Host-only build.
  // Public key and signing function belong to one server identity.
  int ExchangeWithSigner(MiniRtcSignalExchangeStage stage, const nlohmann::json& message,
      const std::string& public_key, const Signer& sign, char* response, size_t capacity) {
  auto output = [&](const nlohmann::json& reply) {
    const auto text = reply.dump();
    if (text.empty() || text.size() > capacity) return -1;
    std::memcpy(response, text.data(), text.size());
    return static_cast<int>(text.size());
  };
  if (stage == MiniRtcSignalExchangeStage::PrepareLogin) {
    phase_ = Phase::Idle;
    used_password_ = false;
    password_.clear();
    request_ = message;
    const auto login = message.at("user_id").get<std::string>();
    if (public_key.empty() || !sign) return -1;  // Never fall back when local key storage fails.
    const auto at = login.find('@');
    request_["user_id"] = login.substr(0, at);
    if (at != std::string::npos) password_ = login.substr(at + 1);
    request_["device_key"] = {{"version", 1}, {"public_key", public_key}};
    request_.erase("reconnect_token");
    request_.erase("reconnect_version");
    phase_ = Phase::AwaitingChallenge;
    return output(request_);
  }
  if (!Pending()) return 0;
  const auto type = message.value("type", "");
  if (type == "login_challenge") {
    if (public_key.empty() || !sign || phase_ != Phase::AwaitingChallenge || message.at("device_key_version") != 1 ||
        message.at("user_id") != request_.at("user_id")) return -1;
    const auto nonce = message.at("nonce").get<std::string>();
    if (nonce.size() != 64 || !std::all_of(nonce.begin(), nonce.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
      return -1;
    const auto id = request_.at("user_id").get<std::string>();
    auto reply = request_;
    used_password_ = message.at("password_required").get<bool>();
    if (used_password_) {
      if (id.empty() || id.rfind("C-", 0) == 0 || password_.empty()) return -1;
      reply["user_id"] = id + "@" + password_;
    }
    const auto transcript = nlohmann::json::array(
        {"crossdesk.device-login.v1", id, public_key, nonce}).dump();
    reply["device_key"]["nonce"] = nonce;
    reply["device_key"]["signature"] = sign(transcript);
    phase_ = Phase::AwaitingResult;
    password_.clear();
    return output(reply);
  }
  if (type == "login") {
    if (message.at("status") == "success") {
      if (phase_ != Phase::AwaitingResult || message.value("device_key_version", 0) != 1) return -1;
      const auto assigned = message.at("user_id").get<std::string>();
      const auto expected = request_.at("user_id").get<std::string>();
      if (assigned.empty() || (!expected.empty() && assigned != expected)) return -1;
    }
    password_.clear();
    request_.clear();
    phase_ = Phase::Idle;
  }
  return 0;
  }
  bool Pending() const { return phase_ != Phase::Idle; }
  bool UsedPassword() const { return used_password_; }

 private:
  enum class Phase { Idle, AwaitingChallenge, AwaitingResult };
  nlohmann::json request_;
  std::string password_;
  Phase phase_ = Phase::Idle;
  bool used_password_ = false;
};
}  // namespace crossdesk

#endif
