#include "MeshPool.h"

#include "../keel-vk/DebugUtils.h"
#include "../keel-vk/VkCheck.h"
#include "../keel-vk/VulkanContext.h"

#include <cstring>
#include <stdexcept>

namespace renderer {

namespace {

VkBuffer createDeviceLocalBuffer(keel::VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                                  VmaAllocation& outAllocation, const char* debugName) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    // EXCLUSIVE: this upload and the graphics queue that reads it are the
    // same queue family (see the wiki's Rendering page for why 0.6.0
    // dropped the dedicated transfer queue), so no ownership transfer.
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkBuffer buffer = VK_NULL_HANDLE;
    keel::vkCheck(vmaCreateBuffer(context.allocator(), &bufferInfo, &allocInfo, &buffer, &outAllocation, nullptr),
                  "Failed to create mesh pool buffer");
    keel::setDebugObjectName(context.device(), VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer), debugName);
    return buffer;
}

VkBuffer createMappedStagingBuffer(keel::VulkanContext& context, VkDeviceSize size, VmaAllocation& outAllocation,
                                    void*& outMapped) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};
    keel::vkCheck(
        vmaCreateBuffer(context.allocator(), &bufferInfo, &allocInfo, &buffer, &outAllocation, &allocationInfo),
        "Failed to create mesh pool staging buffer");
    outMapped = allocationInfo.pMappedData;
    keel::setDebugObjectName(context.device(), VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer),
                              "mesh pool staging buffer");
    return buffer;
}

} // namespace

MeshPool::MeshPool(keel::VulkanContext& context, VkCommandPool uploadCommandPool, VkSemaphore timelineSemaphore,
                    uint64_t firstSignalValue, uint32_t vertexCapacity, uint32_t indexCapacity,
                    VkDeviceSize vertexStride)
    : context_(context), uploadCommandPool_(uploadCommandPool), timelineSemaphore_(timelineSemaphore),
      nextSignalValue_(firstSignalValue), vertexStride_(vertexStride), vertexCapacity_(vertexCapacity),
      indexCapacity_(indexCapacity) {
    vertexBuffer_ = createDeviceLocalBuffer(context_, vertexStride_ * vertexCapacity_,
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexAllocation_,
                                             "mesh pool vertex buffer");
    indexBuffer_ = createDeviceLocalBuffer(context_, sizeof(uint32_t) * static_cast<VkDeviceSize>(indexCapacity_),
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexAllocation_,
                                            "mesh pool index buffer");
}

MeshPool::~MeshPool() {
    // Only reached after ~Renderer's vkDeviceWaitIdle, so every pending
    // upload below is long since complete - safe to free unconditionally,
    // same as TextureArray2D/Atlas2D's construction uploads.
    for (const PendingUpload& pending : pendingUploads_) {
        vkFreeCommandBuffers(context_.device(), uploadCommandPool_, 1, &pending.cmd);
        vmaDestroyBuffer(context_.allocator(), pending.stagingBuffer, pending.stagingAllocation);
    }
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

    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertexCount) * vertexStride_;
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t);

    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    VkBuffer stagingBuffer = createMappedStagingBuffer(context_, vertexBytes + indexBytes, stagingAllocation,
                                                          stagingMapped);
    std::memcpy(stagingMapped, vertices, static_cast<size_t>(vertexBytes));
    std::memcpy(static_cast<uint8_t*>(stagingMapped) + vertexBytes, indices, static_cast<size_t>(indexBytes));

    VkCommandBufferAllocateInfo cmdAllocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAllocInfo.commandPool = uploadCommandPool_;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    keel::vkCheck(vkAllocateCommandBuffers(context_.device(), &cmdAllocInfo, &cmd),
                  "Failed to allocate mesh pool upload command buffer");

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    keel::vkCheck(vkBeginCommandBuffer(cmd, &beginInfo), "Failed to begin mesh pool upload command buffer");

    VkBufferCopy vertexCopy{};
    vertexCopy.srcOffset = 0;
    vertexCopy.dstOffset = static_cast<VkDeviceSize>(vertexCursor_) * vertexStride_;
    vertexCopy.size = vertexBytes;
    vkCmdCopyBuffer(cmd, stagingBuffer, vertexBuffer_, 1, &vertexCopy);

    VkBufferCopy indexCopy{};
    indexCopy.srcOffset = vertexBytes;
    indexCopy.dstOffset = static_cast<VkDeviceSize>(indexCursor_) * sizeof(uint32_t);
    indexCopy.size = indexBytes;
    vkCmdCopyBuffer(cmd, stagingBuffer, indexBuffer_, 1, &indexCopy);

    keel::vkCheck(vkEndCommandBuffer(cmd), "Failed to end mesh pool upload command buffer");

    // No pipeline barrier after the copy: a plain buffer copy needs no
    // layout transition, and the timeline semaphore signal below already
    // orders "copy visible" before whatever waits on it (sync2 semaphore
    // signal/wait carries a memory dependency scoped by stageMask, the
    // same mechanism the queue-family CONCURRENT choice above relies on).
    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = timelineSemaphore_;
    signalInfo.value = nextSignalValue_;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ++nextSignalValue_;

    VkCommandBufferSubmitInfo cmdSubmitInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdSubmitInfo.commandBuffer = cmd;
    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdSubmitInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalInfo;
    keel::vkCheck(vkQueueSubmit2(context_.uploadQueue(), 1, &submitInfo, VK_NULL_HANDLE),
                  "Failed to submit mesh pool upload");

    pendingUploads_.push_back({stagingBuffer, stagingAllocation, cmd});

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
