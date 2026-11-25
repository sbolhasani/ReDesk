#include "runtime/device_login.h"
#include "runtime/trusted_devices.h"

namespace crossdesk {
int DeviceLogin::Exchange(MiniRtcSignalExchangeStage stage, const nlohmann::json& message,
    const TrustedDevices* identity, char* response, size_t capacity) {
  return ExchangeWithSigner(stage, message, identity ? identity->PublicKey() : std::string{},
      identity ? Signer([identity](std::string_view value) { return identity->Sign(value); }) : Signer{},
      response, capacity);
}
}  // namespace crossdesk
