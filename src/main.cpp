#include "capture.hpp"
#include "log.hpp"
#include "options.hpp"
#include "portal.hpp"
#include "renderer.hpp"
#include "vk/context.hpp"
#include "vk/frame_pool.hpp"

#include <SDL3/SDL.h>
#include <getopt.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace lsfg {

static const char* TAG = "main";
static std::atomic<bool> g_quit{false};

static void onSignal(int) { g_quit.store(true); }

static void printUsage() {
    std::printf(
        "lsfg-cap - capture a window and re-present it (frame generation soon)\n"
        "\n"
        "usage: lsfg-cap [options]\n"
        "  -m, --multiplier N       frame-gen multiplier (parsed; active in milestone 3)\n"
        "  -f, --fullscreen         start fullscreen (F toggles at runtime)\n"
        "      --present-mode M     fifo (vsync, default) | mailbox | immediate\n"
        "      --drm-test           run the black-frame test and exit with a verdict\n"
        "      --drm-test-seconds S sampling duration for --drm-test (default 12)\n"
        "      --no-dmabuf          force the SHM (CPU copy) capture path\n"
        "      --no-restore         always show the window picker\n"
        "      --validate           enable Vulkan validation layers\n"
        "  -v, --verbose            debug logging\n"
        "  -h, --help               this text\n"
        "\n"
        "keys: F fullscreen | Esc/Q quit\n");
}

static std::filesystem::path configDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base =
        xdg && *xdg ? std::filesystem::path(xdg)
                    : std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".") /
                          ".config";
    return base / "lsfg-cap";
}

static std::string loadRestoreToken() {
    std::ifstream f(configDir() / "restore_token");
    std::string token;
    if (f)
        std::getline(f, token);
    return token;
}

static void saveRestoreToken(const std::string& token) {
    if (token.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(configDir(), ec);
    std::ofstream f(configDir() / "restore_token", std::ios::trunc);
    f << token << "\n";
}

static bool parseArgs(int argc, char** argv, Options& opts) {
    static const option long_opts[] = {
        {"multiplier", required_argument, nullptr, 'm'},
        {"fullscreen", no_argument, nullptr, 'f'},
        {"present-mode", required_argument, nullptr, 1000},
        {"drm-test", no_argument, nullptr, 1001},
        {"drm-test-seconds", required_argument, nullptr, 1002},
        {"no-dmabuf", no_argument, nullptr, 1003},
        {"no-restore", no_argument, nullptr, 1004},
        {"validate", no_argument, nullptr, 1005},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "m:fvh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'm': opts.multiplier = std::atoi(optarg); break;
        case 'f': opts.fullscreen = true; break;
        case 'v': opts.verbose = true; break;
        case 1000: opts.present_mode = optarg; break;
        case 1001: opts.drm_test = true; break;
        case 1002: opts.drm_test_seconds = std::atof(optarg); break;
        case 1003: opts.no_dmabuf = true; break;
        case 1004: opts.no_restore = true; break;
        case 1005: opts.validate = true; break;
        case 'h': printUsage(); return false;
        default: printUsage(); return false;
        }
    }
    return true;
}

