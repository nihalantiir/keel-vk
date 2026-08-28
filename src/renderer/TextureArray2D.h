#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>

namespace keel {
class VulkanContext;
}

namespace renderer {

// One VkImage with arrayLayers > 1: the "many same-size tiles, one image,
// one descriptor" texture path. Contrast with TextureStreamer (arbitrary
// sizes, one descriptor slot per texture, runtime allocate/update/free)
// and Atlas2D (one packed image, UV rects instead of layers). Useful
// later for things like terrain splat layers or sprite-sheet animation
// frames sampled with sampler2DArray. Populated once at construction
// (a one-shot upload, not the queued streaming path); nothing samples it
// yet, it exists to prove the path works.
class TextureArray2D {
public:
    TextureArray2D(keel::VulkanContext& context, VkCommandPool commandPool, uint32_t tileSize, uint32_t layerCount);
    ~TextureArray2D();

    TextureArray2D(const TextureArray2D&) = delete;
    TextureArray2D& operator=(const TextureArray2D&) = delete;

    VkImageView view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    uint32_t layerCount() const { return layerCount_; }
    uint32_t tileSize() const { return tileSize_; }

private:
    keel::VulkanContext& context_;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t tileSize_;
    uint32_t layerCount_;
};

} // namespace renderer
