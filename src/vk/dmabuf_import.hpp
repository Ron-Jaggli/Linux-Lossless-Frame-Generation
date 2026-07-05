#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace lsfg::vk {

class Context;

struct DmaBufPlane {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t stride = 0;
};

struct ImportedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
};

// Imports a PipeWire DMA-BUF frame as a VkImage with an explicit DRM format
// modifier. The caller keeps ownership of the plane fds (Vulkan gets a dup).
// All planes are assumed to alias one buffer object (plane 0's fd is
// imported), which holds for every compositor screencast implementation.
bool importDmaBuf(Context& ctx, VkFormat format, uint32_t width, uint32_t height,
                  uint64_t modifier, const DmaBufPlane* planes, uint32_t n_planes,
                  ImportedImage& out, std::string& error);

void destroyImported(Context& ctx, ImportedImage& img);

} // namespace lsfg::vk
