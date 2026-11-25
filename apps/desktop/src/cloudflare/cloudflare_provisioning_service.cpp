#include "cloudflare_provisioning_service.h"
#include "bundled_worker.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

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
  if (response->status == 401)
    return {false, "Cloudflare returned HTTP 401 (authentication failed). Check the API Token."};
  if (response->status == 403)
    return {false, "Cloudflare returned HTTP 403 (permission denied). Check the token's Account scope and API permissions."};
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
// Staged recovery for freshly created TURN credentials. A TURN secret is
// returned only once. Never create a replacement key after partial failure.
// Windows DPAPI protects the pending file using the current user context.
#if defined(_WIN32)
std::string RecoveryFile(const std::string& account) {
  const char* local = std::getenv("LOCALAPPDATA");
  if (!local || !*local) return {};
  return std::string(local) + "\\ReDesk\\cloudflare-turn-" + account + ".bin";
}
bool SavePendingKey(const std::string& account, const std::string& uid,
                    const std::string& secret) {
  const auto path = RecoveryFile(account);
  if (path.empty()) return false;
  const std::string plain = uid + "\n" + secret;
  DATA_BLOB input{static_cast<DWORD>(plain.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
  DATA_BLOB encrypted{};
  if (!CryptProtectData(&input, L"ReDesk TURN recovery", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) return false;
  const auto root = path.substr(0, path.find_last_of("\\"));
  CreateDirectoryA(root.c_str(), nullptr);
  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  bool ok = false;
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    ok = WriteFile(file, encrypted.pbData, encrypted.cbData, &written, nullptr) &&
         written == encrypted.cbData && FlushFileBuffers(file);
    CloseHandle(file);
  }
  LocalFree(encrypted.pbData);
  return ok;
}
bool LoadPendingKey(const std::string& account,
                    std::string* uid, std::string* secret) {
  const auto path = RecoveryFile(account);
  if (path.empty()) return false;
  HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD size = GetFileSize(file, nullptr);
  bool loaded = false;
  if (size > 0 && size <= 2048) {
    std::vector<BYTE> bytes(size);
    DWORD read = 0;
    if (ReadFile(file, bytes.data(), size, &read, nullptr) && read == size) {
      DATA_BLOB input{size, bytes.data()}, decrypted{};
      if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &decrypted)) {
        std::string plain(reinterpret_cast<char*>(decrypted.pbData),
                          decrypted.cbData);
        auto sep = plain.find('\n');
        if (sep != std::string::npos) {
          *uid = plain.substr(0, sep);
          *secret = plain.substr(sep + 1);
          loaded = IsHex32(*uid) && secret->size() == 64;
        }
        SecureZeroMemory(decrypted.pbData, decrypted.cbData);
        LocalFree(decrypted.pbData);
      }
    }
  }
  CloseHandle(file);
  return loaded;
}
void ClearPendingKey(const std::string& account) {
  const auto file = RecoveryFile(account);
  if (!file.empty()) DeleteFileA(file.c_str());
}
#else
bool SavePendingKey(const std::string&, const std::string&,
                    const std::string&) { return false; }
bool LoadPendingKey(const std::string&, std::string*, std::string*) { return false; }
void ClearPendingKey(const std::string&) {}
#endif


}

