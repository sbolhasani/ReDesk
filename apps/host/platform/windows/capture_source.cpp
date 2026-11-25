#include "capture_source.h"
#include <display_stream_id.h>
#include <stdexcept>
#include <utility>

namespace redesk::host::windows {
CaptureSource::CaptureSource() : pool_(crossdesk::CapturedNv12FramePool::Create(3)) {}
CaptureSource::~CaptureSource() { stop(); }
void CaptureSource::initialize(int fps) {
    if (initialized_ || fps < 1 || fps > 60)
        throw std::invalid_argument("Invalid capture initialization");
    if (backend_.Init(fps, [this](unsigned char* data, int size, int width, int height,
            const char* stream, const MiniRtcNativeVideoFrame*) {
            publish(data, size, width, height, stream);
        }) != 0)
        throw std::runtime_error("No usable interactive displays");
    displays_ = backend_.GetDisplayInfoList();
    if (displays_.empty()) throw std::runtime_error("No usable interactive displays");
    // Do not allow absurd geometry to reach the backend's int-sized buffers.
    for (const auto& d : displays_)
        if (d.width < 2 || d.height < 2 || d.width > 16384 || d.height > 16384 ||
            std::uint64_t(d.width) * d.height > 67108864)
            throw std::runtime_error("Unsupported display geometry");
    desired_ = crossdesk::MakeDisplayStreamId(0);
    initialized_ = true;
}
void CaptureSource::start(bool cursor) {
    if (!initialized_) throw std::logic_error("Capture not initialized");
    if (running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = true;
        latest_ = {};
    }
    try {
        if (backend_.Start(cursor) != 0) throw std::runtime_error("Capture start failed");
        running_ = true;
    } catch (...) {
        stop();
        throw;
    }
}
void CaptureSource::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        latest_ = {};
    }
    // Never join while holding the callback mutex.
    backend_.Stop();
    running_ = false;
}
bool CaptureSource::select(std::size_t monitor) {
    if (!initialized_ || monitor >= displays_.size()) return false;
    auto stream = crossdesk::MakeDisplayStreamId(monitor);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stream == desired_) return true;
        desired_ = std::move(stream);
        latest_ = {};
    }
    // The GDI backend has an immutable display list after Init and selects
    // its monitor atomically. Old in-flight frames are filtered by stream ID.
    return backend_.SwitchTo(static_cast<int>(monitor)) == 0;
}
CaptureSource::Frame CaptureSource::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}
void CaptureSource::publish(unsigned char* data, int size, int width, int height,
                            const char* stream) noexcept {
    try {
        if (!data || !stream || size <= 0 || width <= 0 || height <= 0 ||
            (width & 1) || (height & 1) ||
            std::uint64_t(width) * height * 3 / 2 != static_cast<std::uint64_t>(size))
            return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_ || desired_ != stream) return;
        }
        auto* raw = pool_->CopyFrom(data, static_cast<std::size_t>(size),
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        if (!raw) return;
        FramePtr frame(raw, [](const crossdesk::CapturedNv12Frame* p) {
            const_cast<crossdesk::CapturedNv12Frame*>(p)->Release();
        });
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ || desired_ != stream) return;
        latest_ = {std::move(frame), stream, ++sequence_};
    } catch (...) {
        // A dropped capture must never propagate into the backend thread.
    }
}
}
