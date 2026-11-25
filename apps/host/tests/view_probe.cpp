#include "signal_peer.h"
#include "capture_source.h"
#include <remote_action.h>
#include <windows.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace redesk::host;
using namespace redesk::host::windows;
using namespace std::chrono_literals;
int main(int argc, char** argv) {
    if (argc != 3) {
        std::cout << "Usage: redesk-host-view-probe wss://server/signal VIEWER_NUMERIC_ID\n"
                     "Temporary registration, one approved viewer, primary display only.\n";
        return argc == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (MiniRtcConfigureLogging(MiniRtcLogOff, nullptr, nullptr) != 0) return EXIT_FAILURE;
    try {
        CaptureSource capture;
        capture.initialize(15);
        SignalPeer peer(argv[1], "", "", DeviceKey::generate());
        const std::string viewer = std::string("C-") + argv[2];
        peer.enable_view_probe(viewer);
        peer.start();
        auto deadline = std::chrono::steady_clock::now() + 30s;
        while (peer.status().state != MiniRtcSignalConnected &&
               !peer.status().revoked && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(50ms);
        const auto status = peer.status();
        if (status.state != MiniRtcSignalConnected || status.revoked) {
            std::cerr << "Signaling not connected\n";
            return EXIT_FAILURE;
        }
        auto password = peer.take_registration_password();
        if (password.empty()) {
            std::cerr << "Server did not provide a new registration password\n";
            return EXIT_FAILURE;
        }
        std::cout << "Temporary host ID: " << status.device_id
                  << "\nTemporary password: " << password
                  << "\nWaiting for viewer " << viewer << std::endl;
        OPENSSL_cleanse(password.data(), password.size());
        password.clear();

        deadline = std::chrono::steady_clock::now() + 120s;
        while (!peer.status().viewer_connected && !peer.status().revoked &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(50ms);
        if (!peer.status().viewer_connected || peer.status().revoked) {
            std::cerr << "Viewer not connected or admission revoked\n";
            return EXIT_FAILURE;
        }
        const auto prompt = "Allow " + viewer + " to view the primary monitor for up to 60 seconds? No remote control.";
        if (MessageBoxA(nullptr, prompt.c_str(), "ReDesk Host - view test",
            MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES ||
            !peer.approve_viewer()) return EXIT_FAILURE;

        const auto& d = capture.displays().front();
        auto info = crossdesk::MakeHostInformation("ReDesk Host (view test)",
            {{d.name,d.left,d.top,d.right,d.bottom}},false,"",crossdesk::HostPlatform::Windows);
        std::string message;
        try { message = info.to_json(); }
        catch (...) { crossdesk::FreeRemoteAction(info); throw; }
        crossdesk::FreeRemoteAction(info);
        // Reliable control may not be ready on the first Connected callback.
        deadline = std::chrono::steady_clock::now() + 5s;
        bool announced = false;
        while (peer.view_active() && std::chrono::steady_clock::now() < deadline) {
            if (peer.send_host_info(message)) { announced = true; break; }
            std::this_thread::sleep_for(50ms);
        }
        if (!announced) {
            std::cerr << "Host information could not be sent\n";
            return EXIT_FAILURE;
        }
        capture.start();
        std::cout << "Sending primary display. Press Esc locally to stop; timeout 60 seconds.\n";
        std::uint64_t last = 0, frames = 0;
        deadline = std::chrono::steady_clock::now() + 60s;
        while (peer.view_active() && std::chrono::steady_clock::now() < deadline &&
               !(GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
            auto latest = capture.latest();
            if (latest.pixels && latest.stream == "Display1" && latest.sequence != last) {
                last = latest.sequence;
                const auto* desc = latest.pixels->Descriptor();
                MiniRtcVideoFrame frame{};
                frame.native_frame = desc;
                frame.size = latest.pixels->Size();
                frame.width = desc->width;
                frame.height = desc->height;
                if (peer.send_frame(frame)) ++frames;
            }
            std::this_thread::sleep_for(15ms);
        }
        capture.stop();
        peer.stop();
        std::cout << "Frames submitted (not delivery confirmation): " << frames << "\n";
        return frames ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception&) {
        std::cerr << "View probe failed\n";
        return EXIT_FAILURE;
    }
}
