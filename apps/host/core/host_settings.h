#pragma once
#include "host_model.h"
#include <limits>
#include <string_view>
#include <utility>

namespace redesk::host {

// One complete snapshot. The identity payload is opaque to this layer:
// the protocol adapter must validate it before attempting a login.
// Plaintext representations must never be persisted or logged.
struct StoredProfile {
    ServerProfile profile;
    std::string password;
    std::string identity;
};
struct HostSettings {
    ViewerPolicy viewer_policy = ViewerPolicy::SingleViewer;
    std::vector<StoredProfile> servers;
};

namespace snapshot {
constexpr std::size_t max_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_field = 64 * 1024;
inline void require(bool ok) {
    if (!ok) throw std::invalid_argument("Invalid host settings snapshot");
}
inline void validate(const HostSettings& settings) {
    require(settings.viewer_policy == ViewerPolicy::SingleViewer ||
            settings.viewer_policy == ViewerPolicy::SingleController ||
            settings.viewer_policy == ViewerPolicy::MultipleControllers);
    std::vector<ServerProfile> profiles;
    for (const auto& entry : settings.servers) {
        const auto& p = entry.profile;
        require(p.key != "@lan" && p.key.size() <= 128 &&
                p.label.size() <= 512 && p.endpoint.size() <= 4096 &&
                p.device_id.size() <= 128 &&
                entry.password.size() <= 4096 && entry.identity.size() <= max_field);
        require(p.kind == ServerKind::Cloudflare || p.kind == ServerKind::CrossDesk ||
                p.kind == ServerKind::SelfHosted);
        require(p.password_mode == PasswordMode::Fixed || p.password_mode == PasswordMode::Temporary);
        profiles.push_back(p);
    }
    validate_profiles(profiles);
}
class Writer {
public:
    void number(std::uint64_t v, unsigned bytes) {
        require(bytes <= max_bytes - out.size());
        for (unsigned i = 0; i < bytes; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 255));
    }
    void field(std::string_view v) {
        require(v.size() <= max_field);
        number(v.size(), 4);
        require(v.size() <= max_bytes - out.size());
        out.append(v);
    }
    std::string out = "RDHS";
};
class Reader {
public:
    explicit Reader(std::string_view data) : input(data) {
        require(data.size() >= 4 && data.size() <= max_bytes && data.substr(0, 4) == "RDHS");
    }
    std::uint64_t number(unsigned bytes) {
        require(bytes <= remaining());
        std::uint64_t v = 0;
        for (unsigned i = 0; i < bytes; ++i)
            v |= std::uint64_t(static_cast<unsigned char>(input[pos++])) << (8 * i);
        return v;
    }
    bool flag() { const auto v = number(1); require(v <= 1); return v != 0; }
    std::string field() {
        const auto n = number(4);
        require(n <= max_field && n <= remaining());
        auto value = std::string(input.substr(pos, static_cast<std::size_t>(n)));
        pos += static_cast<std::size_t>(n);
        return value;
    }
    std::size_t remaining() const { return input.size() - pos; }
private:
    std::string_view input;
    std::size_t pos = 4;
};

// Versioned, length-delimited, endian-independent internal representation.
// DPAPI protects the entire byte sequence, including identity and preferences.
inline std::string encode(const HostSettings& settings) {
    validate(settings);
    Writer w;
    w.number(1, 2);
    w.number(static_cast<unsigned>(settings.viewer_policy), 1);
    require(settings.servers.size() <= max_bytes / 37);
    w.number(settings.servers.size(), 4);
    for (const auto& entry : settings.servers) {
        const auto& p = entry.profile;
        w.field(p.key); w.field(p.label); w.field(p.endpoint); w.field(p.device_id);
        w.number(static_cast<unsigned>(p.kind), 1);
        w.number(static_cast<unsigned>(p.password_mode), 1);
        w.number(p.credential_revision, 8);
        w.number(p.enabled, 1);
        w.number(p.require_local_confirmation, 1);
        w.field(entry.password); w.field(entry.identity);
    }
    return std::move(w.out);
}
inline HostSettings decode(std::string_view bytes) {
    Reader r(bytes);
    require(r.number(2) == 1);
    HostSettings settings;
    const auto policy = r.number(1);
    require(policy <= 2);
    settings.viewer_policy = static_cast<ViewerPolicy>(policy);
    const auto count = r.number(4);
    // Six length prefixes, 12 fixed bytes and nonempty key/endpoint.
    require(count <= r.remaining() / 38);
    for (std::uint64_t i = 0; i < count; ++i) {
        StoredProfile entry;
        auto& p = entry.profile;
        p.key = r.field(); p.label = r.field(); p.endpoint = r.field(); p.device_id = r.field();
        const auto kind = r.number(1), mode = r.number(1);
        require(kind <= 2 && mode <= 1);
        p.kind = static_cast<ServerKind>(kind);
        p.password_mode = static_cast<PasswordMode>(mode);
        p.credential_revision = r.number(8);
        p.enabled = r.flag(); p.require_local_confirmation = r.flag();
        entry.password = r.field(); entry.identity = r.field();
        settings.servers.push_back(std::move(entry));
    }
    require(r.remaining() == 0);
    validate(settings);
    return settings;
}
} // namespace snapshot
} // namespace redesk::host
