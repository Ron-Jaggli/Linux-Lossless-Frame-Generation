#include "capture.hpp"

#include "log.hpp"
#include "vk/context.hpp"
#include "vk/frame_pool.hpp"

#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lsfg {

static const char* TAG = "capture";
static constexpr uint32_t PROBE_SIZE = 16;
static constexpr uint32_t PROBE_INTERVAL = 30;

struct FormatMap {
    uint32_t spa;
    VkFormat vk;
};
static const FormatMap kFormats[] = {
    {SPA_VIDEO_FORMAT_BGRA, VK_FORMAT_B8G8R8A8_UNORM},
    {SPA_VIDEO_FORMAT_BGRx, VK_FORMAT_B8G8R8A8_UNORM},
    {SPA_VIDEO_FORMAT_RGBA, VK_FORMAT_R8G8B8A8_UNORM},
    {SPA_VIDEO_FORMAT_RGBx, VK_FORMAT_R8G8B8A8_UNORM},
};

static VkFormat vkFormatFromSpa(uint32_t spa) {
    for (const auto& f : kFormats)
        if (f.spa == spa)
            return f.vk;
    return VK_FORMAT_UNDEFINED;
}

// ---------------------------------------------------------------- lifecycle

bool Capture::start(vk::Context& ctx, vk::FramePool& pool, int pipewire_fd,
                    uint32_t node_id, bool allow_dmabuf) {
    ctx_ = &ctx;
    pool_ = &pool;
    allow_dmabuf_ = allow_dmabuf && ctx.dmabuf_supported;

    if (allow_dmabuf_) {
        for (const auto& f : kFormats) {
            if (!modifiers_.count(f.vk))
                modifiers_[f.vk] = ctx.drmModifiersFor(f.vk);
        }
        size_t total = 0;
        for (auto& [fmt, mods] : modifiers_)
            total += mods.size();
        if (total == 0) {
            logWarn(TAG, "no usable DRM modifiers; capture will use SHM");
            allow_dmabuf_ = false;
        } else {
            logInfo(TAG, "offering DMA-BUF with %zu modifiers", total);
        }
    }

    if (!ensureVkResources())
        return false;

    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("lsfg-capture", nullptr);
    pw_ctx_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (pw_thread_loop_start(loop_) != 0) {
        logError(TAG, "failed to start pipewire loop");
        return false;
    }

    pw_thread_loop_lock(loop_);
    int fd = fcntl(pipewire_fd, F_DUPFD_CLOEXEC, 0);
    core_ = pw_context_connect_fd(pw_ctx_, fd, nullptr, 0);
    if (!core_) {
        pw_thread_loop_unlock(loop_);
        logError(TAG, "pw_context_connect_fd failed");
        return false;
    }

    stream_ = pw_stream_new(
        core_, "lsfg-cap",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                          "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));

    static pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = &Capture::onStateChanged;
    events.param_changed = &Capture::onParamChanged;
    events.process = &Capture::onProcess;
    events.remove_buffer = &Capture::onRemoveBuffer;
    pw_stream_add_listener(stream_, &hook_, &events, this);

    uint8_t buf[16384];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    auto params = buildFormatParams(&b, false, 0, 0);

    int res = pw_stream_connect(
        stream_, PW_DIRECTION_INPUT, node_id,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                     PW_STREAM_FLAG_MAP_BUFFERS),
        params.data(), uint32_t(params.size()));
    pw_thread_loop_unlock(loop_);
    if (res < 0) {
        logError(TAG, "pw_stream_connect failed: %d", res);
        return false;
    }
    logInfo(TAG, "connecting to pipewire node %u", node_id);
    return true;
}

