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
  const std::string account = "0123456789abcdef0123456789abcdef";
  Require(CloudflareProvisioningService::TokenVerifyPath(
      "cfat_sample", account) ==
      "/client/v4/accounts/" + account + "/tokens/verify",
      "account-owned token uses account-scoped verification");
  Require(CloudflareProvisioningService::TokenVerifyPath("cfat_sample").empty(),
      "account-owned tokens require an Account ID");
  Require(CloudflareProvisioningService::TokenVerifyPath("cfut_sample", account) ==
      "/client/v4/user/tokens/verify",
      "user token uses user-scoped verification");
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
  // A failed health check cannot unlock either action.
  crossdesk::cloudflare::BackendReview review;
  review.ok = true;
  review.worker_exists = true;
  review.worker_bindings_verified = true;
  review.worker_url = "https://redesk-signal.example.workers.dev";
  crossdesk::cloudflare::BackendStatus unhealthy;
  auto failed_retry = CloudflareProvisioningService::WithHealthStatus(
      review, unhealthy);
  Require(!failed_retry.redesk_health && !failed_retry.update_safe,
          "failed health check must not unlock update");

  // A healthy public endpoint allows Use Existing after a retry. Updating
  // requires prior account-level binding evidence, not /health alone.
  crossdesk::cloudflare::BackendStatus healthy;
  healthy.redesk_health = true;
  healthy.turn_configured = true;
  auto recovered = CloudflareProvisioningService::WithHealthStatus(
      review, healthy);
  Require(recovered.redesk_health && recovered.update_safe,
          "healthy retry restores update only with prior binding evidence");
  Require(recovered.turn_configured && recovered.worker_url == review.worker_url,
          "health retry retains the selected Worker and updates TURN status");
  review.worker_bindings_verified = false;
  review.worker_exists = false;
  auto restricted = CloudflareProvisioningService::WithHealthStatus(
      review, healthy);
  Require(restricted.worker_exists && restricted.redesk_health,
          "healthy retry permits use when metadata API is restricted");
  Require(!restricted.update_safe,
          "healthy endpoint alone must never authorize Worker code overwrite");
  auto failed_again = CloudflareProvisioningService::WithHealthStatus(
      recovered, unhealthy);
  Require(!failed_again.update_safe && !failed_again.redesk_health,
          "subsequent failed health check disables both actions again");
  
  std::cout << "Cloudflare safety tests passed." << std::endl;
  return 0;
}
