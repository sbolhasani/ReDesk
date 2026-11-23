#include "cloudflare/cloudflare_provisioning_service.h"

#include <cassert>
#include <string>

int main() {
  using crossdesk::cloudflare::CloudflareProvisioningService;
  using crossdesk::cloudflare::SetupOptions;
  using crossdesk::cloudflare::ExistingAction;
  const auto& service = CloudflareProvisioningService{};
  std::string base, signal;

  assert(CloudflareProvisioningService::NormalizeWorkerUrl(
      "https://redesk-signal.example.workers.dev/signal", &base, &signal));
  assert(base == "https://redesk-signal.example.workers.dev");
  assert(signal == "redesk-signal.example.workers.dev/signal");

  for (const auto& invalid : {
       "http://example.workers.dev", "https://example.workers.dev:80",
       "https://user@example.workers.dev", "https://example.workers.dev/other",
       "https://example.workers.dev?token=abc",
       "https://example.workers.dev#fragment", "https://example.workers.dev\r\n",
       "https://.example.workers.dev", "https://example.workers.dev."}) {
    assert(!CloudflareProvisioningService::NormalizeWorkerUrl(
        invalid, &base, &signal));
  }

  assert(CloudflareProvisioningService::ValidAccountId(
      "0123456789abcdef0123456789abcdef"));
  assert(!CloudflareProvisioningService::ValidAccountId(
      "not-a-cloudflare-account"));

  // These safety paths must not issue any HTTP requests.
  SetupOptions unsafe;
  unsafe.account_id = "0123456789abcdef0123456789abcdef";
  unsafe.existing_action = ExistingAction::ReinitializeData;
  const auto blocked = service.Deploy("placeholder-token", unsafe, "worker-script");
  assert(!blocked.ok);
  assert(blocked.error.find("not supported") != std::string::npos);

  SetupOptions invalid_account;
  invalid_account.account_id = "bad";
  const auto no_request = service.Deploy("placeholder-token", invalid_account, "worker-script");
  assert(!no_request.ok);
  assert(service.Inspect("placeholder-token", "bad").worker_exists == false);

  // A user-provided URL alone never creates or alters a backend.
  SetupOptions existing;
  existing.existing_action = ExistingAction::UpdateCode;
  const auto denied = service.ConfigureExisting(existing, &signal);
  assert(!denied.ok);

  return 0;
}
