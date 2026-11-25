#include "capture_source.h"
#include <display_stream_id.h>
#include <windows.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace redesk::host::windows;
int main() {
    // No network, no saved images; exercise physical coordinates on mixed DPI.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        CaptureSource source;
        source.initialize(15);
        std::cout << "Displays: " << source.displays().size() << "\n";
        if (source.latest().pixels) return EXIT_FAILURE;
        source.start();
        for (std::size_t i = 0; i < source.displays().size(); ++i) {
            if (!source.select(i)) return EXIT_FAILURE;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            CaptureSource::Frame frame;
            do {
                frame = source.latest();
                if (frame.pixels && frame.stream == crossdesk::MakeDisplayStreamId(i)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            } while (std::chrono::steady_clock::now() < deadline);
            if (!frame.pixels || frame.stream != crossdesk::MakeDisplayStreamId(i)) {
                std::cerr << "No frame for display " << i+1 << "\n";
                return EXIT_FAILURE;
            }
            const auto* d = frame.pixels->Descriptor();
            const auto& display = source.displays()[i];
            if (d->width != static_cast<std::uint32_t>(display.width & ~1) ||
                d->height != static_cast<std::uint32_t>(display.height & ~1) ||
                frame.pixels->Size() != std::size_t(d->width) * d->height * 3 / 2)
                return EXIT_FAILURE;
            std::cout << frame.stream << ": " << d->width << "x" << d->height
                      << " NV12 OK\n";
        }
        auto retained = source.latest();
        source.stop();
        if (source.latest().pixels || !retained.pixels) return EXIT_FAILURE;
        auto* descriptor = retained.pixels->Descriptor();
        std::vector<std::uint8_t> copy(retained.pixels->Size());
        if (descriptor->copy_to_nv12(descriptor->owner, copy.data(), copy.size()) != 0)
            return EXIT_FAILURE;
        source.start();
        source.stop();
        std::cout << "Capture start/stop, monitor selection and frame ownership passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "Capture probe failed: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
