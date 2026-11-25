#include "device_key.h"
#include "runtime/device_login.h"
#include <cstdlib>
#include <iostream>
using namespace redesk::host;
using namespace crossdesk;
using Json = nlohmann::json;
#define CHECK(x) do { if (!(x)) { std::cerr << "Failed at " << __LINE__ << '\n'; return EXIT_FAILURE; } } while(false)
int main() {
    auto a = DeviceKey::generate();
    auto b = DeviceKey::generate();
    CHECK(a.PublicKey() != b.PublicKey());
    auto restored = DeviceKey::restore(a.export_seed());
    CHECK(a.PublicKey() == restored.PublicKey());
    CHECK(a.Sign("test") == restored.Sign("test"));
    CHECK(a.Sign("test") != b.Sign("test"));
    char buffer[4096]{};
    DeviceLogin login;
    auto exchange = [&](MiniRtcSignalExchangeStage stage, const Json& msg) {
        return login.ExchangeWithSigner(stage, msg, a.PublicKey(),
            [&](std::string_view text) { return a.Sign(text); }, buffer, sizeof(buffer));
    };
    auto n = exchange(MiniRtcSignalExchangeStage::PrepareLogin,
        {{"type","login"},{"user_id","123456789@secret"}});
    CHECK(n > 0);
    auto reply = Json::parse(buffer, buffer + n);
    CHECK(reply.at("user_id") == "123456789");
    CHECK(reply.at("device_key").at("public_key") == a.PublicKey());
    CHECK(reply.dump().find("secret") == std::string::npos);
    Json challenge{{"type","login_challenge"},{"device_key_version",1},{"user_id","123456789"},
                   {"nonce",std::string(64,'a')},{"password_required",true}};
    auto invalid = challenge;
    invalid["user_id"] = "another";
    CHECK(exchange(MiniRtcSignalExchangeStage::IncomingMessage,invalid) == -1);
    n = exchange(MiniRtcSignalExchangeStage::IncomingMessage,challenge);
    CHECK(n > 0 && login.UsedPassword());
    reply = Json::parse(buffer, buffer + n);
    CHECK(reply.at("user_id") == "123456789@secret");
    auto transcript = Json::array({"crossdesk.device-login.v1","123456789",a.PublicKey(),std::string(64,'a')}).dump();
    CHECK(reply.at("device_key").at("signature") == a.Sign(transcript));
    CHECK(exchange(MiniRtcSignalExchangeStage::IncomingMessage,
        {{"type","login"},{"status","success"},{"device_key_version",1},{"user_id","123456789"}}) == 0);
    CHECK(!login.Pending());
    std::cout << "Host identity and shared login protocol tests passed\n";
    return EXIT_SUCCESS;
}