void Capture::stop() {
    if (loop_) {
        pw_thread_loop_lock(loop_);
        if (stream_) {
            pw_stream_disconnect(stream_);
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        clearImports();
        if (core_) {
            pw_core_disconnect(core_);
            core_ = nullptr;
        }
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
        if (pw_ctx_) {
            pw_context_destroy(pw_ctx_);
            pw_ctx_ = nullptr;
        }
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
        pw_deinit();
    }
    destroyVkResources();
}

// ------------------------------------------------------------- negotiation

std::vector<const spa_pod*> Capture::buildFormatParams(spa_pod_builder* b,
                                                       bool fixate,
                                                       uint32_t fixate_spa,
                                                       uint64_t fixate_modifier) {
    std::vector<const spa_pod*> pods;
    spa_rectangle def_size = SPA_RECTANGLE(1920, 1080);
    spa_rectangle min_size = SPA_RECTANGLE(1, 1);
    spa_rectangle max_size = SPA_RECTANGLE(16384, 16384);
    spa_fraction def_rate = SPA_FRACTION(60, 1);
    spa_fraction min_rate = SPA_FRACTION(0, 1);
    spa_fraction max_rate = SPA_FRACTION(1000, 1);

    auto push = [&](uint32_t spa_fmt, const uint64_t* mods,
                    size_t n_mods) -> const spa_pod* {
        spa_pod_frame f;
        spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format,
                                    SPA_PARAM_EnumFormat);
        spa_pod_builder_add(b,
                            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_fmt), 0);
        if (n_mods == 1) {
            spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
                                 SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_long(b, int64_t(mods[0]));
        } else if (n_mods > 1) {
            spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
                                 SPA_POD_PROP_FLAG_MANDATORY |
                                     SPA_POD_PROP_FLAG_DONT_FIXATE);
            spa_pod_frame f2;
            spa_pod_builder_push_choice(b, &f2, SPA_CHOICE_Enum, 0);
            spa_pod_builder_long(b, int64_t(mods[0])); // preferred
            for (size_t i = 0; i < n_mods; i++)
                spa_pod_builder_long(b, int64_t(mods[i]));
            spa_pod_builder_pop(b, &f2);
        }
        spa_pod_builder_add(
            b, SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(&def_size, &min_size, &max_size),
            SPA_FORMAT_VIDEO_framerate,
            SPA_POD_CHOICE_RANGE_Fraction(&def_rate, &min_rate, &max_rate), 0);
        return static_cast<const spa_pod*>(spa_pod_builder_pop(b, &f));
    };

    if (fixate)
        pods.push_back(push(fixate_spa, &fixate_modifier, 1));
    if (allow_dmabuf_) {
        for (const auto& f : kFormats) {
            const auto& mods = modifiers_[f.vk];
            if (mods.size() > 1)
                pods.push_back(push(f.spa, mods.data(), mods.size()));
            else if (mods.size() == 1)
                pods.push_back(push(f.spa, mods.data(), 1));
        }
    }
    for (const auto& f : kFormats)
        pods.push_back(push(f.spa, nullptr, 0)); // SHM fallback
    return pods;
}

void Capture::onParamChanged(void* data, uint32_t id, const spa_pod* param) {
    auto* self = static_cast<Capture*>(data);
    if (id == SPA_PARAM_Format)
        self->handleFormatChanged(param);
}

