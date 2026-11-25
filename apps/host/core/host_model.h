#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace redesk::host {

enum class ServerKind { Cloudflare, CrossDesk, SelfHosted };
enum class PasswordMode { Fixed, Temporary };
enum class ViewerPolicy { SingleViewer, SingleController, MultipleControllers };

// No password or token is stored in this model. The platform credential store
// owns secrets, indexed by the stable profile key. Advance credential_revision
// whenever credentials are replaced, including temporary password rotation.
struct ServerProfile {
    std::string key;
    std::string label;
    ServerKind kind = ServerKind::Cloudflare;
    std::string endpoint;
    std::string device_id;
    PasswordMode password_mode = PasswordMode::Fixed;
    std::uint64_t credential_revision = 0;
    bool enabled = false;
    bool require_local_confirmation = true;
};

enum class Change { Start, Stop, Reconnect, UpdatePolicy };
struct ProfileChange {
    std::string key;
    Change action;
};

inline void validate_profiles(const std::vector<ServerProfile>& profiles) {
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const auto& p = profiles[i];
        if (p.key.empty() || p.endpoint.empty())
            throw std::invalid_argument("profile key and endpoint are required");
        for (std::size_t j = 0; j < i; ++j)
            if (profiles[j].key == p.key)
                throw std::invalid_argument("duplicate profile key");
    }
}

inline bool same_transport(const ServerProfile& a, const ServerProfile& b) {
    return a.kind == b.kind && a.endpoint == b.endpoint &&
        a.device_id == b.device_id && a.password_mode == b.password_mode &&
        a.credential_revision == b.credential_revision;
}

// Pure reconciliation plan: the runtime owns transactional persistence,
// credentials, authentication and worker lifetime. Unchanged workers are left
// alone; a label-only edit does not change the transport. Endpoint validation
// belongs to the real protocol adapter before applying a plan.
inline std::vector<ProfileChange> plan_changes(
        const std::vector<ServerProfile>& before,
        const std::vector<ServerProfile>& after) {
    validate_profiles(before);
    validate_profiles(after);
    std::vector<ProfileChange> changes;
    auto find = [](const auto& profiles, const std::string& key) {
        return std::find_if(profiles.begin(), profiles.end(),
            [&](const auto& p) { return p.key == key; });
    };
    for (const auto& old : before) {
        if (!old.enabled) continue;
        const auto next = find(after, old.key);
        if (next == after.end() || !next->enabled)
            changes.push_back({old.key, Change::Stop});
        else if (!same_transport(old, *next))
            changes.push_back({old.key, Change::Reconnect});
        else if (old.require_local_confirmation != next->require_local_confirmation)
            changes.push_back({old.key, Change::UpdatePolicy});
    }
    for (const auto& next : after) {
        if (!next.enabled) continue;
        const auto old = find(before, next.key);
        if (old == before.end() || !old->enabled)
            changes.push_back({next.key, Change::Start});
    }
    return changes;
}

struct SessionKey {
    std::string profile_key;
    std::string session_id;
    bool operator==(const SessionKey& other) const {
        return profile_key == other.profile_key && session_id == other.session_id;
    }
};

enum class Admission { Rejected, ViewOnly, Control };

// Global policy across all servers and LAN. Call only from a serialized owner
// after the protocol adapter has authenticated and authorized the peer.
// ViewOnly is a capability ceiling, not authentication: the runtime must enforce
// it on every input, clipboard, file-transfer and restart operation.
class SessionRegistry {
public:
    explicit SessionRegistry(ViewerPolicy policy = ViewerPolicy::SingleViewer)
        : policy_(policy) {}

    ViewerPolicy policy() const noexcept { return policy_; }
    std::size_t size() const noexcept { return sessions_.size(); }

    // Reject an incompatible tightening without disrupting existing sessions.
    // A caller may close sessions and retry. No implicit control promotion.
    bool set_policy(ViewerPolicy next) {
        if (next == ViewerPolicy::SingleViewer && sessions_.size() > 1) return false;
        if (next != ViewerPolicy::MultipleControllers && controller_count() > 1) return false;
        policy_ = next;
        return true;
    }

    Admission admit(const SessionKey& key, bool requested_control) {
        if (key.profile_key.empty() || key.session_id.empty() ||
            lookup(key) != sessions_.end()) return Admission::Rejected;
        if (policy_ == ViewerPolicy::SingleViewer && !sessions_.empty())
            return Admission::Rejected;
        const bool control = requested_control &&
            (policy_ == ViewerPolicy::MultipleControllers || controller_count() == 0);
        sessions_.push_back({key, control});
        return control ? Admission::Control : Admission::ViewOnly;
    }

    bool can_control(const SessionKey& key) const {
        const auto it = lookup(key);
        return it != sessions_.end() && it->control;
    }

    void close(const SessionKey& key) {
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
            [&](const auto& s) { return s.key == key; }), sessions_.end());
    }

    void close_profile(const std::string& key) {
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
            [&](const auto& s) { return s.key.profile_key == key; }), sessions_.end());
    }

private:
    struct Session { SessionKey key; bool control; };
    using Iterator = std::vector<Session>::const_iterator;
    Iterator lookup(const SessionKey& key) const {
        return std::find_if(sessions_.begin(), sessions_.end(),
            [&](const auto& s) { return s.key == key; });
    }
    std::size_t controller_count() const {
        return static_cast<std::size_t>(std::count_if(sessions_.begin(), sessions_.end(),
            [](const auto& s) { return s.control; }));
    }
    ViewerPolicy policy_;
    std::vector<Session> sessions_;
};

} // namespace redesk::host
