#include "signal_peer.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include <vector>

using namespace redesk::host;
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Build/link check passed. To test temporary registrations:\n"
                     "  redesk-host-signal-probe wss://server1/signal wss://server2/signal\n";
        return EXIT_SUCCESS;
    }
    if (argc > 9) {
        std::cerr << "Diagnostic accepts up to eight endpoints\n";
        return EXIT_FAILURE;
    }
    // Suppress library logs: initial login and signaling messages may contain
    // registration credentials. The probe never writes settings or passwords.
    if (MiniRtcConfigureLogging(MiniRtcLogOff, nullptr, nullptr) != 0) return EXIT_FAILURE;
    try {
        std::set<std::string> endpoints;
        std::vector<std::unique_ptr<SignalPeer>> peers;
        for (int i = 1; i < argc; ++i) {
            if (!endpoints.emplace(argv[i]).second)
                throw std::invalid_argument("Duplicate diagnostic endpoint");
            peers.push_back(std::make_unique<SignalPeer>(argv[i], "", "", DeviceKey::generate()));
        }
        for (auto& peer : peers) peer->start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool online = false;
        do {
            online = true;
            for (auto& peer : peers) online &= peer->status().state == MiniRtcSignalConnected;
            if (online) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (std::chrono::steady_clock::now() < deadline);
        for (std::size_t i=0; i<peers.size(); ++i) {
            auto status = peers[i]->status();
            std::cout << "Server " << i+1 << ": "
                      << (status.state == MiniRtcSignalConnected ? "Online" : "Not online")
                      << " (status " << static_cast<int>(status.state) << ")\n";
            online &= status.state == MiniRtcSignalConnected;
        }
        for (auto& peer : peers) peer->stop();
        return online ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception&) {
        std::cerr << "Signaling probe failed; check endpoint and configuration\n";
        return EXIT_FAILURE;
    }
}
