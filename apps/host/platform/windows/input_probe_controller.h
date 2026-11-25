#pragma once
#include "mouse_controller.h"
#include "keyboard_capturer.h"
#include "keyboard_state.h"
#include <stream_names.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <string>

// Diagnostic adapter for one locally approved controller. Never installs a
// keyboard hook. All methods run on the probe owner thread, not RTC callbacks.
namespace redesk::host::windows {
class InputProbeController {
public:
    explicit InputProbeController(const crossdesk::DisplayInfo& display) {
        mouse_.Init({display});
    }
    ~InputProbeController() {
        // Best effort fallback for exceptional exit; the normal path reports
        // any cleanup failure and makes additional bounded attempts.
        try { release(); } catch (...) {}
    }
    bool apply(const std::string& stream, const std::string& payload) {
        using namespace crossdesk;
        const auto json = nlohmann::json::parse(payload,nullptr,false);
        if (!json.is_object() || !json.contains("type") || !json["type"].is_number_integer()) return true;
        const auto type = json["type"];
        const bool mouse = stream == kMouseStream && type == int(ControlType::mouse);
        const bool keyboard = stream == kKeyboardStream &&
            (type == int(ControlType::keyboard) || type == int(ControlType::keyboard_state) ||
             type == int(ControlType::text_input));
        if (!mouse && !keyboard) return true;
        RemoteAction action{};
        if (!action.from_json(payload)) return true;
        if (mouse) {
            if (!std::isfinite(action.m.x) || !std::isfinite(action.m.y) ||
                action.m.flag < MouseFlag::move || action.m.flag > MouseFlag::wheel_horizontal ||
                action.m.s < -100 || action.m.s > 100) return true;
            return mouse_.SendMouseCommand(action,0) == 0;
        }
        if (action.type == ControlType::text_input)
            return keyboard_.SendTextCommand(action.text) == 0;
        if (action.type == ControlType::keyboard) {
            const auto& k = action.k;
            if (!valid(k.key_value,k.scan_code) ||
                (k.flag != KeyFlag::key_down && k.flag != KeyFlag::key_up)) return true;
            const int code = static_cast<int>(k.key_value);
            const bool down = k.flag == KeyFlag::key_down;
            // Do not release a locally held key that this session never pressed.
            if (!down && !pressed_.count(code)) return true;
            auto key = KeyboardKey{code,k.scan_code,k.extended};
            if (!down) key = pressed_.at(code);
            last_keyboard_ = Clock::now();
            return inject(key,down);
        }
        if (action.ks.seq && seq_ &&
            static_cast<int32_t>(action.ks.seq-seq_) <= 0) return true;
        PressedKeys desired;
        for (size_t i=0;i<action.ks.pressed_count;++i) {
            const auto& k=action.ks.pressed_keys[i];
            if (!valid(k.key_value,k.scan_code)) return true;
            const int code=static_cast<int>(k.key_value);
            desired[code]={code,k.scan_code,k.extended};
        }
        seq_=action.ks.seq;
        last_keyboard_=Clock::now();
        // Reuse the shared keyboard-state repair and modifier ordering.
        for (const auto& key : ReconcileKeyboardState(pressed_,desired))
            if (!inject({key.key_code,key.scan_code,key.extended},key.is_down)) return false;
        return true;
    }
    bool tick() {
        if (!pressed_.empty() && Clock::now()-last_keyboard_ > std::chrono::seconds(3))
            return release_keys();
        return true;
    }
    bool release() {
        const bool keys=release_keys();
        const bool buttons=mouse_.ReleasePressedButtons()==7;
        return keys && buttons;
    }
private:
    using Clock=std::chrono::steady_clock;
    static bool valid(std::size_t code,uint32_t scan) {
        return code >= 8 && code <= 254 && scan <= 255;
    }
    bool inject(crossdesk::KeyboardKey key,bool down) {
        if (keyboard_.SendKeyboardCommand(key.key_code,down,key.scan_code,key.extended)!=0)
            return false;
        if (down) pressed_[key.key_code]=key;
        else pressed_.erase(key.key_code);
        return true;
    }
    bool release_keys() {
        bool ok=true;
        const auto changes=crossdesk::ReconcileKeyboardState(pressed_,{});
        for (const auto& k:changes)
            if (!inject({k.key_code,k.scan_code,k.extended},false)) ok=false;
        return ok;
    }
    crossdesk::PlatformMouseController mouse_;
    crossdesk::PlatformKeyboardCapturer keyboard_;
    crossdesk::PressedKeys pressed_;
    uint32_t seq_=0;
    Clock::time_point last_keyboard_{};
};
}
