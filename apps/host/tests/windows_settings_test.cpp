#include "settings_store.h"
#include <cstdlib>
#include <iostream>
using namespace redesk::host;
using namespace redesk::host::windows;
#define CHECK(x) do { if (!(x)) { std::cerr << "Failed at " << __LINE__ << '\n'; return EXIT_FAILURE; } } while (false)
int main() {
    HostSettings s;
    StoredProfile p;
    p.profile.key = "test";
    p.profile.endpoint = "wss://example.invalid";
    p.profile.device_id = "12345";
    p.password = "test-only-password";
    p.identity = "test-only-identity";
    s.servers.push_back(p);
    const auto expected = snapshot::encode(s);
    for (auto scope : {ProtectionScope::CurrentUser, ProtectionScope::LocalMachine}) {
        auto blob = protect_settings(s, scope);
        CHECK(blob != expected);
        CHECK(snapshot::encode(unprotect_settings(blob)) == expected);
        blob[blob.size()/2] ^= 1;
        bool rejected = false;
        try { (void)unprotect_settings(blob); } catch (const std::exception&) { rejected = true; }
        CHECK(rejected);
    }
    const auto portable = settings_directory(StoreMode::Portable);
    const auto installed = settings_directory(StoreMode::Installed);
    CHECK(portable.is_absolute() && installed.is_absolute());
    CHECK(portable.filename() == L"ReDeskHost" && installed.filename() == L"ReDeskHost");
    CHECK(portable != installed);
    // Never write to the real application's data directory from this test.
    std::cout << "Windows settings protection tests passed\n";
    return EXIT_SUCCESS;
}
