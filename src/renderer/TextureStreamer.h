#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <vector>

namespace keel {
class VulkanContext;
}

namespace renderer {

// A handle to a bindless texture slot. generation distinguishes this
// allocation from whatever a freed/reused slot holds later.
struct TextureHandle {
    uint32_t slot = 0;
    uint32_t generation = 0;
};

// Bindless sampled-image array: arbitrary-size 2D textures, one per slot,
// each independently allocated/freed. Slot 0 is a permanently resident
// 1x1 white default so an unset bindless index always samples something
// defined.
//
// This is the "arbitrary sizes, sparse occupancy" texture path. See
// TextureArray2D for the "many same-size tiles, one image" path and
// Atlas2D for the "one packed image, UV rects" path - the three exist for
// different access patterns, not because one supersedes another.
//
// Uploads are queued, not immediate: allocate()/update() stage pixel data
// into a small ring of persistently-mapped buffers and record nothing
// themselves. processUploads() (called once per frame, before anything
// that might sample a just-allocated slot) records the actual copy,
// layout transition, and descriptor write. Nothing on this path calls
// vkQueueWaitIdle; that only happens once, at startup, outside this class
// (see Renderer::createTextureStreamer).
class TextureStreamer {
public:
    TextureStreamer(keel::VulkanContext& context, VkCommandPool commandPool, uint32_t capacity,
                     uint32_t framesInFlight);
    ~TextureStreamer();

    TextureStreamer(const TextureStreamer&) = delete;
    TextureStreamer& operator=(const TextureStreamer&) = delete;

    // Queues an upload into a newly claimed slot. The slot is reserved
    // immediately; its descriptor and pixel content land on the next
    // processUploads() call.
    TextureHandle allocate(uint32_t width, uint32_t height, const void* pixelsRgba8, const char* debugName);

    // Replaces a slot's content in place: same slot index, new image, new
    // generation. The old image is only destroyed once it's safe (see the
    // frame-in-flight retirement in freeSlotResources).
    void update(TextureHandle& handle, uint32_t width, uint32_t height, const void* pixelsRgba8,
                const char* debugName);

    // No-op on a stale handle (wrong generation) or the default slot.
    void free(TextureHandle handle);

    // Records queued copies/barriers/descriptor writes into cmd and
    // retires any resources whose deferred-destroy delay has elapsed.
    // Call once per frame, before anything that might sample a slot.
    void processUploads(VkCommandBuffer cmd);

    VkDescriptorSetLayout descriptorSetLayout() const { return descriptorSetLayout_; }
    VkDescriptorSet descriptorSet() const { return descriptorSet_; }
    uint32_t capacity() const { return capacity_; }
    uint32_t usedSlots() const;

private:
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t generation = 0;
        bool inUse = false;
    };

    struct StagingRingEntry {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        bool busy = false;
        uint64_t retireAtFrame = 0;
    };

    struct PendingUpload {
        uint32_t slot;
        uint32_t generation;
        uint32_t width;
        uint32_t height;
        uint32_t ringIndex;
    };

    struct DeferredDestroy {
        VkImage image;
        VmaAllocation allocation;
        VkImageView view;
        uint64_t retireAtFrame;
    };

    void createSampler();
    void createDescriptorObjects();
    void reserveDefaultSlot();
    void createSlotImage(uint32_t slotIndex, uint32_t width, uint32_t height, const char* debugName);
    void freeSlotResources(uint32_t slotIndex);
    uint32_t claimStagingRingEntry(VkDeviceSize sizeBytes);
    void reclaimStagingRing();
    void reclaimDeferredDestroys();
    void queueUpload(uint32_t slotIndex, uint32_t generation, uint32_t width, uint32_t height, const void* pixels);
    void updateDescriptorSlot(uint32_t slotIndex, VkImageView view);
    uint32_t findFreeSlotIndex() const;

    keel::VulkanContext& context_;
    VkCommandPool commandPool_;
    uint32_t capacity_;
    uint32_t framesInFlight_;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    std::vector<Slot> slots_;

    // Must comfortably exceed the largest batch of allocate()/update() calls
    // made before the next processUploads() flush: Renderer's startup batch
    // (default + checker + stripes + gradient + spare = 5) is the biggest
    // one today, and nothing reclaims a ring entry until a flush happens.
    static constexpr uint32_t kStagingRingSize = 8;
    static constexpr VkDeviceSize kStagingEntryBytes = 128 * 128 * 4; // covers every demo texture with room to spare
    std::array<StagingRingEntry, kStagingRingSize> stagingRing_;

    std::vector<PendingUpload> pendingUploads_;
    std::vector<DeferredDestroy> deferredDestroys_;
    uint64_t frameCounter_ = 0;
};

} // namespace renderer
