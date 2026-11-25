#include "signal_peer.h"
#include "capture_source.h"
#include "input_probe_controller.h"
#include "clipboard_probe.h"
#include "interactive_desktop.h"
#include <memory>
#include <string_view>
#include <remote_action.h>
#include <app_version.h>
#include <windows.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace redesk::host;
using namespace redesk::host::windows;
using namespace std::chrono_literals;
int main(int argc, char** argv) {
    const bool control = argc >= 4 && std::string_view(argv[3]) == "--control";
    const bool clipboard = argc == 5 && control && std::string_view(argv[4]) == "--clipboard";
    if (argc != 3 && !(argc == 4 && control) && !clipboard) {
        std::cout << "Usage: redesk-host-view-probe wss://server/signal VIEWER_NUMERIC_ID [--control [--clipboard]]\n"
                     "Temporary registration, one approved viewer, primary display only.\n";
        return argc == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (MiniRtcConfigureLogging(MiniRtcLogOff, nullptr, nullptr) != 0) return EXIT_FAILURE;
    try {
        if (!crossdesk::IsValidAppVersion(CROSSDESK_VERSION)) {
            std::cerr << "Invalid configured host build version\n";
            return EXIT_FAILURE;
        }
        CaptureSource capture;
        capture.initialize(15);
        SignalPeer peer(argv[1], "", "", DeviceKey::generate());
        const std::string viewer = std::string("C-") + argv[2];
        peer.enable_view_probe(viewer,control,clipboard);
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
        const auto prompt = "Allow " + viewer + (control ? " to view and control the primary desktop for up to 60 seconds?" : " to view the primary monitor for up to 60 seconds? No remote control.") + (clipboard ? "\nClipboard text will be shared in both directions after approval." : "");
        if (MessageBoxA(nullptr, prompt.c_str(), "ReDesk Host - view test",
            MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES ||
            !peer.approve_viewer()) return EXIT_FAILURE;

        const auto& d = capture.displays().front();
        auto info = crossdesk::MakeHostInformation("ReDesk Host (view test)",
            {{d.name,d.left,d.top,d.right,d.bottom}},false,CROSSDESK_VERSION,crossdesk::HostPlatform::Windows);
        info.i.supports_text_input = control;
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
        std::unique_ptr<InputProbeController> input;
        if (control) input = std::make_unique<InputProbeController>(d);
        std::unique_ptr<ClipboardProbe> clip;
        if (clipboard) clip = std::make_unique<ClipboardProbe>();
        capture.start();
        std::cout << (control ? "View and control enabled. " : "View-only. ")
                  << "Press Esc to stop; timeout 60 seconds.\n";
        std::uint64_t last = 0, frames = 0;
        deadline = std::chrono::steady_clock::now() + 60s;
        while (peer.view_active() && std::chrono::steady_clock::now() < deadline &&
               !(GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
            // This probe does not authorize lock-screen/UAC input or capture.
            if (!crossdesk::IsCurrentSessionUserDesktopActive()) break;
            if (input) {
                bool ok=true;
                for (const auto& event:peer.take_input()) {
                    if (!peer.view_active() || (GetAsyncKeyState(VK_ESCAPE)&0x8000)) { ok=false; break; }
                    if (!input->apply(event.stream,event.payload)) { ok=false; break; }
                }
                if (!ok || !input->tick()) break;
            }
            if (clip && peer.view_active()) {
                if (auto value=peer.take_clipboard()) clip->receive(std::move(*value));
                clip->tick([&] { return peer.view_active() &&
                    crossdesk::IsCurrentSessionUserDesktopActive(); },
                    [&](const std::string& value) { return peer.send_clipboard(value); });
            }
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
        bool released = true;
        if (input) {
            for (int attempt=0;attempt<5;++attempt) {
                released=input->release();
                if (released) break;
                std::this_thread::sleep_for(100ms);
            }
            if (!released) std::cerr << "Warning: remote input cleanup incomplete\n";
            else std::cout << "Remote input cleanup passed (native release calls accepted)\n";
        }
        std::cout << "Frames submitted (not delivery confirmation): " << frames << "\n";
        return frames && released ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception&) {
        std::cerr << "View probe failed\n";
        return EXIT_FAILURE;
    }
}
