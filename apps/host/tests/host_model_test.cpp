#include "host_model.h"
#include <cstdlib>
#include <iostream>

using namespace redesk::host;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return EXIT_FAILURE; } } while (false)

int main() {
    ServerProfile a; a.key = "a"; a.endpoint = "wss://a.example"; a.enabled = true;
    ServerProfile b = a; b.key = "b"; b.endpoint = "wss://b.example";
    ServerProfile c = a; c.key = "c"; c.endpoint = "wss://c.example"; c.enabled = false;
    auto changes = plan_changes({}, {a,b,c});
    CHECK(changes.size() == 2);
    CHECK(changes[0].key == "a" && changes[0].action == Change::Start);
    auto renamed = a; renamed.label = "Office";
    CHECK(plan_changes({a,b}, {b,renamed}).empty());
    auto rotated = a; ++rotated.credential_revision;
    changes = plan_changes({a,b}, {rotated,b});
    CHECK(changes.size() == 1 && changes[0].key == "a" && changes[0].action == Change::Reconnect);
    auto policy = b; policy.require_local_confirmation = false;
    changes = plan_changes({a,b}, {a,policy});
    CHECK(changes.size() == 1 && changes[0].key == "b" && changes[0].action == Change::UpdatePolicy);
    changes = plan_changes({a,b,c}, {b});
    CHECK(changes.size() == 1 && changes[0].key == "a" && changes[0].action == Change::Stop);
    auto enabled = c; enabled.enabled = true;
    changes = plan_changes({a,b,c}, {a,b,enabled});
    CHECK(changes.size() == 1 && changes[0].key == "c" && changes[0].action == Change::Start);
    bool duplicate_rejected = false;
    try { (void)plan_changes({}, {a,a}); } catch (const std::invalid_argument&) { duplicate_rejected = true; }
    CHECK(duplicate_rejected);

    SessionRegistry registry;
    const SessionKey x{"a", "same"}, y{"b", "same"}, z{"lan", "third"};
    CHECK(registry.admit(x, true) == Admission::Control);
    CHECK(registry.admit(y, true) == Admission::Rejected);
    CHECK(registry.admit(x, true) == Admission::Rejected);
    CHECK(registry.set_policy(ViewerPolicy::SingleController));
    CHECK(registry.admit(y, true) == Admission::ViewOnly);
    CHECK(!registry.can_control(y));
    CHECK(!registry.set_policy(ViewerPolicy::SingleViewer));
    CHECK(registry.policy() == ViewerPolicy::SingleController);
    registry.close_profile("a");
    CHECK(registry.size() == 1 && !registry.can_control(x) && !registry.can_control(y));
    CHECK(registry.admit(z, true) == Admission::Control);
    CHECK(registry.set_policy(ViewerPolicy::MultipleControllers));
    CHECK(registry.admit(x, true) == Admission::Control);
    CHECK(registry.can_control(x) && registry.can_control(z));
    CHECK(!registry.set_policy(ViewerPolicy::SingleController));
    registry.close(x);
    registry.close(y);
    CHECK(registry.set_policy(ViewerPolicy::SingleViewer));
    registry.close(z);
    CHECK(registry.admit({"a", ""}, true) == Admission::Rejected);
    CHECK(registry.admit(x, false) == Admission::ViewOnly);
    CHECK(!registry.can_control(x));
    std::cout << "Host model tests passed\n";
    return EXIT_SUCCESS;
}
