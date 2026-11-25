#include "server_runtime.h"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << "Failed line " << __LINE__ << ": " << #x << "\n"; return EXIT_FAILURE; } } while (false)
using namespace redesk::host;
using R = ServerRuntime;
int main() {
    R r;
    const auto now = R::Time{};
    ServerProfile a; a.key = "a"; a.endpoint = "a"; a.enabled = true;
    auto b = a; b.key = "b"; b.endpoint = "b"; b.require_local_confirmation = false;
    auto commands = r.apply({a,b});
    CHECK(commands.size() == 2);
    const auto ga = r.statuses().at("a").generation;
    const auto gb = r.statuses().at("b").generation;
    CHECK(r.admit_authenticated({"a","x"},ga,true,true) == Admission::Rejected);
    CHECK(r.online("a", ga) && r.online("b",gb));
    CHECK(r.admit_authenticated({"a","x"},ga,true,false) == Admission::Rejected);
    CHECK(r.admit_authenticated({"a","x"},ga,true,true) == Admission::Control);
    CHECK(r.admit_authenticated({"b","x"},gb,true,false) == Admission::Rejected);
    CHECK(r.set_policy(ViewerPolicy::SingleController));
    CHECK(r.admit_authenticated({"b","x"},gb,true,false) == Admission::ViewOnly);
    CHECK(!r.can_control({"b","x"},gb));
    commands = r.failed("a",ga,now);
    CHECK(commands.size()==1 && commands[0].operation==R::Operation::Stop);
    CHECK(r.session_count()==1 && r.statuses().at("b").state==R::State::Online);
    CHECK(!r.online("a",ga) && r.failed("a",ga,now).empty());
    CHECK(r.tick(now).empty());
    commands=r.tick(now+std::chrono::seconds(1));
    CHECK(commands.size()==1 && commands[0].profile.key=="a");
    auto ga2=commands[0].generation;
    CHECK(ga2!=ga && !r.online("a",ga) && r.online("a",ga2));
    CHECK(r.admit_authenticated({"a","x"},ga2,true,true)==Admission::Control);
    r.close({"a","x"},ga);
    CHECK(r.can_control({"a","x"},ga2));
    a.label="new name";
    CHECK(r.apply({a,b}).empty());
    CHECK(r.statuses().at("a").profile.label=="new name");
    ++a.credential_revision;
    commands=r.apply({a,b});
    CHECK(commands.size()==2 && commands[0].operation==R::Operation::Stop && commands[1].operation==R::Operation::Start);
    CHECK(r.session_count()==1 && r.statuses().at("b").generation==gb);
    b.require_local_confirmation=true;
    commands=r.apply({a,b});
    CHECK(commands.size()==1 && commands[0].operation==R::Operation::UpdatePolicy);
    CHECK(r.admit_authenticated({"b","y"},gb,true,false)==Admission::Rejected);
    auto invalid=a; invalid.key=R::LanKey;
    bool rejected=false;
    try { r.apply({invalid}); } catch(const std::invalid_argument&) { rejected=true; }
    CHECK(rejected && r.statuses().size()==2);
    CHECK(r.apply({}).size()==2 && r.session_count()==0 && r.statuses().empty());
    std::cout << "Server runtime tests passed\n";
}
