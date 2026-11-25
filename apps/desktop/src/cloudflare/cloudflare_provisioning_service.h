#ifndef REDESK_CLOUDFLARE_PROVISIONING_SERVICE_H_
#define REDESK_CLOUDFLARE_PROVISIONING_SERVICE_H_

#include <functional>
#include <string>
#include <vector>

namespace crossdesk::cloudflare {

// No API token is stored here. The caller owns the token for one operation.
struct Account {
  std::string id;
  std::string name;
};
struct BackendStatus {
  bool worker_exists = false;
  bool redesk_health = false;
  bool durable_object_binding = false;
  bool turn_configured = false;
  std::string worker_version; // empty means legacy/unknown, not "0.0.0"
  bool turn_key_id_binding = false;
  bool turn_api_token_binding = false;
  bool unexpected_bindings = false;
  std::string url;
  std::string diagnostic;
};
struct Result {
  bool ok = false;
  std::string error;
};
struct BackendReview {
  bool ok = false;
  bool worker_exists = false;
  bool durable_object_binding = false;
  bool redesk_health = false;
  bool turn_configured = false;
  std::string worker_version;
  bool existing_turn_key = false;
  bool pending_turn_recovery = false;
  bool turn_keys_checked = false;
  bool update_safe = false;
  // Set only after the account API verifies the expected SignalHub binding.
  // RetryHealth may enable code updates only with this prior evidence.
  bool worker_bindings_verified = false;
  std::string account_id;
  std::string worker_url;
  std::string explanation;
};
// Read-only guided discovery: additional fields are requested only when the
// relevant Cloudflare API cannot supply that information.
struct BackendDiscovery {
  bool token_verified = false;
  bool needs_account_id = false;
  bool needs_worker_url = false;
  bool review_ready = false;
  std::string account_id;
  // A proposed URL only; Cloudflare does not reserve the subdomain until Install.
  std::string suggested_worker_url;
  std::string message;
  BackendReview review;
};
enum class ExistingAction { UseExisting, UpdateCode, ReinitializeData };
struct SetupOptions {
  std::string account_id;
  std::string worker_url;
  ExistingAction existing_action = ExistingAction::UseExisting;
  bool confirmed_worker_update = false;
  // Data reset is never performed by this service: the official, verified
  // reset path must be implemented separately to prevent silent data loss.
};
using Progress = std::function<void(const char* step, const char* state)>;

class CloudflareProvisioningService {
 public:
  // All calls are synchronous and may block. Invoke on a background thread.
  // Account-owned tokens (cfat_) require account-scoped verification.
  // User tokens use the user endpoint. Legacy unprefixed tokens may be tried
  // against both endpoints when the Account ID is known.
  Result ValidateToken(const std::string& api_token,
                       const std::string& account_id = {}) const;
  static std::string TokenVerifyPath(const std::string& api_token,
                                     const std::string& account_id = {});
  std::vector<Account> ListAccounts(const std::string& api_token,
                                    std::string* error) const;
  BackendStatus Inspect(const std::string& api_token,
                        const std::string& account_id,
                        const std::string& worker_url = {}) const;
  BackendStatus CheckHealth(const std::string& worker_url) const;
  BackendReview ReviewAccount(const std::string& api_token,
                              const std::string& account_id,
                              const std::string& worker_url = {}) const;
  // Rechecks ONLY the public /health endpoint, without API token or writes.
  // Does not re-enumerate TURN keys or change Durable Object data.
  BackendReview RetryHealth(const BackendReview& previous) const;
  // Pure safety policy, used by network-free tests.
  static BackendReview WithHealthStatus(const BackendReview& previous,
                                        const BackendStatus& health);
  BackendDiscovery DiscoverBackend(const std::string& api_token,
                                   const std::string& account_id = {},
                                   const std::string& worker_url = {}) const;
  static bool MayUpdateWorker(const BackendStatus& status);
  // Strict semantic version comparison; legacy workers report no version.
  // They may be manually updated only if Worker bindings were verified.
  static bool IsUpdateAvailable(const std::string& deployed_version);
  // Local DPAPI recovery status only; never exposes the secret to the UI.
  static bool HasPendingTurnRecovery(const std::string& account_id);
  // Explicit, account-scoped reconciliation. No Cloudflare resource is
  // deleted. Removes only an orphaned local encrypted TURN recovery file
  // after independently verifying that Worker and ReDesk TURN keys are absent.
  Result DiscardOrphanTurnRecovery(const std::string& api_token,
                                  const std::string& account_id) const;
  Result ConfigureExisting(const SetupOptions& options,
                           std::string* signal_address) const;
  // Fresh deployment only. Existing Worker/TURN state always requires a
  // separate explicit reconciliation; this never deletes or rotates keys.
  Result ProvisionNew(const std::string& api_token,
                      const std::string& account_id,
                      const std::string& worker_script,
                      std::string* worker_url, Progress progress = {}) const;
  Result Deploy(const std::string& api_token, const SetupOptions& options,
                const std::string& worker_script, Progress progress = {}) const;
  // Only uses a separately authorized action. Never deletes existing keys.
  Result AddTurnSecrets(const std::string& api_token,
                        const std::string& account_id,
                        const std::string& key_id,
                        const std::string& key_secret) const;
  static bool ValidAccountId(const std::string& value);
  static bool NormalizeWorkerUrl(const std::string& value,
                                 std::string* base_url,
                                 std::string* signal_address);
};

} // namespace crossdesk::cloudflare
#endif
