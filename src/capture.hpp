#pragma once

#include "vk/dmabuf_import.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <vulkan/vulkan.h>

namespace lsfg {

namespace vk {
class Context;
class FramePool;
}

// PipeWire screencast consumer. Frame handling runs on PipeWire's loop
// thread; every arriving frame is copied (DMA-BUF import + GPU blit, or SHM
// staging upload) into the shared FramePool and fence-waited before the
// PipeWire buffer is requeued. A 16x16 downscale probe measures mean
// luminance for the DRM-black-frame test and general diagnostics.
class Capture {
public:
    bool start(vk::Context& ctx, vk::FramePool& pool, int pipewire_fd,
               uint32_t node_id, bool allow_dmabuf);
    void stop();

    // polled by the main thread
    uint64_t frameCount() const { return frames_.load(); }
    bool hasError() const { return error_flag_.load(); }
    bool isStreaming() const { return streaming_.load(); }
    double lastLuma() const { return last_luma_.load(); }
    double maxLuma() const { return max_luma_.load(); }
    uint64_t lumaSamples() const { return luma_samples_.load(); }
    bool usingDmaBuf() const { return negotiated_dmabuf_.load(); }
    // probe every frame (DRM test) instead of every 30th
    void setProbeEveryFrame(bool v) { probe_every_frame_.store(v); }

private:
    // PipeWire callbacks (run on the PipeWire loop thread)
    static void onStateChanged(void* data, pw_stream_state old_state,
                               pw_stream_state state, const char* error);
    static void onParamChanged(void* data, uint32_t id, const spa_pod* param);
    static void onProcess(void* data);
    static void onRemoveBuffer(void* data, pw_buffer* buffer);

    void handleFormatChanged(const spa_pod* param);
    void handleProcess();
    bool processDmaBuf(pw_buffer* buf, int write_idx);
    bool processShm(pw_buffer* buf, int write_idx);
    void recordPoolBlit(VkCommandBuffer cmd, VkImage src, int write_idx,
                        bool probe);
    void recordProbeFromPool(VkCommandBuffer cmd, VkImage pool_img);
    bool submitAndWait(VkCommandBuffer cmd);
    void readProbe();
    void fallbackToShm();

    std::vector<const spa_pod*> buildFormatParams(spa_pod_builder* b,
                                                  bool fixate,
                                                  uint32_t fixate_spa,
                                                  uint64_t fixate_modifier);

    bool ensureVkResources();
    bool ensureStaging(size_t bytes);
    void destroyVkResources();
    void clearImports();

    // pipewire
    pw_thread_loop* loop_ = nullptr;
    pw_context* pw_ctx_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook hook_{};
    bool allow_dmabuf_ = true;
    std::map<VkFormat, std::vector<uint64_t>> modifiers_; // queried at start

    // vulkan resources (used only on the capture thread after start)
    vk::Context* ctx_ = nullptr;
    vk::FramePool* pool_ = nullptr;
    bool pool_created_ = false;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
    void* staging_map_ = nullptr;
    size_t staging_size_ = 0;
    VkImage probe_image_ = VK_NULL_HANDLE;
    VkDeviceMemory probe_image_mem_ = VK_NULL_HANDLE;
    VkBuffer probe_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory probe_buf_mem_ = VK_NULL_HANDLE;
    void* probe_map_ = nullptr;

    std::unordered_map<pw_buffer*, vk::ImportedImage> imports_;

    // negotiated format (capture thread only)
    VkFormat vk_format_ = VK_FORMAT_UNDEFINED;
    uint32_t spa_format_ = 0;
    uint32_t width_ = 0, height_ = 0;
    bool is_dmabuf_ = false;
    uint64_t modifier_ = 0;
    bool import_failed_once_ = false;

    // shared state
    std::atomic<uint64_t> frames_{0};
    std::atomic<bool> error_flag_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<bool> negotiated_dmabuf_{false};
    std::atomic<double> last_luma_{-1.0};
    std::atomic<double> max_luma_{-1.0};
    std::atomic<uint64_t> luma_samples_{0};
    std::atomic<bool> probe_every_frame_{false};
    bool probe_pending_ = false;
};

} // namespace lsfg
