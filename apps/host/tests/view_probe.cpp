#include "signal_peer.h"
#include "capture_source.h"
#include "input_probe_controller.h"
#include "clipboard_probe.h"
#include "audio_probe.h"
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
#include <openssl/rand.h>

using namespace redesk::host;
using namespace redesk::host::windows;
using namespace std::chrono_literals;
namespace {
std::string numeric_test_password() {
    std::string result;
    while (result.size() < 6) {
        unsigned char byte=0;
        if (RAND_bytes(&byte,1)!=1) throw std::runtime_error("Random generator failed");
        if (byte<250) result += static_cast<char>('0' + byte%10);
    }
    return result;
}
}
int main(int argc, char** argv) {
    bool control=false, clipboard=false, audio=false;
    bool valid=argc>=3;
    for(int i=3;i<argc;++i) {
        const std::string_view flag(argv[i]);
        if(flag=="--control" && !control) control=true;
        else if(flag=="--clipboard" && !clipboard) clipboard=true;
        else if(flag=="--audio" && !audio) audio=true;
        else valid=false;
    }
    if (!valid || (clipboard && !control)) {
        std::cout << "Usage: redesk-host-view-probe wss://server/signal VIEWER_NUMERIC_ID [--control] [--clipboard] [--audio]\n"
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
        peer.enable_view_probe(viewer,control,clipboard,audio);
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
        // Rotate only this freshly created diagnostic identity. Never alter
        // an existing user's password. Wait for the server's acknowledgement.
        OPENSSL_cleanse(password.data(),password.size());
        password=numeric_test_password();
        if (!peer.request_test_password(password)) {
            std::cerr << "Could not request numeric diagnostic password\n";
            return EXIT_FAILURE;
        }
        deadline=std::chrono::steady_clock::now()+10s;
        while(peer.password_result()==SignalPeer::PasswordResult::Pending &&
              !peer.status().revoked && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(50ms);
        if(peer.password_result()!=SignalPeer::PasswordResult::Accepted || peer.status().revoked) {
            OPENSSL_cleanse(password.data(),password.size());
            std::cerr << "Numeric password not confirmed by server; restart probe\n";
            return EXIT_FAILURE;
        }
        const auto& id=status.device_id;
        std::cout << "Temporary host ID: " << id
                  << "\nID groups: " << (id.size()==9 ? id.substr(0,3)+" "+id.substr(3,3)+" "+id.substr(6,3) : id)
                  << "\nTemporary password: " << password
                  << "\nWaiting for viewer " << viewer << std::endl;
        OPENSSL_cleanse(password.data(), password.size());
        password.clear();

        deadline = std::chrono::steady_clock::now() + 10min;
        while (!peer.status().viewer_connected && !peer.status().revoked &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(50ms);
        if (!peer.status().viewer_connected || peer.status().revoked) {
            std::cerr << "Viewer not connected or admission revoked\n";
            return EXIT_FAILURE;
        }
        const auto prompt = "Allow " + viewer + (control ? " to view and control the primary desktop for up to 15 minutes?" : " to view the primary monitor for up to 15 minutes? No remote control.") + (clipboard ? "\nClipboard text will be shared in both directions after approval." : "") + (audio ? "\nSystem playback audio will be shared after approval (not microphone)." : "");
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
        std::unique_ptr<AudioProbe> sound;
        if (audio) sound=std::make_unique<AudioProbe>();
        std::uint64_t audio_packets=0;
        bool audio_running_seen=false;
        std::optional<bool> last_audio;
        std::optional<bool> last_audio_running;
        auto next_audio_report=std::chrono::steady_clock::now();
        std::uint64_t audio_submitted_at_report=0;
        capture.start();
        std::cout << (control ? "View and control enabled. " : "View-only. ")
                  << "Press Esc to stop; timeout 15 minutes.\n";
        std::uint64_t last = 0, frames = 0;
        deadline = std::chrono::steady_clock::now() + 15min;
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
            if (sound) {
                const bool requested=peer.audio_requested();
                // Keep the previously verified PCM submission path. Mute
                // stops capture and clears its bounded queue, independently
                // of optional transport pause APIs.
                sound->set_enabled(requested);
                if (!last_audio || *last_audio != requested) {
                    std::cout << "Viewer audio: " << (requested ? "enabled" : "muted") << "\n";
                    last_audio=requested;
                }
                const bool running=sound->running();
                audio_running_seen |= running;
                if (!last_audio_running || *last_audio_running!=running) {
                    std::cout << "Audio capture: " << (running ? "running" : "stopped") << std::endl;
                    last_audio_running=running;
                }
                for (const auto& packet:sound->take()) {
                    if (!peer.audio_requested() ||
                        !crossdesk::IsCurrentSessionUserDesktopActive()) break;
                    if (peer.send_audio(packet.pcm.data(),packet.pcm.size())) ++audio_packets;
                }
                const auto now=std::chrono::steady_clock::now();
                if (now>=next_audio_report) {
                    std::cout << "Audio packets submitted since last report: "
                              << audio_packets-audio_submitted_at_report << std::endl;
                    audio_submitted_at_report=audio_packets;
                    next_audio_report=now+5s;
                }
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
        if (sound) sound->set_enabled(false);
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
        if (audio) {
            std::cout << "Audio capture started: " << (audio_running_seen ? "yes" : "no")
                      << "\nAudio packets submitted (not delivery confirmation): " << audio_packets << "\n";
        }
        return frames && released && (!audio || (audio_running_seen && audio_packets)) ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception&) {
        std::cerr << "View probe failed\n";
        return EXIT_FAILURE;
    }
}
