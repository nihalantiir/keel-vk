#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>

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

// One vertex buffer, one index buffer, host-visible mapped and written
// once per allocate() call (static geometry only, the same tradeoff
// Renderer's old geometry buffers made - see the wiki's Rendering page).
// A bump allocator: allocate() only ever grows both cursors, nothing
// frees a range back. A real content pipeline needs that eventually;
// nothing built on this template yet does.
class MeshPool {
public:
    MeshPool(keel::VulkanContext& context, uint32_t vertexCapacity, uint32_t indexCapacity, VkDeviceSize vertexStride);
    ~MeshPool();

    MeshPool(const MeshPool&) = delete;
    MeshPool& operator=(const MeshPool&) = delete;

    // vertices must point at vertexCount * vertexStride bytes. Throws if
    // either cursor would exceed the pool's fixed capacity.
    MeshRange allocate(const void* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount);

    VkBuffer vertexBuffer() const { return vertexBuffer_; }
    VkBuffer indexBuffer() const { return indexBuffer_; }

private:
    keel::VulkanContext& context_;
    VkDeviceSize vertexStride_;
    uint32_t vertexCapacity_;
    uint32_t indexCapacity_;
    uint32_t vertexCursor_ = 0;
    uint32_t indexCursor_ = 0;
    uint32_t nextMeshId_ = 0;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation_ = VK_NULL_HANDLE;
    void* vertexMapped_ = nullptr;

    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indexAllocation_ = VK_NULL_HANDLE;
    void* indexMapped_ = nullptr;
};

} // namespace renderer
