#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

namespace keel {
class VulkanContext;
}

namespace renderer {

struct MeshId {
    uint32_t value = 0;
};

struct MeshRange {
    MeshId id;
    uint32_t vertexOffset = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
};

// One DEVICE_LOCAL vertex buffer, one DEVICE_LOCAL index buffer, both
// written through a per-call staging buffer + vkCmdCopyBuffer rather than
// host-visible mapped memory. A bump allocator: allocate() only ever
// grows both cursors, nothing frees a range back. A real content pipeline
// needs that eventually; nothing built on this template yet does.
class MeshPool {
public:
    // uploadCommandPool: bound to VulkanContext::uploadQueueFamily(), same
    // pool Renderer's other construction-time uploads use. timelineSemaphore:
    // shared with those same uploads (see Renderer::createUploadTimelineSemaphore);
    // each allocate() call signals it to a fresh, strictly increasing
    // value instead of blocking the CPU. Caller is responsible for
    // waiting the timeline up to the highest value ever signaled before
    // anything might read the ranges those calls wrote.
    MeshPool(keel::VulkanContext& context, VkCommandPool uploadCommandPool, VkSemaphore timelineSemaphore,
             uint64_t firstSignalValue, uint32_t vertexCapacity, uint32_t indexCapacity, VkDeviceSize vertexStride);
    ~MeshPool();

    MeshPool(const MeshPool&) = delete;
    MeshPool& operator=(const MeshPool&) = delete;

    // vertices must point at vertexCount * vertexStride bytes. Throws if
    // either cursor would exceed the pool's fixed capacity. Never blocks:
    // records a one-shot staging copy and returns after submitting it,
    // signaling the timeline semaphore given at construction.
    MeshRange allocate(const void* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount);

    // The highest timeline value any allocate() call so far has signaled.
    // Starts at firstSignalValue - 1 (nothing signaled yet) if allocate()
    // is never called.
    uint64_t lastSignaledUploadValue() const { return nextSignalValue_ - 1; }

    VkBuffer vertexBuffer() const { return vertexBuffer_; }
    VkBuffer indexBuffer() const { return indexBuffer_; }

private:
    keel::VulkanContext& context_;
    VkCommandPool uploadCommandPool_;
    VkSemaphore timelineSemaphore_;
    uint64_t nextSignalValue_;

    VkDeviceSize vertexStride_;
    uint32_t vertexCapacity_;
    uint32_t indexCapacity_;
    uint32_t vertexCursor_ = 0;
    uint32_t indexCursor_ = 0;
    uint32_t nextMeshId_ = 0;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation_ = VK_NULL_HANDLE;

    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indexAllocation_ = VK_NULL_HANDLE;

    // Every allocate() call's staging buffer and one-shot command buffer,
    // kept alive until ~MeshPool instead of freed right after submission
    // (there's no CPU wait here to know sooner when it's safe). Safe
    // because ~MeshPool only ever runs after ~Renderer's vkDeviceWaitIdle
    // has already guaranteed the GPU is idle - the same tradeoff
    // TextureArray2D/Atlas2D made for their own construction uploads.
    struct PendingUpload {
        VkBuffer stagingBuffer;
        VmaAllocation stagingAllocation;
        VkCommandBuffer cmd;
    };
    std::vector<PendingUpload> pendingUploads_;
};

} // namespace renderer
