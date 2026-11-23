#include "cloudflare_provisioning_service.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace crossdesk::cloudflare {
namespace {
using Json = nlohmann::json;
constexpr const char* kApi = "https://api.cloudflare.com";

httplib::Client Client(const std::string& host) {
  httplib::Client client(host);
  client.set_connection_timeout(5);
  client.set_read_timeout(15);
  client.set_write_timeout(15);
  client.set_max_timeout(30000);
  client.enable_server_certificate_verification(true);
  client.set_follow_location(false);
  return client;
}

httplib::Headers Authorization(const std::string& token) {
  return {{"Authorization", "Bearer " + token}};
}

// Do not forward arbitrary response bodies into UI or logs: Cloudflare can
// echo input or secret values in diagnostic fields.
Result Decode(const httplib::Result& response, Json* result = nullptr) {
  if (!response) return {false, "Network/TLS error contacting Cloudflare"};
  if (response->status == 401 || response->status == 403)
    return {false, "Cloudflare token is invalid or lacks permissions"};
  if (response->status == 429) return {false, "Cloudflare rate limit exceeded"};
  if (response->status < 200 || response->status >= 300)
    return {false, "Cloudflare API HTTP " + std::to_string(response->status)};
  try {
    auto body = Json::parse(response->body);
    if (!body.value("success", false)) return {false, "Cloudflare API rejected request"};
    if (result) *result = body.value("result", Json::object());
    return {true, {}};
  } catch (...) {
    return {false, "Cloudflare API returned malformed JSON"};
  }
}
std::string Prefix(const std::string& id) {
  return "/client/v4/accounts/" + id + "/workers/scripts/redesk-signal";
}
bool IsHex32(const std::string& value) {
  return value.size() == 32 && std::all_of(value.begin(), value.end(),
    [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}
}

bool CloudflareProvisioningService::ValidAccountId(const std::string& value) {
  return IsHex32(value);
}

bool CloudflareProvisioningService::NormalizeWorkerUrl(
    const std::string& value, std::string* base, std::string* signal) {
  // A Worker URL must never be allowed to point a bearer token to an
  // attacker-controlled host. Only this HTTPS URL is used for /health;
  // Cloudflare API tokens are sent exclusively to kApi.
  if (value.rfind("https://", 0) != 0) return false;
  auto hostname_end = value.find_first_of("/?#", 8);
  auto hostname = value.substr(8, hostname_end == std::string::npos
                                  ? hostname_end : hostname_end - 8);
  if (hostname.empty() || hostname.size() > 253 ||
      hostname.find(':') != std::string::npos ||
      hostname.front() == '.' || hostname.back() == '.' ||
      !std::all_of(hostname.begin(), hostname.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '.';
      }))
    return false;
  const auto path = hostname_end == std::string::npos ? "" : value.substr(hostname_end);
  if (!path.empty() && path != "/" && path != "/signal")
    return false;
  if (base) *base = "https://" + hostname;
  if (signal) *signal = hostname + "/signal";
  return true;
}

Result CloudflareProvisioningService::ValidateToken(const std::string& token) const {
  if (token.empty()) return {false, "Cloudflare API Token required"};
  auto client = Client(kApi);
  Json data;
  auto response = client.Get("/client/v4/user/tokens/verify", Authorization(token));
  auto outcome = Decode(response, &data);
  if (!outcome.ok) return outcome;
  return data.value("status", "") == "active"
      ? Result{true, {}} : Result{false, "Cloudflare API Token is not active"};
}

std::vector<Account> CloudflareProvisioningService::ListAccounts(
    const std::string& token, std::string* error) const {
  auto client = Client(kApi);
  Json data;
  auto status = Decode(client.Get("/client/v4/accounts?per_page=50",
                                  Authorization(token)), &data);
  if (!status.ok) {
    if (error) *error = status.error + " (Account ID may be entered manually)";
    return {};
  }
  std::vector<Account> accounts;
  if (data.is_array()) for (const auto& item : data) {
    if (item.is_object()) {
      std::string id = item.value("id", "");
      if (IsHex32(id)) accounts.push_back({id, item.value("name", "")});
    }
  }
  return accounts;
}

BackendStatus CloudflareProvisioningService::CheckHealth(
    const std::string& url) const {
  BackendStatus status;
  std::string base;
  if (!NormalizeWorkerUrl(url, &base, nullptr)) {
    status.diagnostic = "Enter a valid HTTPS Worker URL";
    return status;
  }
  auto client = Client(base);
  const auto response = client.Get("/health");
  if (!response || response->status != 200) {
    status.diagnostic = "The Worker health endpoint is unavailable";
    return status;
  }
  try {
    auto body = Json::parse(response->body);
    status.redesk_health = body.value("ok", false) &&
                           body.value("service", "") == "ReDesk signaling";
    status.turn_configured = body.value("turn_configured", false);
    status.diagnostic = status.redesk_health
        ? "Worker health OK (does not validate remote desktop connectivity)"
        : "The endpoint is not a compatible ReDesk Worker";
    status.url = base;
  } catch (...) {
    status.diagnostic = "Invalid Worker health response";
  }
  return status;
}

BackendStatus CloudflareProvisioningService::Inspect(
    const std::string& token, const std::string& account,
    const std::string& worker_url) const {
  BackendStatus status;
  if (!IsHex32(account)) {
    status.diagnostic = "Invalid Cloudflare Account ID";
    return status;
  }
  auto client = Client(kApi);
  const auto endpoint = Prefix(account);
  auto exists = client.Get(endpoint, Authorization(token));
  if (!exists) {
    status.diagnostic = "Cannot inspect Cloudflare Worker (network error)";
    return status;
  }
  if (exists->status == 404) {
    status.diagnostic = "Worker does not exist";
    return status;
  }
  if (exists->status != 200) {
    status.diagnostic = "Cannot inspect Worker; HTTP " +
                         std::to_string(exists->status);
    return status;
  }
  status.worker_exists = true;
  Json settings;
  auto verified = Decode(
      client.Get(endpoint + "/settings", Authorization(token)), &settings);
  if (!verified.ok) {
    status.diagnostic = "Worker exists but settings cannot be verified: " +
                        verified.error;
    return status;
  }
  if (settings.contains("bindings") && settings["bindings"].is_array()) {
    for (const auto& binding : settings["bindings"]) {
      if (binding.is_object() &&
          binding.value("type", "") == "durable_object_namespace" &&
          binding.value("name", "") == "SIGNAL_HUB" &&
          binding.value("class_name", "") == "SignalHub") {
        status.durable_object_binding = true;
      }
    }
  }
  if (!worker_url.empty()) {
    auto health = CheckHealth(worker_url);
    status.redesk_health = health.redesk_health;
    status.turn_configured = health.turn_configured;
    status.url = health.url;
    status.diagnostic = health.diagnostic;
  } else {
    status.diagnostic = status.durable_object_binding
        ? "Worker and SignalHub binding found; provide URL for health verification"
        : "Worker exists but expected Durable Object binding is missing";
  }
  return status;
}

Result CloudflareProvisioningService::ConfigureExisting(
    const SetupOptions& options, std::string* signal_address) const {
  if (options.existing_action != ExistingAction::UseExisting)
    return {false, "Only Use Existing is allowed without explicit deployment"};
  const auto health = CheckHealth(options.worker_url);
  if (!health.redesk_health) return {false, health.diagnostic};
  return NormalizeWorkerUrl(options.worker_url, nullptr, signal_address)
      ? Result{true, {}} : Result{false, "Invalid Worker URL"};
}

Result CloudflareProvisioningService::Deploy(
    const std::string& token, const SetupOptions& options,
    const std::string& worker_script, Progress progress) const {
  if (!IsHex32(options.account_id)) return {false, "Invalid Cloudflare Account ID"};
  if (worker_script.empty()) return {false, "Bundled Worker script is missing"};
  if (options.existing_action == ExistingAction::ReinitializeData)
    return {false, "Durable Object data reset is not supported; nothing was deleted"};
  if (progress) progress("Checking Existing Worker", "Running");
  auto status = Inspect(token, options.account_id, options.worker_url);
  if (status.worker_exists) {
    if (options.existing_action != ExistingAction::UpdateCode ||
        !options.confirmed_worker_update)
      return {false, "Worker exists: explicitly confirm Update Worker first"};
    if (!status.durable_object_binding || !status.redesk_health)
      return {false, "Cannot prove existing Worker is compatible; refusing overwrite"};
  } else if (status.diagnostic != "Worker does not exist") {
    return {false, "Could not safely determine Worker existence: " + status.diagnostic};
  }
  if (progress) progress("Deploying Worker", "Running");
  // The first migration is sent only for new Workers. Existing Workers
  // retain their Durable Object namespace, data and secret bindings.
  Json metadata = {
    {"main_module", "worker.js"},
    {"compatibility_date", "2026-11-01"},
    {"bindings", Json::array({
      Json{{"type", "durable_object_namespace"},
           {"name", "SIGNAL_HUB"}, {"class_name", "SignalHub"}}
    })}
  };
  if (!status.worker_exists) {
    metadata["migrations"] = Json::array({
      Json{{"tag", "v1"}, {"new_sqlite_classes", Json::array({"SignalHub"})}}
    });
  } else {
    // Inheritance is explicit per binding, and strict mode rejects missing
    // secrets instead of silently removing them during deployment.
    metadata["bindings"].push_back(
        Json{{"type", "inherit"}, {"name", "TURN_KEY_ID"}});
    metadata["bindings"].push_back(
        Json{{"type", "inherit"}, {"name", "TURN_API_TOKEN"}});
  }
  httplib::MultipartFormDataItems parts = {
    {"metadata", metadata.dump(), "", "application/json"},
    {"worker.js", worker_script, "worker.js", "application/javascript+module"}
  };
  auto client = Client(kApi);
  auto response = client.Put(Prefix(options.account_id) +
      (status.worker_exists ? "?bindings_inherit=strict" : ""),
      Authorization(token), parts);
  return Decode(response);
}

Result CloudflareProvisioningService::AddTurnSecrets(
    const std::string& token, const std::string& account,
    const std::string& key_id, const std::string& secret) const {
  if (!IsHex32(account) || !IsHex32(key_id) || secret.size() != 64 ||
      !std::all_of(secret.begin(), secret.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
      })) return {false, "Invalid TURN key metadata"};
  auto client = Client(kApi);
  const auto path = Prefix(account) + "/secrets";
  for (const auto& name_and_value : {
        std::pair<const char*, const std::string&>{"TURN_KEY_ID", key_id},
        std::pair<const char*, const std::string&>{"TURN_API_TOKEN", secret}}) {
    const Json body = {{"name", name_and_value.first},
                       {"text", name_and_value.second},
                       {"type", "secret_text"}};
    auto response = client.Put(path, Authorization(token), body.dump(),
                               "application/json");
    auto result = Decode(response);
    if (!result.ok) return result;
  }
  return {true, {}};
}
} // namespace crossdesk::cloudflare