void Capture::handleFormatChanged(const spa_pod* param) {
    if (!param)
        return;

    uint32_t media_type = 0, media_subtype = 0;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
        media_type != SPA_MEDIA_TYPE_video ||
        media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(param, &info) < 0) {
        logError(TAG, "could not parse negotiated video format");
        return;
    }

    const spa_pod_prop* mod_prop =
        spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);

    if (mod_prop && (mod_prop->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)) {
        // Compositor offered several modifiers; we must pick one (fixation).
        uint32_t n_vals = 0, choice = 0;
        const spa_pod* vals_pod =
            spa_pod_get_values(&mod_prop->value, &n_vals, &choice);
        const int64_t* vals =
            static_cast<const int64_t*>(SPA_POD_BODY(vals_pod));
        VkFormat vkfmt = vkFormatFromSpa(info.format);
        const auto& ours = modifiers_[vkfmt];
        uint64_t picked = uint64_t(vals[0]);
        for (uint32_t i = 0; i < n_vals; i++) {
            if (std::find(ours.begin(), ours.end(), uint64_t(vals[i])) !=
                ours.end()) {
                picked = uint64_t(vals[i]);
                break;
            }
        }
        logDebug(TAG, "fixating modifier 0x%016lx (%u offered)",
                 (unsigned long)picked, n_vals);
        uint8_t buf[16384];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        auto params = buildFormatParams(&b, true, info.format, picked);
        pw_stream_update_params(stream_, params.data(), uint32_t(params.size()));
        return;
    }

    // Format is fixated: adopt it.
    vk_format_ = vkFormatFromSpa(info.format);
    if (vk_format_ == VK_FORMAT_UNDEFINED) {
        logError(TAG, "compositor chose unsupported format %u", info.format);
        error_flag_.store(true);
        return;
    }
    spa_format_ = info.format;
    width_ = info.size.width;
    height_ = info.size.height;
    is_dmabuf_ = mod_prop != nullptr;
    modifier_ = info.modifier;
    negotiated_dmabuf_.store(is_dmabuf_);
    import_failed_once_ = false;

    logInfo(TAG, "format: %ux%u spa=%u %s%s rate=%u/%u", width_, height_,
            spa_format_, is_dmabuf_ ? "DMA-BUF" : "SHM",
            is_dmabuf_ ? "" : " (CPU copy path)", info.framerate.num,
            info.framerate.denom);

    clearImports();

    bool pool_ok;
    if (!pool_created_) {
        pool_ok = pool_->create(*ctx_, vk_format_, width_, height_);
        pool_created_ = pool_ok;
    } else {
        pool_ok = pool_->recreate(vk_format_, width_, height_);
    }
    if (!pool_ok) {
        error_flag_.store(true);
        return;
    }

    // Announce buffer requirements + request header metadata (pts).
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    uint32_t data_types = is_dmabuf_
                              ? (1u << SPA_DATA_DmaBuf)
                              : ((1u << SPA_DATA_MemFd) | (1u << SPA_DATA_MemPtr));
    const spa_pod* params[2];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(int(data_types))));
    params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
        SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
        SPA_POD_Int(sizeof(spa_meta_header))));
    pw_stream_update_params(stream_, params, 2);
}

void Capture::fallbackToShm() {
    logWarn(TAG, "DMA-BUF import failed; renegotiating with SHM only");
    allow_dmabuf_ = false;
    clearImports();
    uint8_t buf[16384];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    auto params = buildFormatParams(&b, false, 0, 0);
    pw_stream_update_params(stream_, params.data(), uint32_t(params.size()));
}

void Capture::onStateChanged(void* data, pw_stream_state,
                             pw_stream_state state, const char* error) {
    auto* self = static_cast<Capture*>(data);
    logInfo(TAG, "stream state: %s%s%s", pw_stream_state_as_string(state),
            error ? " - " : "", error ? error : "");
    switch (state) {
    case PW_STREAM_STATE_STREAMING:
        self->streaming_.store(true);
        break;
    case PW_STREAM_STATE_ERROR:
        self->error_flag_.store(true);
        self->streaming_.store(false);
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        // Stream dropped (source window closed, compositor stopped the cast).
        if (self->frames_.load() > 0)
            self->error_flag_.store(true);
        self->streaming_.store(false);
        break;
    default:
        self->streaming_.store(false);
        break;
    }
}

void Capture::onRemoveBuffer(void* data, pw_buffer* buffer) {
    auto* self = static_cast<Capture*>(data);
    auto it = self->imports_.find(buffer);
    if (it != self->imports_.end()) {
        destroyImported(*self->ctx_, it->second);
        self->imports_.erase(it);
    }
}

