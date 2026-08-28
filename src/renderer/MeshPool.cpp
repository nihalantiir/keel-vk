#include "MeshPool.h"

#include "../keel-vk/DebugUtils.h"
#include "../keel-vk/VkCheck.h"
#include "../keel-vk/VulkanContext.h"

#include <cstring>
#include <stdexcept>

namespace renderer {

namespace {

VkBuffer createMappedBuffer(keel::VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                             VmaAllocation& outAllocation, void*& outMapped, const char* debugName) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};
    keel::vkCheck(
        vmaCreateBuffer(context.allocator(), &bufferInfo, &allocInfo, &buffer, &outAllocation, &allocationInfo),
        "Failed to create buffer");
    outMapped = allocationInfo.pMappedData;

    keel::setDebugObjectName(context.device(), VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer), debugName);
    return buffer;
}

} // namespace

MeshPool::MeshPool(keel::VulkanContext& context, uint32_t vertexCapacity, uint32_t indexCapacity,
                    VkDeviceSize vertexStride)
    : context_(context), vertexStride_(vertexStride), vertexCapacity_(vertexCapacity), indexCapacity_(indexCapacity) {
    vertexBuffer_ = createMappedBuffer(context_, vertexStride_ * vertexCapacity_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                        vertexAllocation_, vertexMapped_, "mesh pool vertex buffer");
    indexBuffer_ = createMappedBuffer(context_, sizeof(uint32_t) * static_cast<VkDeviceSize>(indexCapacity_),
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexAllocation_, indexMapped_,
                                       "mesh pool index buffer");
}

MeshPool::~MeshPool() {
    if (indexBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context_.allocator(), indexBuffer_, indexAllocation_);
    }
    if (vertexBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context_.allocator(), vertexBuffer_, vertexAllocation_);
    }
}

MeshRange MeshPool::allocate(const void* vertices, uint32_t vertexCount, const uint32_t* indices,
                              uint32_t indexCount) {
    if (vertexCursor_ + vertexCount > vertexCapacity_ || indexCursor_ + indexCount > indexCapacity_) {
        throw std::runtime_error("MeshPool: out of capacity");
    }

    std::memcpy(static_cast<uint8_t*>(vertexMapped_) + static_cast<VkDeviceSize>(vertexCursor_) * vertexStride_,
                vertices, static_cast<size_t>(vertexCount) * vertexStride_);
    std::memcpy(static_cast<uint32_t*>(indexMapped_) + indexCursor_, indices, indexCount * sizeof(uint32_t));

    MeshRange range{};
    range.id = MeshId{nextMeshId_++};
    range.vertexOffset = vertexCursor_;
    range.indexOffset = indexCursor_;
    range.indexCount = indexCount;

    vertexCursor_ += vertexCount;
    indexCursor_ += indexCount;
    return range;
}

} // namespace renderer
