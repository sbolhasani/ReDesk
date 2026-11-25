#include "signal_peer.h"
#include "capture_source.h"
#include "input_probe_controller.h"
#include "clipboard_probe.h"
#include "audio_probe.h"
#include "file_probe.h"
#include <shellapi.h>
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
    unsigned duration_minutes=15;
    bool duration_set=false;
    std::filesystem::path receive_dir,send_file;
    int wide_argc=0;
    auto wide_argv=CommandLineToArgvW(GetCommandLineW(),&wide_argc);
    if (!wide_argv || wide_argc!=argc) { if(wide_argv) LocalFree(wide_argv); return EXIT_FAILURE; }
    bool valid=argc>=3;
    for(int i=3;i<argc;++i) {
        const std::string_view flag(argv[i]);
        if(flag=="--control" && !control) control=true;
        else if(flag=="--clipboard" && !clipboard) clipboard=true;
        else if(flag=="--audio" && !audio) audio=true;
        else if(flag=="--files" && receive_dir.empty() && i+1<argc) receive_dir=wide_argv[++i];
        else if(flag=="--send-file" && send_file.empty() && i+1<argc) send_file=wide_argv[++i];
        else if(flag=="--duration-minutes" && !duration_set && i+1<argc) {
            duration_set=true;
            const std::string_view value(argv[++i]);
            unsigned parsed=0;
            if(value.empty() || value.size()>3) valid=false;
            else {
                for(char ch:value) {
                    if(ch<'0' || ch>'9') { valid=false; break; }
                    parsed=parsed*10+static_cast<unsigned>(ch-'0');
                }
                if(parsed<1 || parsed>120) valid=false;
                else duration_minutes=parsed;
            }
        }
        else valid=false;
    }
    LocalFree(wide_argv);
    const bool files=!receive_dir.empty();
    if (!valid || ((clipboard || files) && !control) || (!files && !send_file.empty())) {
        std::cout << "Usage: redesk-host-view-probe wss://server/signal VIEWER_NUMERIC_ID [--control] [--clipboard] [--audio] [--files EXISTING_LOCAL_DIRECTORY] [--send-file LOCAL_FILE] [--duration-minutes 1-120]\n"
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
        peer.enable_view_probe(viewer,control,clipboard,audio,files);
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
        const auto prompt = "Allow " + viewer + (control ? " to view and control the primary desktop for up to " : " to view the primary monitor for up to ") + std::to_string(duration_minutes) + " minutes?" + (control ? "" : " No remote control.") + (clipboard ? "\nClipboard text will be shared in both directions after approval." : "") + (audio ? "\nSystem playback audio will be shared after approval (not microphone)." : "") + (files ? "\nFile transfers are allowed into a new isolated folder." : "") + (!send_file.empty() ? "\nThe locally selected file will also be sent to this viewer." : "");
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
        std::unique_ptr<FileProbe> transfer;
        if (files) transfer=std::make_unique<FileProbe>(peer,receive_dir,send_file);
        std::uint64_t audio_packets=0;
        bool audio_running_seen=false;
        std::optional<bool> last_audio;
        std::optional<bool> last_audio_running;
        std::uint64_t last_audio_commands=0;
        auto next_audio_report=std::chrono::steady_clock::now();
        std::uint64_t audio_submitted_at_report=0;
        capture.start();
        std::cout << (control ? "View and control enabled. " : "View-only. ")
                  << "Press Esc to stop; timeout " << duration_minutes << " minutes.\n";
        std::uint64_t last = 0, frames = 0;
        deadline = std::chrono::steady_clock::now() + std::chrono::minutes(duration_minutes);
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
                const auto control_status=peer.status();
                const bool requested=peer.audio_requested();
                if (control_status.audio_commands != last_audio_commands) {
                    std::cout << "Audio command received (#" << control_status.audio_commands
                              << "): " << (requested ? "enable" : "mute") << std::endl;
                    last_audio_commands=control_status.audio_commands;
                }
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
                              << audio_packets-audio_submitted_at_report
                              << "; control messages: " << control_status.control_messages
                              << "; audio commands: " << control_status.audio_commands << std::endl;
                    audio_submitted_at_report=audio_packets;
                    next_audio_report=now+5s;
                }
            }
            if (transfer) transfer->tick();
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
        if (transfer) {
            transfer->stop();
            std::cout << "Completed received files: " << transfer->completed() << std::endl;
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