void Capture::clearImports() {
    for (auto& [buf, img] : imports_)
        destroyImported(*ctx_, img);
    imports_.clear();
}

// ------------------------------------------------------------------ frames

void Capture::onProcess(void* data) {
    static_cast<Capture*>(data)->handleProcess();
}

void Capture::handleProcess() {
    // Drain the queue and keep only the most recent buffer.
    pw_buffer* last = nullptr;
    pw_buffer* b = nullptr;
    while ((b = pw_stream_dequeue_buffer(stream_)) != nullptr) {
        if (last)
            pw_stream_queue_buffer(stream_, last);
        last = b;
    }
    if (!last)
        return;

    spa_buffer* sb = last->buffer;
    spa_data* d0 = &sb->datas[0];
    bool corrupted = d0->chunk && (d0->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED);

    if (!corrupted && width_ > 0 && pool_created_) {
        int idx = pool_->acquireWrite();
        bool ok = d0->type == SPA_DATA_DmaBuf ? processDmaBuf(last, idx)
                                              : processShm(last, idx);
        if (ok) {
            double t_cap = nowSeconds();
            auto* h = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(
                sb, SPA_META_Header, sizeof(spa_meta_header)));
            if (h && h->pts > 0) {
                // pts is CLOCK_MONOTONIC nanoseconds; sanity-check the domain
                double pts_s = double(h->pts) * 1e-9;
                if (std::abs(pts_s - t_cap) < 1.0)
                    t_cap = pts_s;
            }
            uint64_t seq = frames_.fetch_add(1) + 1;
            pool_->publish(idx, seq, t_cap);
            if (probe_pending_)
                readProbe();
        }
    }
    pw_stream_queue_buffer(stream_, last);
}

bool Capture::processDmaBuf(pw_buffer* buf, int write_idx) {
    spa_buffer* sb = buf->buffer;
    auto it = imports_.find(buf);
    if (it == imports_.end()) {
        vk::DmaBufPlane planes[4];
        uint32_t n = std::min(sb->n_datas, 4u);
        for (uint32_t i = 0; i < n; i++) {
            planes[i].fd = int(sb->datas[i].fd);
            planes[i].offset = sb->datas[i].chunk->offset;
            planes[i].stride = uint32_t(sb->datas[i].chunk->stride);
        }
        vk::ImportedImage img;
        std::string err;
        if (!importDmaBuf(*ctx_, vk_format_, width_, height_, modifier_, planes,
                          n, img, err)) {
            logError(TAG, "dmabuf import: %s", err.c_str());
            if (!import_failed_once_) {
                import_failed_once_ = true;
                fallbackToShm();
            }
            return false;
        }
        logDebug(TAG, "imported dmabuf buffer %p (%u planes, mod 0x%016lx)",
                 (void*)buf, n, (unsigned long)modifier_);
        it = imports_.emplace(buf, img).first;
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    uint32_t foreign_qf = ctx_->has_foreign_queue ? VK_QUEUE_FAMILY_FOREIGN_EXT
                                                  : VK_QUEUE_FAMILY_EXTERNAL;

    // Acquire the compositor's image for our queue family.
    VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    acquire.srcAccessMask = 0;
    acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    acquire.srcQueueFamilyIndex = foreign_qf;
    acquire.dstQueueFamilyIndex = ctx_->queue_family;
    acquire.image = it->second.image;
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &acquire);

    bool probe = probe_every_frame_.load() || (frames_.load() % PROBE_INTERVAL) == 0;
    recordPoolBlit(cmd_, it->second.image, write_idx, probe);

    // Release back to the compositor.
    VkImageMemoryBarrier release = acquire;
    release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    release.dstAccessMask = 0;
    release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    release.srcQueueFamilyIndex = ctx_->queue_family;
    release.dstQueueFamilyIndex = foreign_qf;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &release);

    vkEndCommandBuffer(cmd_);
    return submitAndWait(cmd_);
}

