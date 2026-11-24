#include "cloudflare_provisioning_service.h"
#include "bundled_worker.h"

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
  if (!response || response->status != 200) {
    status.diagnostic = "The Worker health endpoint is unavailable";
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
  auto refreshed = WithHealthStatus(previous, CheckHealth(previous.worker_url));
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
        "ReDesk /health is still unavailable or incompatible. "
        "Use Existing and Update remain disabled. You may retry or go back "
        "to correct the Worker URL.";
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
      discovery.message =
          "This Cloudflare account has no workers.dev subdomain. "
          "Enter your preferred new Worker URL in the format "
          "https://redesk-signal.YOUR-SUBDOMAIN.workers.dev. "
          "ReDesk will request that account subdomain only after you confirm Install.";
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
  if (worker.worker_exists) {
    review = WithHealthStatus(review, CheckHealth(review.worker_url));
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
  review.ok = true;
  if (review.redesk_health) {
    review.explanation = review.update_safe
        ? "Existing Worker is healthy. You may use or update it after confirmation."
        : "Existing Worker is healthy. Use Existing is available.";
    if (!review.worker_version.empty())
      review.explanation += " Version: " + review.worker_version + ".";
  } else if (review.worker_exists) {
    review.explanation = "Worker exists but /health is not compatible. No installation or overwrite is permitted.";
  } else if (!review.turn_keys_checked) {
    review.explanation = "No Worker was found, but TURN keys cannot be listed. "
                         "Installation is blocked until the token can inspect them.";
  } else if (review.existing_turn_key) {
    review.explanation = "No Worker was found, but a ReDesk TURN key already exists. "
                         "Installation is blocked to prevent credential loss.";
  } else {
    review.explanation = "No Worker or ReDesk TURN key found. "
                         "Ready to install a new Worker, Durable Object and TURN.";
  }
  return review;
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
    metadata["migrations"] = Json::array({
      Json{{"new_tag", "v1"}, {"new_sqlite_classes", Json::array({"SignalHub"})}}
    });
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
  if (progress) progress("Validating Cloudflare Account", "Running");
  auto validity = ValidateToken(token, account);
  if (!validity.ok) return validity;

  if (progress) progress("Checking Existing Worker", "Running");
  auto prior = Inspect(token, account);
  if (prior.worker_exists)
    return {false, "Worker already exists. Use Existing Backend or confirm an update."};
  if (prior.diagnostic != "Worker does not exist")
    return {false, "Cannot safely determine whether Worker exists: " + prior.diagnostic};

  auto client = Client(kApi);
  if (progress) progress("Configuring TURN", "Running");
  // Cloudflare cannot reveal existing TURN secrets. Do not generate duplicates
  // or rotate keys that other installations may already be using.
  Json keys;
  const auto turn_path = "/client/v4/accounts/" + account + "/calls/turn_keys";
  auto listed = Decode(client.Get(turn_path, Authorization(token)), &keys);
  if (!listed.ok) return {false, "Could not inspect existing TURN keys: " + listed.error};
  if (!keys.is_array()) return {false, "Unexpected TURN key list"};
  for (const auto& key : keys) {
    if (key.is_object() && key.value("name", "") == "redesk-signal")
      return {false, "Existing ReDesk TURN key found: cannot recover secret or safely rotate it"};
  }

  Json subdomain;
  auto resolved = Decode(client.Get("/client/v4/accounts/" + account +
                                    "/workers/subdomain", Authorization(token)),
                         &subdomain);
  if (!resolved.ok) return {false, "Could not resolve Workers subdomain: " + resolved.error};
  const std::string name = subdomain.is_object()
      ? subdomain.value("subdomain", "") : "";
  std::string base_url;
  const std::string url = "https://redesk-signal." + name + ".workers.dev";
  if (!NormalizeWorkerUrl(url, &base_url, nullptr))
    return {false, "Cloudflare account has no valid workers.dev subdomain"};

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
  Json key;
  auto created = Decode(client.Post(turn_path, Authorization(token),
      Json{{"name", "redesk-signal"}}.dump(), "application/json"), &key);
  if (!created.ok) return {false, "Worker deployed but TURN creation failed: " + created.error};
  const auto uid = key.value("uid", "");
  const auto secret = key.value("key", key.value("secret", ""));
  auto secrets = AddTurnSecrets(token, account, uid, secret);
  if (!secrets.ok)
    return {false, "Worker deployed and TURN key created, but secret installation failed: " +
                   secrets.error + ". Check Cloudflare before retrying."};

  if (progress) progress("Verifying Backend", "Running");
  auto health = CheckHealth(base_url);
  if (!health.redesk_health || !health.turn_configured)
    return {false, "Worker deployed but TURN/health verification failed; check Cloudflare state"};
  if (worker_url) *worker_url = base_url;
  if (progress) progress("Verifying Backend", "Completed");
  return {true, {}};
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
