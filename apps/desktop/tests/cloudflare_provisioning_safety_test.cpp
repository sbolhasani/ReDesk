#include "cloudflare/cloudflare_provisioning_service.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Cloudflare safety test FAILED: " << message << std::endl;
    std::exit(1);
  }
}
}

int main() {
  using crossdesk::cloudflare::CloudflareProvisioningService;
  using crossdesk::cloudflare::SetupOptions;
  using crossdesk::cloudflare::ExistingAction;
  const auto& service = CloudflareProvisioningService{};
  std::string base, signal;

  Require(CloudflareProvisioningService::NormalizeWorkerUrl(
      "https://redesk-signal.example.workers.dev/signal", &base, &signal), "check 1");
  Require(base == "https://redesk-signal.example.workers.dev", "check 2");
  Require(signal == "redesk-signal.example.workers.dev/signal", "check 3");

  for (const auto& invalid : {
       "http://example.workers.dev", "https://example.workers.dev:80",
       "https://user@example.workers.dev", "https://example.workers.dev/other",
       "https://example.workers.dev?token=abc",
       "https://example.workers.dev#fragment", "https://example.workers.dev\r\n",
       "https://.example.workers.dev", "https://example.workers.dev."}) {
    Require(!CloudflareProvisioningService::NormalizeWorkerUrl(
        invalid, &base, &signal), "check 4");
  }

  Require(CloudflareProvisioningService::ValidAccountId(
      "0123456789abcdef0123456789abcdef"), "check 5");
  Require(!CloudflareProvisioningService::ValidAccountId(
      "not-a-cloudflare-account"), "check 6");

  // These safety paths must not issue any HTTP requests.
  SetupOptions unsafe;
  unsafe.account_id = "0123456789abcdef0123456789abcdef";
  unsafe.existing_action = ExistingAction::ReinitializeData;
  const auto blocked = service.Deploy("placeholder-token", unsafe, "worker-script");
  Require(!blocked.ok, "check 7");
  Require(blocked.error.find("not supported") != std::string::npos, "check 8");

  SetupOptions invalid_account;
  invalid_account.account_id = "bad";
  const auto no_request = service.Deploy("placeholder-token", invalid_account, "worker-script");
  Require(!no_request.ok, "check 9");
  Require(service.Inspect("placeholder-token", "bad").worker_exists == false, "check 10");

  // A user-provided URL alone never creates or alters a backend.
  SetupOptions existing;
  existing.existing_action = ExistingAction::UpdateCode;
  const auto denied = service.ConfigureExisting(existing, &signal);
  Require(!denied.ok, "check 11");

  crossdesk::cloudflare::BackendStatus safe;
  safe.worker_exists = true;
  safe.redesk_health = true;
  safe.durable_object_binding = true;
  Require(CloudflareProvisioningService::MayUpdateWorker(safe),
          "compatible Worker can be updated with confirmation");
  safe.unexpected_bindings = true;
  Require(!CloudflareProvisioningService::MayUpdateWorker(safe),
          "unknown bindings block update");
  safe.unexpected_bindings = false;
  safe.redesk_health = false;
  Require(!CloudflareProvisioningService::MayUpdateWorker(safe),
          "unknown worker identity blocks update");
  std::cout << "Cloudflare safety tests passed." << std::endl;
  return 0;
}