bool Capture::processShm(pw_buffer* buf, int write_idx) {
    spa_buffer* sb = buf->buffer;
    spa_data* d = &sb->datas[0];
    auto* src = static_cast<const uint8_t*>(d->data);
    if (!src)
        return false;
    uint32_t stride = d->chunk->stride > 0 ? uint32_t(d->chunk->stride)
                                           : width_ * 4;
    size_t need = size_t(stride) * height_;
    if (!ensureStaging(need))
        return false;
    std::memcpy(staging_map_, src + d->chunk->offset, need);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    VkImage dst = pool_->image(write_idx);
    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = dst;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_dst);

    VkBufferImageCopy region{};
    region.bufferRowLength = stride / 4;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, height_, 1};
    vkCmdCopyBufferToImage(cmd_, staging_, dst,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    bool probe = probe_every_frame_.load() || (frames_.load() % PROBE_INTERVAL) == 0;
    // recordPoolBlit handles the DST->SRC transition + probe; reuse it with
    // src == dst is wrong, so inline the tail here.
    VkImageMemoryBarrier to_src = to_dst;
    to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_src);
    if (probe)
        recordProbeFromPool(cmd_, dst);

    vkEndCommandBuffer(cmd_);
    return submitAndWait(cmd_);
}

// Blits an already-transfer-src-ready source image into the pool slot and
// optionally records the luminance probe.
void Capture::recordPoolBlit(VkCommandBuffer cmd, VkImage src, int write_idx,
                             bool probe) {
    VkImage dst = pool_->image(write_idx);

    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = dst;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_dst);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(width_), int32_t(height_), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = {int32_t(width_), int32_t(height_), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_NEAREST);

    VkImageMemoryBarrier to_src = to_dst;
    to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_src);

    if (probe)
        recordProbeFromPool(cmd, dst);
}

void Capture::recordProbeFromPool(VkCommandBuffer cmd, VkImage pool_img) {
    VkImageMemoryBarrier pb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    pb.srcAccessMask = 0;
    pb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pb.image = probe_image_;
    pb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &pb);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(width_), int32_t(height_), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = {int32_t(PROBE_SIZE), int32_t(PROBE_SIZE), 1};
    vkCmdBlitImage(cmd, pool_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   probe_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    VkImageMemoryBarrier pb2 = pb;
    pb2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pb2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pb2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pb2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &pb2);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {PROBE_SIZE, PROBE_SIZE, 1};
    vkCmdCopyImageToBuffer(cmd, probe_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probe_buf_, 1,
                           &region);

    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                         nullptr);
    probe_pending_ = true;
}

void Capture::readProbe() {
    probe_pending_ = false;
    const auto* px = static_cast<const uint8_t*>(probe_map_);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < PROBE_SIZE * PROBE_SIZE; i++) {
        // mean of the three color bytes; channel order irrelevant
        sum += px[i * 4 + 0] + px[i * 4 + 1] + px[i * 4 + 2];
    }
    double luma = double(sum) / (PROBE_SIZE * PROBE_SIZE * 3);
    last_luma_.store(luma);
    double prev = max_luma_.load();
    if (luma > prev)
        max_luma_.store(luma);
    luma_samples_.fetch_add(1);
}

bool Capture::submitAndWait(VkCommandBuffer cmd) {
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    {
        std::lock_guard lock(ctx_->queue_mutex);
        if (vkQueueSubmit(ctx_->queue, 1, &si, fence_) != VK_SUCCESS) {
            logError(TAG, "vkQueueSubmit failed on capture thread");
            error_flag_.store(true);
            return false;
        }
    }
    vkWaitForFences(ctx_->device, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx_->device, 1, &fence_);
    return true;
}

// --------------------------------------------------------------- resources

