#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

namespace keel {
class VulkanContext;
}

namespace renderer {

struct AtlasEntry {
    uint32_t width;
    uint32_t height;
    const void* pixelsRgba8;
};

struct AtlasRect {
    uint32_t x, y, width, height;
    float u0, v0, u1, v1; // normalized, ready to sample
};

// One packed 2D image plus a UV rect table: the "many small, differently
// sized images sharing one descriptor" texture path, meant for UI/glyphs
// later. Contrast with TextureStreamer (arbitrary sizes but one
// descriptor slot per texture) and TextureArray2D (same-size tiles as
// array layers). v1 packer is a dumb shelf packer: entries are placed
// left-to-right, wrapping to a new shelf (row) when a row is full. No
// eviction, no repacking, one page, packed once at construction (not the
// queued streaming path TextureStreamer uses).
class Atlas2D {
public:
    // timelineSemaphore/signalValue: see TextureArray2D's constructor
    // comment - same non-blocking construction-upload pattern.
    Atlas2D(keel::VulkanContext& context, VkCommandPool commandPool, uint32_t pageSize,
            const std::vector<AtlasEntry>& entries, VkSemaphore timelineSemaphore, uint64_t signalValue);
    ~Atlas2D();

    Atlas2D(const Atlas2D&) = delete;
    Atlas2D& operator=(const Atlas2D&) = delete;

    VkImageView view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    const std::vector<AtlasRect>& rects() const { return rects_; }
    uint32_t pageSize() const { return pageSize_; }

private:
    keel::VulkanContext& context_;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t pageSize_;
    std::vector<AtlasRect> rects_;

    // Construction upload is fire-and-forget; see TextureArray2D.h's
    // matching members for why these stay alive until the destructor.
    VkCommandPool uploadCommandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer uploadCommandBuffer_ = VK_NULL_HANDLE;
    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation_ = VK_NULL_HANDLE;
};

} // namespace renderer
