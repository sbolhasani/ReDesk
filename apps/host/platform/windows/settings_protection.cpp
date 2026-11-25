#include "settings_protection.h"
#include <windows.h>
#include <dpapi.h>
#include <memory>

namespace redesk::host::windows {
namespace {
struct WipeText {
    std::string& value;
    ~WipeText() { if (!value.empty()) SecureZeroMemory(value.data(), value.size()); }
};
struct Blob {
    DATA_BLOB data{};
    ~Blob() {
        if (data.pbData) {
            SecureZeroMemory(data.pbData, data.cbData);
            LocalFree(data.pbData);
        }
    }
    Blob() = default;
    Blob(const Blob&) = delete;
    Blob& operator=(const Blob&) = delete;
};
DATA_BLOB bytes(std::string_view v) {
    return {static_cast<DWORD>(v.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(v.data()))};
}
// Domain separation from ReDesk's existing credential store.
DATA_BLOB entropy() {
    return bytes("ReDeskHost/settings/v1");
}
constexpr std::size_t max_ciphertext = snapshot::max_bytes + 65536;
}
std::string protect_settings(const HostSettings& settings, ProtectionScope scope) {
    auto plaintext = snapshot::encode(settings);
    WipeText wipe{plaintext};
    auto input = bytes(plaintext);
    auto extra = entropy();
    Blob output;
    DWORD flags = CRYPTPROTECT_UI_FORBIDDEN;
    if (scope == ProtectionScope::LocalMachine) flags |= CRYPTPROTECT_LOCAL_MACHINE;
    else if (scope != ProtectionScope::CurrentUser) throw std::invalid_argument("Invalid protection scope");
    if (!CryptProtectData(&input, L"ReDesk Host settings", &extra, nullptr,
                          nullptr, flags, &output.data))
        throw std::runtime_error("Cannot encrypt host settings");
    if (output.data.cbData > max_ciphertext)
        throw std::runtime_error("Encrypted host settings exceed size limit");
    return std::string(reinterpret_cast<const char*>(output.data.pbData), output.data.cbData);
}
HostSettings unprotect_settings(std::string_view ciphertext) {
    if (ciphertext.empty() || ciphertext.size() > max_ciphertext)
        throw std::invalid_argument("Invalid encrypted settings size");
    auto input = bytes(ciphertext);
    auto extra = entropy();
    Blob output;
    if (!CryptUnprotectData(&input, nullptr, &extra, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output.data))
        throw std::runtime_error("Cannot decrypt host settings");
    return snapshot::decode(std::string_view(
        reinterpret_cast<const char*>(output.data.pbData), output.data.cbData));
}
}
