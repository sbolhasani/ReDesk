#include "host_settings.h"
#include <cstdlib>
#include <iostream>

using namespace redesk::host;
#define CHECK(x) do { if (!(x)) { std::cerr << "Failed at " << __LINE__ << '\n'; return EXIT_FAILURE; } } while (false)
template<class F> bool rejects(F f) {
    try { f(); } catch (const std::invalid_argument&) { return true; }
    return false;
}
int main() {
    HostSettings settings;
    settings.viewer_policy = ViewerPolicy::MultipleControllers;
    StoredProfile a;
    a.profile.key = "office";
    a.profile.label = "Office";
    a.profile.endpoint = "wss://office.example/signal";
    a.profile.device_id = "123456789";
    a.profile.kind = ServerKind::Cloudflare;
    a.profile.enabled = true;
    a.profile.credential_revision = 42;
    a.password = "example-password";
    a.identity = std::string("opaque\0identity", 15);
    auto b = a;
    b.profile.key = "personal";
    b.profile.endpoint = "wss://personal.example/signal";
    b.profile.device_id = "987654321";
    b.profile.password_mode = PasswordMode::Temporary;
    b.profile.kind = ServerKind::SelfHosted;
    b.password = "different-password";
    b.identity = "different-identity";
    b.profile.require_local_confirmation = false;
    settings.servers = {a,b};
    const auto encoded = snapshot::encode(settings);
    const auto decoded = snapshot::decode(encoded);
    CHECK(snapshot::encode(decoded) == encoded);
    CHECK(decoded.servers.size() == 2);
    CHECK(decoded.servers[0].identity == a.identity);
    CHECK(decoded.servers[1].identity == b.identity);
    CHECK(decoded.servers[1].password == b.password);
    CHECK(snapshot::decode(snapshot::encode(HostSettings{})).servers.empty());
    for (std::size_t n = 0; n < encoded.size(); ++n)
        CHECK(rejects([&] { snapshot::decode(std::string_view(encoded).substr(0,n)); }));
    CHECK(rejects([&] { snapshot::decode(encoded + "x"); }));
    auto bad = encoded; bad[4] = 2;
    CHECK(rejects([&] { snapshot::decode(bad); }));
    bad = encoded; bad[6] = 9;
    CHECK(rejects([&] { snapshot::decode(bad); }));
    bad = encoded; bad[7] = static_cast<char>(255);
    CHECK(rejects([&] { snapshot::decode(bad); }));
    settings.servers.push_back(a);
    CHECK(rejects([&] { snapshot::encode(settings); }));
    settings.servers.pop_back();
    settings.servers[0].profile.key = "@lan";
    CHECK(rejects([&] { snapshot::encode(settings); }));
    std::cout << "Host settings tests passed\n";
    return EXIT_SUCCESS;
}
