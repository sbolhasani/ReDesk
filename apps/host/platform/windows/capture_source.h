#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include "display_info.h"
#include "captured_nv12_frame.h"
#include "screen_capturer_gdi.h"

namespace redesk::host::windows {

// One capture source per interactive desktop, not per signaling peer.
// Only the serialized owner calls lifecycle methods; the existing native
// backend invokes publish() on its own capture thread. Consumers retain the
// latest frame without borrowing GDI or backend-owned pixel storage.
class CaptureSource {
public:
    using FramePtr = std::shared_ptr<const crossdesk::CapturedNv12Frame>;
    struct Frame {
        FramePtr pixels;
        std::string stream;
        std::uint64_t sequence = 0;
    };
    CaptureSource();
    ~CaptureSource();
    CaptureSource(const CaptureSource&) = delete;
    CaptureSource& operator=(const CaptureSource&) = delete;
    void initialize(int fps = 30);
    const std::vector<crossdesk::DisplayInfo>& displays() const noexcept { return displays_; }
    void start(bool cursor = true);
    void stop();
    bool select(std::size_t monitor);
    Frame latest() const;
private:
    void publish(unsigned char*, int, int, int, const char*) noexcept;
    crossdesk::ScreenCapturerGdi backend_;
    std::shared_ptr<crossdesk::CapturedNv12FramePool> pool_;
    std::vector<crossdesk::DisplayInfo> displays_;
    mutable std::mutex mutex_;
    std::string desired_;
    Frame latest_;
    std::uint64_t sequence_ = 0;
    bool initialized_ = false;
    bool running_ = false; // owner-thread only
    bool accepting_ = false; // protected by mutex_
};
}