bool CloudflareProvisioningService::HasPendingTurnRecovery(
    const std::string& account) {
  std::string uid, secret;
  return IsHex32(account) && LoadPendingKey(account, &uid, &secret);
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

std::string CloudflareProvisioningService::TokenVerifyPath(
    const std::string& token, const std::string& account_id) {
  if (token.rfind("cfat_", 0) == 0) {
    if (!IsHex32(account_id)) return {};
    return "/client/v4/accounts/" + account_id + "/tokens/verify";
  }
  return "/client/v4/user/tokens/verify";
}

Result CloudflareProvisioningService::ValidateToken(
    const std::string& token, const std::string& account_id) const {
  if (token.empty()) return {false, "Cloudflare API Token required"};
  const std::string verify_path = TokenVerifyPath(token, account_id);
  if (verify_path.empty())
    return {false, "Account-owned API Token requires a valid 32-character Account ID."};

  auto client = Client(kApi);
  auto verify_at = [&client, &token](const std::string& path) -> Result {
    Json data;
    auto checked = Decode(client.Get(path, Authorization(token)), &data);
    if (!checked.ok) return checked;
    if (!data.is_object() || data.value("status", "") != "active")
      return {false, "Cloudflare API Token is not active"};
    return {true, {}};
  };
  auto result = verify_at(verify_path);
  if (result.ok) return result;
  // Old (unprefixed) tokens are ambiguous. Cloudflare supports both user-
  // owned and account-owned legacy formats, so try account verification
  // after user verification without mistaking the first 403 for invalidity.
  if (token.rfind("cfat_", 0) != 0 &&
      token.rfind("cfut_", 0) != 0 && IsHex32(account_id)) {
    const auto account_result = verify_at(
        "/client/v4/accounts/" + account_id + "/tokens/verify");
    if (account_result.ok) return account_result;
    return {false,
        "Neither user nor account token verification succeeded. "
        "Check token status, Account ID and permission scope."};
  }
  return result;
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
  if (!response) {
    status.diagnostic = "Could not reach Worker /health (network, TLS or DNS error)";
    return status;
  }
  if (response->status != 200) {
    status.diagnostic = "Worker /health returned HTTP " +
                        std::to_string(response->status);
    return status;
  }
  try {
    auto body = Json::parse(response->body);
    status.redesk_health = body.value("ok", false) &&
                           body.value("service", "") == "ReDesk signaling";
    status.turn_configured = body.value("turn_configured", false);
    if (body.contains("version") && body["version"].is_string()) {
      const auto parsed = body["version"].get<std::string>();
      if (parsed.size() <= 32 &&
          std::all_of(parsed.begin(), parsed.end(), [](unsigned char ch) {
            return std::isdigit(ch) || ch == '.';
          })) status.worker_version = parsed;
    }
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
  if (!settings.is_object() || !settings.contains("bindings") ||
      !settings["bindings"].is_array()) {
    status.diagnostic = "Worker bindings could not be verified";
    return status;
  }
  for (const auto& binding : settings["bindings"]) {
    if (!binding.is_object()) {
      status.unexpected_bindings = true;
      continue;
    }
    const std::string type = binding.value("type", "");
    const std::string name = binding.value("name", "");
    if (type == "durable_object_namespace" && name == "SIGNAL_HUB" &&
        binding.value("class_name", "") == "SignalHub") {
      status.durable_object_binding = true;
    } else if (type == "secret_text" && name == "TURN_KEY_ID") {
      status.turn_key_id_binding = true;
    } else if (type == "secret_text" && name == "TURN_API_TOKEN") {
      status.turn_api_token_binding = true;
    } else {
      status.unexpected_bindings = true;
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

bool CloudflareProvisioningService::IsUpdateAvailable(
    const std::string& deployed_version) {
  // An old Worker without a version may be updated only when the account
  // binding checks have already proven its identity.
  if (deployed_version.empty()) return true;
  auto parse = [](const std::string& value, int out[3]) {
    std::size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
      auto end = value.find('.', pos);
      if ((i < 2 && end == std::string::npos) ||
          (i == 2 && end != std::string::npos)) return false;
      auto component = value.substr(pos, end == std::string::npos
                                          ? end : end - pos);
      if (component.empty() || component.size() > 8 ||
          !std::all_of(component.begin(), component.end(),
              [](unsigned char ch) { return std::isdigit(ch); }))
        return false;
      try { out[i] = std::stoi(component); }
      catch (...) { return false; }
      pos = end + 1;
    }
    return true;
  };
  int deployed[3]{}, current[3]{};
  if (!parse(deployed_version, deployed) ||
      !parse(kReDeskWorkerVersion, current)) return false;
  for (int i = 0; i < 3; ++i) {
    if (deployed[i] < current[i]) return true;
    if (deployed[i] > current[i]) return false;
  }
  return false;
}

bool CloudflareProvisioningService::MayUpdateWorker(
    const BackendStatus& status) {
  return status.worker_exists && status.durable_object_binding &&
         status.redesk_health && !status.unexpected_bindings;
}

BackendReview CloudflareProvisioningService::WithHealthStatus(
    const BackendReview& previous, const BackendStatus& health) {
  auto updated = previous;
  updated.redesk_health = health.redesk_health;
  updated.turn_configured = health.turn_configured;
  updated.worker_version = health.worker_version;
  updated.worker_exists = previous.worker_exists || health.redesk_health;
  updated.update_safe = health.redesk_health && previous.worker_bindings_verified &&
                        IsUpdateAvailable(health.worker_version);
  return updated;
}

BackendReview CloudflareProvisioningService::RetryHealth(
    const BackendReview& previous) const {
  // Health is public and independent of the token. Keep the account-level
  // evidence recorded during discovery, but never promote an unverified
  // Worker into the code-update path.
  BackendStatus health;
  for (int attempt = 0; attempt < 6; ++attempt) {
    health = CheckHealth(previous.worker_url);
    if (health.redesk_health && (health.turn_configured ||
        !previous.pending_turn_recovery)) break;
    if (attempt < 5) std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  auto refreshed = WithHealthStatus(previous, health);
  if (refreshed.redesk_health) {
    refreshed.explanation = refreshed.update_safe
        ? "ReDesk /health is healthy. Use Existing Backend is available; "
          "code update is also permitted after explicit confirmation."
        : "ReDesk /health is healthy. Use Existing Backend is available. "
          "Code update remains disabled because Worker bindings could not be verified.";
    if (!refreshed.worker_version.empty())
      refreshed.explanation += " Deployed version: " + refreshed.worker_version + ".";
    if (!IsUpdateAvailable(refreshed.worker_version))
      refreshed.explanation += " No newer bundled version is available.";
  } else {
    refreshed.explanation =
        "ReDesk /health is unavailable: " + health.diagnostic +
        ". Use Existing and Update remain disabled. Confirm that the "
        "workers.dev URL is correct and the Worker route is enabled.";
  }
  return refreshed;
}

BackendDiscovery CloudflareProvisioningService::DiscoverBackend(
    const std::string& token, const std::string& requested_account,
    const std::string& requested_url) const {
  BackendDiscovery discovery;
  discovery.needs_account_id = true; // Account-scoped template tokens cannot list accounts.
  if (token.empty()) {
    discovery.message = "Enter your Cloudflare API Token.";
    return discovery;
  }
  if (!IsHex32(requested_account)) {
    discovery.message = "Enter the 32-character Cloudflare Account ID for this token.";
    return discovery;
  }
  const auto verified = ValidateToken(token, requested_account);
  if (!verified.ok) { discovery.message = verified.error; return discovery; }
  discovery.token_verified = true;
  discovery.account_id = requested_account;

  // Check the Worker FIRST. A new account may legitimately have no
  // workers.dev subdomain. This must not block the fresh install flow.
  const auto worker = Inspect(token, requested_account);
  if (!worker.worker_exists && worker.diagnostic != "Worker does not exist") {
    discovery.message = "Could not verify Worker absence: " + worker.diagnostic;
    return discovery;
  }

  std::string url = requested_url;
  if (url.empty()) {
    auto client = Client(kApi);
    Json subdomain;
    auto response = client.Get("/client/v4/accounts/" + requested_account +
                               "/workers/subdomain", Authorization(token));
    if (response && response->status == 404 && !worker.worker_exists) {
      // A documented account initialization state. The candidate is created
      // only after explicit Install confirmation, not during Fetch.
      discovery.needs_worker_url = true;
      // Deterministic, account-specific candidate. It is not reserved or
      // guaranteed available until Cloudflare accepts it during Install.
      discovery.suggested_worker_url =
          "https://redesk-signal.redesk-" + requested_account.substr(0, 12) +
          ".workers.dev";
      // Review the generated candidate immediately. A second Fetch must
      // not be necessary merely to accept the proposed URL.
      discovery.review = ReviewAccount(token, requested_account,
                                       discovery.suggested_worker_url);
      discovery.review_ready = discovery.review.ok;
      discovery.message = discovery.review_ready
          ? discovery.review.explanation
          : "A Worker URL has been suggested. You may edit it and retry: " +
                discovery.review.explanation;
      return discovery;
    } else {
      auto resolved = Decode(response, &subdomain);
      if (!resolved.ok) {
        if (worker.worker_exists) {
          discovery.needs_worker_url = true;
          discovery.message = "Existing Worker found; could not discover its URL (" +
                              resolved.error + "). Enter the Worker HTTPS URL.";
        } else {
          discovery.message = "Could not verify Workers subdomain state: " +
                              resolved.error + ". Nothing was installed.";
        }
        return discovery;
      }
      const auto name = subdomain.is_object()
          ? subdomain.value("subdomain", "") : std::string{};
      url = "https://redesk-signal." + name + ".workers.dev";
    }
  }
  if (!NormalizeWorkerUrl(url, nullptr, nullptr)) {
    discovery.needs_worker_url = true;
    discovery.message = "Enter a valid HTTPS Worker URL.";
    return discovery;
  }

  discovery.review = ReviewAccount(token, requested_account, url);
  discovery.review_ready = discovery.review.ok;
  discovery.message = discovery.review.explanation;
  return discovery;
}

BackendReview CloudflareProvisioningService::ReviewAccount(
    const std::string& token, const std::string& account,
    const std::string& provided_url) const {
  BackendReview review;
  review.account_id = account;
  if (!IsHex32(account)) {
    review.explanation = "Invalid Cloudflare Account ID";
    return review;
  }
  const auto checked = ValidateToken(token, account);
  if (!checked.ok) { review.explanation = checked.error; return review; }
  if (!NormalizeWorkerUrl(provided_url, &review.worker_url, nullptr)) {
    review.explanation = "Enter a valid HTTPS Worker URL";
    return review;
  }

  const auto worker = Inspect(token, account);
  if (!worker.worker_exists && worker.diagnostic != "Worker does not exist") {
    review.explanation = "Cannot confirm Worker absence: " + worker.diagnostic;
    return review;
  }
  review.worker_exists = worker.worker_exists;
  review.durable_object_binding = worker.durable_object_binding;
  review.worker_bindings_verified =
      worker.worker_exists && worker.durable_object_binding &&
      !worker.unexpected_bindings;
  std::string health_diagnostic;
  if (worker.worker_exists) {
    const auto health = CheckHealth(review.worker_url);
    health_diagnostic = health.diagnostic;
    review = WithHealthStatus(review, health);
  }

  // Inspect TURN without changing keys. A missing list permission blocks
  // installation, but does not block reuse of a compatible Worker.
  auto client = Client(kApi);
  Json keys;
  auto listed = Decode(client.Get("/client/v4/accounts/" + account +
                                  "/calls/turn_keys", Authorization(token)), &keys);
  if (listed.ok && keys.is_array()) {
    review.turn_keys_checked = true;
    for (const auto& key : keys) {
      if (key.is_object() && key.value("name", "") == "redesk-signal") {
        review.existing_turn_key = true;
        break;
      }
    }
  }
  review.pending_turn_recovery =
      HasPendingTurnRecovery(account) &&
      (!review.worker_exists ||
       (review.worker_bindings_verified && review.redesk_health));
  review.ok = true;
  if (review.redesk_health) {
    review.explanation = review.update_safe
        ? "Existing Worker is healthy. You may use or update it after confirmation."
        : "Existing Worker is healthy. Use Existing is available.";
    if (!review.worker_version.empty())
      review.explanation += " Version: " + review.worker_version + ".";
    if (review.pending_turn_recovery && !review.turn_configured)
      review.explanation += " Pending TURN recovery is available on this Windows account.";
  } else if (review.worker_exists) {
    review.explanation =
        "Worker exists, but /health verification failed at " +
        review.worker_url + ": " + health_diagnostic +
        ". Retry Health Check; no installation or overwrite is permitted "
        "until this Worker is verified.";
  } else if (!review.turn_keys_checked) {
    review.explanation = "No Worker was found, but TURN keys cannot be listed. "
                         "Installation is blocked until the token can inspect them.";
  } else if (review.existing_turn_key) {
    review.explanation = review.pending_turn_recovery
        ? "ReDesk TURN key exists with local encrypted recovery. You can resume setup."
        : "No Worker was found, but a ReDesk TURN key already exists without local recovery. "
          "Installation is blocked to prevent credential loss.";
  } else {
    review.explanation = "No Worker or ReDesk TURN key found. "
                         "Ready to install a new Worker, Durable Object and TURN.";
  }
  return review;
}

Result CloudflareProvisioningService::DiscardOrphanTurnRecovery(
    const std::string& token, const std::string& account) const {
#if !defined(_WIN32)
  return {false, "TURN recovery reconciliation is supported on Windows only"};
#else
  if (!IsHex32(account) || token.empty())
    return {false, "Valid Account ID and API token are required"};
  std::string uid, secret;
  if (!LoadPendingKey(account, &uid, &secret))
    return {false, "There is no readable encrypted local TURN recovery to reconcile"};
  auto validity = ValidateToken(token, account);
  if (!validity.ok) return validity;
  auto worker = Inspect(token, account);
  if (worker.diagnostic != "Worker does not exist" || worker.worker_exists)
    return {false, "Cannot discard local TURN recovery while a Worker exists "
                   "or its absence cannot be verified"};
  auto client = Client(kApi);
  Json keys;
  auto listed = Decode(client.Get(
      "/client/v4/accounts/" + account + "/calls/turn_keys",
      Authorization(token)), &keys);
  if (!listed.ok || !keys.is_array())
    return {false, "Cannot verify ReDesk TURN key absence; local recovery was preserved"};
  for (const auto& key : keys) {
    if (key.is_object() &&
        (key.value("name", "") == "redesk-signal" ||
         key.value("uid", "") == uid))
      return {false, "A ReDesk TURN key or matching key ID still exists in "
                     "Cloudflare. Nothing was removed"};
  }
  const auto path = RecoveryFile(account);
  if (path.empty() || !DeleteFileA(path.c_str()))
    return {false, "Could not remove the orphaned encrypted local recovery file"};
  return {true, {}};
#endif
}

Result CloudflareProvisioningService::DeleteOrphanTurnKey(
    const std::string& token, const std::string& account) const {
  if (!IsHex32(account) || token.empty())
    return {false, "Valid Account ID and API token are required"};
  const auto verified = ValidateToken(token, account);
  if (!verified.ok) return verified;
  const auto worker = Inspect(token, account);
  if (worker.worker_exists || worker.diagnostic != "Worker does not exist")
    return {false, "Cannot delete TURN key until ReDesk Worker absence is verified"};
  if (HasPendingTurnRecovery(account))
    return {false, "Local TURN recovery exists. Reconcile local state first; no key was deleted"};
  auto client = Client(kApi);
  const std::string path = "/client/v4/accounts/" + account + "/calls/turn_keys";
  Json keys;
  auto listed = Decode(client.Get(path, Authorization(token)), &keys);
  if (!listed.ok || !keys.is_array())
    return {false, "Cannot safely enumerate Cloudflare TURN keys"};
  std::string uid;
  for (const auto& key : keys) {
    if (!key.is_object() || key.value("name", "") != "redesk-signal")
      continue;
    const auto candidate = key.value("uid", "");
    if (!IsHex32(candidate) || !uid.empty())
      return {false, "Multiple or invalid ReDesk TURN keys found. Nothing was deleted"};
    uid = candidate;
  }
  if (uid.empty())
    return {false, "ReDesk TURN key is no longer present. Check again before installing"};
  // Re-read the exact key before deleting; a display name alone is not
  // sufficient authority for a destructive request.
  Json current;
  auto checked = Decode(client.Get(path + "/" + uid, Authorization(token)), &current);
  if (!checked.ok || !current.is_object() ||
      current.value("uid", "") != uid ||
      current.value("name", "") != "redesk-signal")
    return {false, "TURN key identity changed before deletion; nothing was deleted"};
  return Decode(client.Delete(path + "/" + uid, Authorization(token)));
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
    if (!MayUpdateWorker(status))
      return {false, "Worker compatibility/bindings cannot be proven; refusing overwrite"};
    if (!IsUpdateAvailable(status.worker_version))
      return {false, "The deployed Worker version is already current or newer. No update was performed."};
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
    metadata["migrations"] =
        Json{{"new_tag", "v1"}, {"new_sqlite_classes", Json::array({"SignalHub"})}};
  } else {
    // Inherit only existing secret bindings; strict mode prevents
    // concurrent deletion from silently dropping a credential.
    if (status.turn_key_id_binding)
      metadata["bindings"].push_back(
          Json{{"type", "inherit"}, {"name", "TURN_KEY_ID"}});
    if (status.turn_api_token_binding)
      metadata["bindings"].push_back(
          Json{{"type", "inherit"}, {"name", "TURN_API_TOKEN"}});
  }
  httplib::UploadFormDataItems parts = {
    {"metadata", metadata.dump(), "", "application/json"},
    {"worker.js", worker_script, "worker.js", "application/javascript+module"}
  };
  auto client = Client(kApi);
  auto response = client.Put(Prefix(options.account_id) +
      (status.worker_exists ? "?bindings_inherit=strict" : ""),
      Authorization(token), parts);
  return Decode(response);
}

Result CloudflareProvisioningService::ProvisionNew(
    const std::string& token, const std::string& account,
    const std::string& script, std::string* worker_url,
    Progress progress) const {
  if (!IsHex32(account) || token.empty() || script.empty())
    return {false, "Provide a valid token, Account ID and bundled Worker"};
#if !defined(_WIN32)
  // Fail before ANY Cloudflare mutation until the platform has durable,
  // user-scoped recovery for the one-time TURN bearer secret.
  return {false, "Fresh Cloudflare deployment currently requires Windows DPAPI TURN recovery."};
#endif
  if (progress) progress("Validating Cloudflare Account", "Running");
  auto validity = ValidateToken(token, account);
  if (!validity.ok) return validity;

  if (progress) progress("Checking Existing Worker", "Running");
  auto prior = Inspect(token, account);
  std::string pending_uid, pending_secret;
  const bool has_pending = LoadPendingKey(account, &pending_uid, &pending_secret);
  if (prior.worker_exists) {
    if (!has_pending || !prior.durable_object_binding)
      return {false, "Worker already exists. Use Existing Backend or confirm an update."};
    // Recover only if the Worker has a verified SignalHub binding and the
    // current /health still identifies it as the ReDesk signaling service.
    if (!worker_url)
      return {false, "Pending TURN recovery exists, but Worker URL is missing"};
    bool verified_identity = false;
    std::string health_error;
    for (int attempt = 0; attempt < 6; ++attempt) {
      const auto health = CheckHealth(*worker_url);
      if (health.redesk_health) { verified_identity = true; break; }
      health_error = health.diagnostic;
      if (attempt < 5) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!verified_identity)
      return {false, "Pending TURN recovery is safe but Worker identity could not "
                     "be verified via /health: " + health_error +
                     ". No TURN keys were changed."};
    Json keys;
    auto client = Client(kApi);
    auto listed = Decode(client.Get("/client/v4/accounts/" + account +
                         "/calls/turn_keys", Authorization(token)), &keys);
    if (!listed.ok || !keys.is_array())
      return {false, "Cannot inspect TURN keys before recovery"};
    bool matches = false;
    for (const auto& key : keys)
      if (key.is_object() && key.value("uid", "") == pending_uid &&
          key.value("name", "") == "redesk-signal") matches = true;
    if (!matches) return {false, "Pending TURN key no longer matches Cloudflare"};
    const auto set = AddTurnSecrets(token, account, pending_uid, pending_secret);
    if (!set.ok) return {false, "TURN recovery failed: " + set.error};
    bool recovered_health = false;
    for (int attempt = 0; attempt < 6; ++attempt) {
      const auto health = CheckHealth(*worker_url);
      if (health.redesk_health && health.turn_configured) {
        recovered_health = true;
        break;
      }
      if (attempt < 5) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!recovered_health)
      return {false, "TURN recovery installed secrets, but /health has not yet "
                     "confirmed TURN. The encrypted recovery is preserved; "
                     "retry later without creating a new key."};
    ClearPendingKey(account);
    return {true, {}};
  }
  if (prior.diagnostic != "Worker does not exist")
    return {false, "Cannot safely determine Worker existence: " + prior.diagnostic};

  auto client = Client(kApi);
  if (progress) progress("Configuring TURN", "Running");
  // Cloudflare cannot reveal existing TURN secrets. Do not generate duplicates
  // or rotate keys that other installations may already be using.
  Json keys;
  const auto turn_path = "/client/v4/accounts/" + account + "/calls/turn_keys";
  auto listed = Decode(client.Get(turn_path, Authorization(token)), &keys);
  if (!listed.ok) return {false, "Could not inspect existing TURN keys: " + listed.error};
  if (!keys.is_array()) return {false, "Unexpected TURN key list"};
  bool matched_pending_key = false;
  for (const auto& key : keys) {
    if (key.is_object() && key.value("name", "") == "redesk-signal") {
      if (!has_pending || key.value("uid", "") != pending_uid)
        return {false, "Existing ReDesk TURN key cannot be safely recovered. "
                       "No duplicate key will be created."};
      matched_pending_key = true;
    }
  }
  if (has_pending && !matched_pending_key)
    return {false, "Encrypted TURN recovery exists, but Cloudflare key is missing. "
                   "Manual reconciliation required."};

  const std::string subdomain_path =
      "/client/v4/accounts/" + account + "/workers/subdomain";
  Json subdomain;
  auto current = client.Get(subdomain_path, Authorization(token));
  if (!current) return {false, "Could not check Cloudflare Workers subdomain"};
  std::string name;
  if (current->status == 404) {
    // The account is new. Never invent or overwrite an account subdomain.
    // The user explicitly supplies the intended workers.dev URL first.
    std::string requested_base;
    if (!worker_url || !NormalizeWorkerUrl(*worker_url, &requested_base, nullptr))
      return {false, "Enter the desired new Worker URL before Install"};
    constexpr const char* kPrefix = "https://redesk-signal.";
    constexpr const char* kSuffix = ".workers.dev";
    if (requested_base.rfind(kPrefix, 0) != 0 ||
        requested_base.size() <= std::strlen(kPrefix) + std::strlen(kSuffix) ||
        requested_base.substr(requested_base.size() - std::strlen(kSuffix)) != kSuffix)
      return {false, "Expected https://redesk-signal.YOUR-SUBDOMAIN.workers.dev"};
    name = requested_base.substr(std::strlen(kPrefix),
        requested_base.size() - std::strlen(kPrefix) - std::strlen(kSuffix));
    if (name.size() < 3 || name.size() > 63 ||
        !std::all_of(name.begin(), name.end(), [](unsigned char ch) {
          return std::islower(ch) || std::isdigit(ch) || ch == '-';
        }) || name.front() == '-' || name.back() == '-')
      return {false, "Workers subdomain must contain 3-63 lowercase letters, digits or hyphens"};
    if (progress) progress("Creating Workers Subdomain", "Running");
    auto created = Decode(client.Put(subdomain_path, Authorization(token),
        Json{{"subdomain", name}}.dump(), "application/json"), &subdomain);
    if (!created.ok) return {false, "Workers subdomain setup failed: " + created.error};
  } else {
    auto resolved = Decode(current, &subdomain);
    if (!resolved.ok) return {false, "Could not resolve Workers subdomain: " + resolved.error};
    name = subdomain.is_object() ? subdomain.value("subdomain", "") : "";
  }
  const std::string url = "https://redesk-signal." + name + ".workers.dev";
  std::string base_url;
  if (!NormalizeWorkerUrl(url, &base_url, nullptr))
    return {false, "Cloudflare returned an invalid Workers subdomain"};
  if (worker_url) *worker_url = base_url;

  if (progress) progress("Deploying Worker", "Running");
  SetupOptions options;
  options.account_id = account;
  auto deployment = Deploy(token, options, script);
  if (!deployment.ok) return deployment;
  auto enabled = Decode(client.Post(Prefix(account) + "/subdomain",
      Authorization(token),
      Json{{"enabled", true}, {"previews_enabled", false}}.dump(),
      "application/json"));
  if (!enabled.ok) return {false, "Worker deployed but workers.dev activation failed: " + enabled.error};

  // Create a dedicated key only after the Worker is deployed. No existing key
  // is removed. If a later step fails, report partial setup, never claim
  // success or silently create a second key on retry.
  if (progress) progress("Configuring TURN", "Running");
#if !defined(_WIN32)
  return {false, "Worker deployed. Automatic TURN setup requires Windows "
                 "DPAPI credential recovery and is not available on this OS."};
#else
  // Fail before creating a key if the current Windows identity cannot
  // initialize DPAPI. Never create untracked or duplicate credentials.
  DATA_BLOB probe{1, reinterpret_cast<BYTE*>(const_cast<char*>("x"))};
  DATA_BLOB protected_probe{};
  if (!CryptProtectData(&probe, L"ReDesk TURN recovery", nullptr, nullptr,
                        nullptr, CRYPTPROTECT_UI_FORBIDDEN, &protected_probe))
    return {false, "Cannot protect TURN recovery credentials with Windows DPAPI"};
  LocalFree(protected_probe.pbData);

  std::string uid = pending_uid;
  std::string secret = pending_secret;
  if (!matched_pending_key) {
    Json key;
    auto created = Decode(client.Post(turn_path, Authorization(token),
        Json{{"name", "redesk-signal"}}.dump(), "application/json"), &key);
    if (!created.ok) return {false, "Worker deployed but TURN creation failed: " + created.error};
    uid = key.value("uid", "");
    secret = key.value("key", key.value("secret", ""));
    if (!IsHex32(uid) || secret.size() != 64)
      return {false, "TURN key returned unexpected metadata. No replacement key will be created."};
    if (!SavePendingKey(account, uid, secret))
      return {false, "TURN key created but encrypted recovery could not be stored. "
                     "Do not retry key creation; inspect the Cloudflare account."};
  }
  auto secrets = AddTurnSecrets(token, account, uid, secret);
  if (!secrets.ok)
    return {false, "Worker and TURN key created, but secrets installation failed: " +
                   secrets.error + ". Encrypted recovery remains on this PC."};
#endif

  if (progress) progress("Verifying Backend", "Running");
  BackendStatus health;
  for (int attempt = 0; attempt < 6; ++attempt) {
    health = CheckHealth(base_url);
    if (health.redesk_health && health.turn_configured) break;
    if (attempt < 5) std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  if (!health.redesk_health || !health.turn_configured)
    return {false, "Worker deployed and TURN secrets were submitted, but "
                   "verification is incomplete: " + health.diagnostic +
                   (health.redesk_health ? " (TURN not yet reported configured)." : ".") +
                   " The encrypted TURN recovery is retained. Do not reset "
                   "the account or create another key; check the Worker URL "
                   "and retry Resume Pending TURN Setup."};
  if (worker_url) *worker_url = base_url;
  ClearPendingKey(account);
  if (progress) progress("Verifying Backend", "Completed");
  return {true, {}};
}

Result CloudflareProvisioningService::AddTurnSecrets(
    const std::string& token, const std::string& account,
    const std::string& key_id, const std::string& secret) const {
  // Cloudflare TURN bearer tokens are 64 bytes, not necessarily hex.
  if (!IsHex32(account) || !IsHex32(key_id) || secret.size() != 64 ||
      !std::all_of(secret.begin(), secret.end(), [](unsigned char c) {
        return c >= 33 && c <= 126;
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
