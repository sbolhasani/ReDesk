#pragma once
#include "speaker_capture_controller.h"
#include "speaker_capturer_wasapi.h"
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <memory>
#include <vector>
#include <algorithm>

namespace redesk::host::windows {

// System playback only: 48 kHz mono signed 16-bit PCM. A bounded ring of
// complete 10 ms frames separates the audio callback from the transport owner.
class AudioProbe {
public:
    using Clock=std::chrono::steady_clock;
    struct Packet {
        std::array<char,960> pcm{};
        Clock::time_point captured{};
    };
    AudioProbe() : state_(std::make_shared<State>()),
        capture_([] { return std::make_unique<crossdesk::SpeakerCapturerWasapi>(); },
            [state=state_](unsigned char* data,std::size_t size,const char*) {
                state->push(data,size);
            }) {}
    ~AudioProbe() { set_enabled(false); capture_.Shutdown(); }
    AudioProbe(const AudioProbe&)=delete;
    AudioProbe& operator=(const AudioProbe&)=delete;
    void set_enabled(bool enable) {
        if (enabled_==enable) return;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->accept=enable;
            state_->count=state_->head=state_->partial_size=0;
        }
        enabled_=enable;
        capture_.SetEnabled(enable);
    }
    bool running() const { return capture_.IsRunning(); }
    std::vector<Packet> take() {
        std::vector<Packet> result;
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto now=Clock::now();
        result.reserve(state_->count);
        while (state_->count) {
            auto& item=state_->ring[state_->head];
            if (now-item.captured <= std::chrono::milliseconds(100))
                result.push_back(item);
            state_->head=(state_->head+1)%state_->ring.size();
            --state_->count;
        }
        return result;
    }
private:
    struct State {
        std::mutex mutex;
        bool accept=false;
        std::array<Packet,10> ring{};
        Packet partial{};
        std::size_t count=0,head=0,partial_size=0;
        void push(const unsigned char* data,std::size_t size) {
            if (!data || !size || size%2 || size>192000) return;
            std::lock_guard<std::mutex> lock(mutex);
            if (!accept) return;
            while(size) {
                const auto n=(std::min)(size,partial.pcm.size()-partial_size);
                std::memcpy(partial.pcm.data()+partial_size,data,n);
                data+=n; size-=n; partial_size+=n;
                if(partial_size==partial.pcm.size()) {
                    partial.captured=Clock::now();
                    if(count==ring.size()) {
                        head=(head+1)%ring.size();
                        --count;
                    }
                    ring[(head+count)%ring.size()]=partial;
                    ++count;
                    partial_size=0;
                }
            }
        }
    };
    std::shared_ptr<State> state_;
    crossdesk::SpeakerCaptureController capture_;
    bool enabled_=false;
};
}
