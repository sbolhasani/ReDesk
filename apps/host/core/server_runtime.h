#pragma once
#include "host_model.h"
#include <chrono>
#include <map>
#include <utility>

namespace redesk::host {

// Called by one serialized host event loop. Network adapters post events to
// that loop; they never call this class from a MiniRTC callback thread.
class ServerRuntime {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    enum class State { Connecting, Online, WaitingRetry };
    enum class Operation { Start, Stop, UpdatePolicy };
    struct Command {
        Operation operation;
        ServerProfile profile;
        std::uint64_t generation;
    };
    struct Status {
        ServerProfile profile;
        State state;
        std::uint64_t generation;
        unsigned failures = 0;
        Time retry_at{};
    };

    // LAN uses a reserved namespace, not a user-controlled server key.
    static constexpr const char* LanKey = "@lan";

    std::vector<Command> apply(std::vector<ServerProfile> next) {
        validate_profiles(next);
        for (const auto& p : next)
            if (p.key == LanKey) throw std::invalid_argument("reserved profile key");
        const auto changes = plan_changes(profiles_, next);
        std::vector<Command> commands;
        for (const auto& change : changes) {
            auto it = workers_.find(change.key);
            if (change.action == Change::Stop || change.action == Change::Reconnect) {
                if (it != workers_.end()) {
                    if (it->second.state != State::WaitingRetry)
                        commands.push_back({Operation::Stop, it->second.profile, it->second.generation});
                    sessions_.close_profile(change.key);
                    workers_.erase(it);
                }
            }
            if (change.action == Change::Start || change.action == Change::Reconnect) {
                const auto& p = *std::find_if(next.begin(), next.end(), [&](const auto& v) { return v.key == change.key; });
                auto& worker = workers_.emplace(p.key, Status{p, State::Connecting, ++generation_}).first->second;
                commands.push_back({Operation::Start, p, worker.generation});
            } else if (change.action == Change::UpdatePolicy && it != workers_.end() &&
                       it->second.state != State::WaitingRetry) {
                const auto& p = *std::find_if(next.begin(), next.end(), [&](const auto& v) { return v.key == change.key; });
                commands.push_back({Operation::UpdatePolicy, p, it->second.generation});
            }
        }
        for (const auto& p : next) {
            auto it = workers_.find(p.key);
            if (it != workers_.end()) it->second.profile = p;
        }
        profiles_ = std::move(next);
        return commands;
    }

    // A generation identifies one transport lifetime. Stop must quiesce its
    // callbacks before destruction; stale events are rejected here as well.
    bool online(const std::string& key, std::uint64_t generation) {
        auto* w = current(key, generation);
        if (!w || w->state != State::Connecting) return false;
        w->state = State::Online;
        w->failures = 0;
        return true;
    }

    std::vector<Command> failed(const std::string& key, std::uint64_t generation, Time now) {
        auto* w = current(key, generation);
        if (!w || w->state == State::WaitingRetry) return {};
        sessions_.close_profile(key);
        w->state = State::WaitingRetry;
        w->failures = std::min(w->failures + 1, 6u);
        w->retry_at = now + std::chrono::seconds(1u << (w->failures - 1));
        return {{Operation::Stop, w->profile, generation}};
    }

    std::vector<Command> tick(Time now) {
        std::vector<Command> commands;
        for (auto& entry : workers_) {
            auto& w = entry.second;
            if (w.state != State::WaitingRetry || now < w.retry_at) continue;
            w.generation = ++generation_;
            w.state = State::Connecting;
            commands.push_back({Operation::Start, w.profile, w.generation});
        }
        return commands;
    }

    // Only an adapter which has authenticated a viewer may call this method.
    // Local approval is separate and is required when configured.
    Admission admit_authenticated(const SessionKey& key, std::uint64_t generation,
                                 bool requested_control, bool locally_approved) {
        auto* w = current(key.profile_key, generation);
        if (!w || w->state != State::Online ||
            (w->profile.require_local_confirmation && !locally_approved)) return Admission::Rejected;
        return sessions_.admit(key, requested_control);
    }
    // The LAN adapter will enforce its own authentication and approval before
    // integration. Until then no LAN admission API is exposed.
    bool can_control(const SessionKey& key, std::uint64_t generation) {
        auto* w = current(key.profile_key, generation);
        return w && w->state == State::Online && sessions_.can_control(key);
    }
    void close(const SessionKey& key, std::uint64_t generation) {
        if (current(key.profile_key, generation)) sessions_.close(key);
    }
    bool set_policy(ViewerPolicy p) { return sessions_.set_policy(p); }
    const std::map<std::string, Status>& statuses() const noexcept { return workers_; }
    std::size_t session_count() const noexcept { return sessions_.size(); }

private:
    Status* current(const std::string& key, std::uint64_t generation) {
        auto it = workers_.find(key);
        return it != workers_.end() && it->second.generation == generation ? &it->second : nullptr;
    }
    std::vector<ServerProfile> profiles_;
    std::map<std::string, Status> workers_;
    SessionRegistry sessions_;
    std::uint64_t generation_ = 0;
};

} // namespace redesk::host