bool Capture::ensureVkResources() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx_->queue_family;
    if (vkCreateCommandPool(ctx_->device, &pci, nullptr, &cmd_pool_) !=
        VK_SUCCESS)
        return false;
    VkCommandBufferAllocateInfo cai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmd_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(ctx_->device, &cai, &cmd_) != VK_SUCCESS)
        return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(ctx_->device, &fci, nullptr, &fence_) != VK_SUCCESS)
        return false;

    // probe image + readback buffer
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {PROBE_SIZE, PROBE_SIZE, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx_->device, &ici, nullptr, &probe_image_) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx_->device, probe_image_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        ctx_->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx_->device, &mai, nullptr, &probe_image_mem_) !=
            VK_SUCCESS ||
        vkBindImageMemory(ctx_->device, probe_image_, probe_image_mem_, 0) !=
            VK_SUCCESS)
        return false;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = PROBE_SIZE * PROBE_SIZE * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(ctx_->device, &bci, nullptr, &probe_buf_) != VK_SUCCESS)
        return false;
    vkGetBufferMemoryRequirements(ctx_->device, probe_buf_, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx_->findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(ctx_->device, &mai, nullptr, &probe_buf_mem_) !=
            VK_SUCCESS ||
        vkBindBufferMemory(ctx_->device, probe_buf_, probe_buf_mem_, 0) !=
            VK_SUCCESS)
        return false;
    if (vkMapMemory(ctx_->device, probe_buf_mem_, 0, VK_WHOLE_SIZE, 0,
                    &probe_map_) != VK_SUCCESS)
        return false;
    return true;
}

bool Capture::ensureStaging(size_t bytes) {
    if (staging_size_ >= bytes)
        return true;
    if (staging_) {
        vkDestroyBuffer(ctx_->device, staging_, nullptr);
        vkFreeMemory(ctx_->device, staging_mem_, nullptr);
        staging_ = VK_NULL_HANDLE;
        staging_mem_ = VK_NULL_HANDLE;
        staging_map_ = nullptr;
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(ctx_->device, &bci, nullptr, &staging_) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx_->device, staging_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx_->findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(ctx_->device, &mai, nullptr, &staging_mem_) !=
            VK_SUCCESS ||
        vkBindBufferMemory(ctx_->device, staging_, staging_mem_, 0) !=
            VK_SUCCESS ||
        vkMapMemory(ctx_->device, staging_mem_, 0, VK_WHOLE_SIZE, 0,
                    &staging_map_) != VK_SUCCESS)
        return false;
    staging_size_ = bytes;
    return true;
}

void Capture::destroyVkResources() {
    if (!ctx_ || !ctx_->device)
        return;
    vkDeviceWaitIdle(ctx_->device);
    clearImports();
    if (staging_)
        vkDestroyBuffer(ctx_->device, staging_, nullptr);
    if (staging_mem_)
        vkFreeMemory(ctx_->device, staging_mem_, nullptr);
    if (probe_buf_)
        vkDestroyBuffer(ctx_->device, probe_buf_, nullptr);
    if (probe_buf_mem_)
        vkFreeMemory(ctx_->device, probe_buf_mem_, nullptr);
    if (probe_image_)
        vkDestroyImage(ctx_->device, probe_image_, nullptr);
    if (probe_image_mem_)
        vkFreeMemory(ctx_->device, probe_image_mem_, nullptr);
    if (fence_)
        vkDestroyFence(ctx_->device, fence_, nullptr);
    if (cmd_pool_)
        vkDestroyCommandPool(ctx_->device, cmd_pool_, nullptr);
    staging_ = VK_NULL_HANDLE;
    staging_mem_ = VK_NULL_HANDLE;
    probe_buf_ = VK_NULL_HANDLE;
    probe_buf_mem_ = VK_NULL_HANDLE;
    probe_image_ = VK_NULL_HANDLE;
    probe_image_mem_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    cmd_pool_ = VK_NULL_HANDLE;
}

} // namespace lsfg
