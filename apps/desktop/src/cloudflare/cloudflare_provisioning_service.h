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
  std::string url;
  std::string diagnostic;
};
struct Result {
  bool ok = false;
  std::string error;
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
  Result ValidateToken(const std::string& api_token) const;
  std::vector<Account> ListAccounts(const std::string& api_token,
                                    std::string* error) const;
  BackendStatus Inspect(const std::string& api_token,
                        const std::string& account_id,
                        const std::string& worker_url = {}) const;
  BackendStatus CheckHealth(const std::string& worker_url) const;
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
