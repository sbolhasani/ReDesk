#pragma once
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace redesk::host {
// The serialized 32-byte seed is hex encoded inside the protected settings
// snapshot. Generate and durably save it before any first registration.
// Never silently regenerate a malformed or missing identity for an existing ID.
class DeviceKey {
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    static void require(bool ok) {
        if (!ok) throw std::runtime_error("Host device signing key unavailable");
    }
    static std::string hex(const unsigned char* data, std::size_t n) {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(n * 2);
        for (std::size_t i = 0; i < n; ++i) {
            result += digits[data[i] >> 4]; result += digits[data[i] & 15];
        }
        return result;
    }
    static unsigned digit(char c) {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        throw std::runtime_error("Invalid host signing key encoding");
    }
    struct Seed {
        std::array<unsigned char, 32> bytes{};
        ~Seed() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
    };
    Key key_{nullptr, EVP_PKEY_free};
    std::string public_key_;
    explicit DeviceKey(Key key) : key_(std::move(key)) {
        require(bool(key_));
        std::array<unsigned char,32> data{};
        std::size_t n = data.size();
        require(EVP_PKEY_get_raw_public_key(key_.get(), data.data(), &n) == 1 && n == data.size());
        public_key_ = hex(data.data(), n);
    }
public:
    static DeviceKey generate() {
        return DeviceKey(Key(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free));
    }
    static DeviceKey restore(std::string_view seed) {
        require(seed.size() == 64);
        Seed raw;
        for (std::size_t i = 0; i < raw.bytes.size(); ++i)
            raw.bytes[i] = static_cast<unsigned char>((digit(seed[2*i]) << 4) | digit(seed[2*i+1]));
        return DeviceKey(Key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                       raw.bytes.data(), raw.bytes.size()), EVP_PKEY_free));
    }
    std::string export_seed() const {
        Seed raw;
        auto n = raw.bytes.size();
        require(EVP_PKEY_get_raw_private_key(key_.get(), raw.bytes.data(), &n) == 1 && n == raw.bytes.size());
        return hex(raw.bytes.data(), n);
    }
    const std::string& PublicKey() const noexcept { return public_key_; }
    std::string Sign(std::string_view value) const {
        Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        require(bool(context) && EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key_.get()) == 1);
        std::array<unsigned char,64> signature{};
        auto n = signature.size();
        require(EVP_DigestSign(context.get(), signature.data(), &n,
            reinterpret_cast<const unsigned char*>(value.data()), value.size()) == 1 && n == signature.size());
        return hex(signature.data(), n);
    }
};
}
