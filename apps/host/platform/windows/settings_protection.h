#pragma once
#include "host_settings.h"

namespace redesk::host::windows {
// User scope for portable storage. Machine scope is permitted only for
// an installed store with a verified SYSTEM/Administrators-only ACL.
// The caller must implement that filesystem boundary before persisting.
enum class ProtectionScope { CurrentUser, LocalMachine };
std::string protect_settings(const HostSettings&, ProtectionScope);
HostSettings unprotect_settings(std::string_view ciphertext);
}