static int run(const Options& opts) {
    vk::Context ctx;
    if (!ctx.init(opts))
        return 1;

    vk::FramePool pool;
    Renderer renderer;
    if (!renderer.init(ctx, pool))
        return 1;

    std::atomic<bool> session_closed{false};
    PortalSession portal;
    portal.on_closed = [&] { session_closed.store(true); };
    std::string token = opts.no_restore ? "" : loadRestoreToken();
    if (!portal.begin(token))
        return 1;

    auto pumpEvents = [&](Renderer& r) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                g_quit.store(true);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_Q)
                    g_quit.store(true);
                else if (ev.key.key == SDLK_F) {
                    bool fs = (SDL_GetWindowFlags(ctx.window) &
                               SDL_WINDOW_FULLSCREEN) != 0;
                    SDL_SetWindowFullscreen(ctx.window, !fs);
                }
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
                r.notifyResize();
                break;
            default:
                break;
            }
        }
    };

    // Wait for the user to pick a window in the portal dialog, keeping the
    // output window alive meanwhile.
    while (portal.pump() && !g_quit.load()) {
        pumpEvents(renderer);
        if (!renderer.drawFrame())
            return 1;
    }
    if (g_quit.load())
        return 0;
    if (!portal.succeeded()) {
        logError(TAG, "portal setup failed: %s", portal.error().c_str());
        return 1;
    }
    saveRestoreToken(portal.restoreToken());

    Capture capture;
    if (opts.drm_test)
        capture.setProbeEveryFrame(true);
    if (!capture.start(ctx, pool, portal.stream().pipewire_fd,
                       portal.stream().node_id, !opts.no_dmabuf))
        return 1;

    logInfo(TAG, "passthrough running (multiplier %d requested; frame "
                 "generation lands in milestone 3)",
            opts.multiplier);
    if (opts.drm_test)
        logInfo(TAG, "DRM black-frame test: sampling for %.0f seconds - play "
                     "your protected video now",
                opts.drm_test_seconds);

    double t_start = nowSeconds();
    double t_last_stats = t_start;
    uint64_t last_cap_frames = 0, last_presented = 0;
    bool verdict_logged = false;

    while (!g_quit.load()) {
        pumpEvents(renderer);
        portal.pump();
        if (!renderer.drawFrame())
            break;

        if (session_closed.load()) {
            logWarn(TAG, "capture session ended (source closed or sharing "
                         "stopped) - exiting");
            break;
        }
        if (capture.hasError()) {
            logError(TAG, "capture stream error - exiting");
            break;
        }

        double now = nowSeconds();
        if (now - t_last_stats >= 1.0) {
            double dt = now - t_last_stats;
            uint64_t cf = capture.frameCount();
            uint64_t pf = renderer.presentedFrames();
            logInfo(TAG,
                    "capture %5.1f fps (%s) | present %5.1f fps | "
                    "video delay %5.1f ms | luma last/max %.1f/%.1f",
                    double(cf - last_cap_frames) / dt,
                    capture.usingDmaBuf() ? "dmabuf" : "shm",
                    double(pf - last_presented) / dt, renderer.latencyMs(),
                    capture.lastLuma(), capture.maxLuma());
            last_cap_frames = cf;
            last_presented = pf;
            t_last_stats = now;
        }

        // Black-capture verdict: after enough samples, say so once (or, in
        // --drm-test mode, print the verdict and exit).
        double elapsed = now - t_start;
        bool test_done = opts.drm_test && elapsed >= opts.drm_test_seconds;
        if ((test_done || (!verdict_logged && !opts.drm_test && elapsed > 6.0)) &&
            capture.lumaSamples() >= 3) {
            verdict_logged = true;
            double max_luma = capture.maxLuma();
            if (max_luma < 2.0) {
                logWarn(TAG, "==================================================");
                logWarn(TAG, "VERDICT: captured frames are BLACK (max luma %.2f)",
                        max_luma);
                logWarn(TAG, "The source is very likely DRM-protected at the");
                logWarn(TAG, "compositor level. Try another browser or source.");
                logWarn(TAG, "==================================================");
                if (opts.drm_test)
                    return 2;
            } else {
                logInfo(TAG, "VERDICT: capture is NOT black (max luma %.1f) - "
                             "frame generation is viable",
                        max_luma);
                if (opts.drm_test)
                    return 0;
            }
        }
        if (opts.drm_test && elapsed >= opts.drm_test_seconds + 5.0) {
            logError(TAG, "DRM test: no probe samples arrived (no frames?) - "
                          "capture is not delivering; verdict INCONCLUSIVE");
            return 3;
        }
    }

    capture.stop();
    renderer.destroy();
    pool.destroy();
    ctx.shutdown();
    return 0;
}

} // namespace lsfg

int main(int argc, char** argv) {
    lsfg::Options opts;
    if (!lsfg::parseArgs(argc, argv, opts))
        return 0;
    if (opts.verbose)
        lsfg::g_log_level = lsfg::LogLevel::Debug;
    std::signal(SIGINT, lsfg::onSignal);
    std::signal(SIGTERM, lsfg::onSignal);
    return lsfg::run(opts);
}
