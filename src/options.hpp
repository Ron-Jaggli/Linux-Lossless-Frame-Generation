#pragma once

#include <cstdint>
#include <string>

namespace lsfg {

struct Options {
    int multiplier = 2;          // frame-gen multiplier (unused until milestone 3)
    bool fullscreen = false;
    std::string present_mode = "fifo"; // fifo | mailbox | immediate
    bool drm_test = false;       // capture, measure brightness, report, exit
    double drm_test_seconds = 12.0;
    bool no_dmabuf = false;      // force SHM capture path
    bool validate = false;       // enable Vulkan validation layers
    bool verbose = false;
    bool no_restore = false;     // ignore saved portal restore token
};

} // namespace lsfg
