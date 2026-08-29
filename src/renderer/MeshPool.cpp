#include "MeshPool.h"

#include "../keel-vk/DebugUtils.h"
#include "../keel-vk/VkCheck.h"
#include "../keel-vk/VulkanContext.h"

#include <algorithm>
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

VkCommandBuffer beginOneShotCommandBuffer(keel::VulkanContext& context, VkCommandPool pool) {
    VkCommandBufferAllocateInfo cmdAllocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAllocInfo.commandPool = pool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    keel::vkCheck(vkAllocateCommandBuffers(context.device(), &cmdAllocInfo, &cmd),
                  "Failed to allocate mesh pool command buffer");

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    keel::vkCheck(vkBeginCommandBuffer(cmd, &beginInfo), "Failed to begin mesh pool command buffer");
    return cmd;
}

} // namespace

MeshPool::MeshPool(keel::VulkanContext& context, VkCommandPool uploadCommandPool, VkSemaphore timelineSemaphore,
                    uint64_t firstSignalValue, uint32_t vertexCapacity, uint32_t indexCapacity,
                    VkDeviceSize vertexStride, uint32_t framesInFlight)
    : context_(context), uploadCommandPool_(uploadCommandPool), timelineSemaphore_(timelineSemaphore),
      nextSignalValue_(firstSignalValue), framesInFlight_(framesInFlight), vertexStride_(vertexStride),
      vertexCapacity_(vertexCapacity), indexCapacity_(indexCapacity) {
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

void MeshPool::insertFreeBlock(std::vector<FreeBlock>& freeList, FreeBlock block) {
    // Kept sorted by offset so a neighbor merge only ever has to look at
    // the insertion point's immediate predecessor/successor.
    auto it = std::lower_bound(freeList.begin(), freeList.end(), block,
                                [](const FreeBlock& a, const FreeBlock& b) { return a.offset < b.offset; });
    it = freeList.insert(it, block);

    if (it + 1 != freeList.end() && it->offset + it->count == (it + 1)->offset) {
        it->count += (it + 1)->count;
        freeList.erase(it + 1);
    }
    if (it != freeList.begin()) {
        auto prev = it - 1;
        if (prev->offset + prev->count == it->offset) {
            prev->count += it->count;
            freeList.erase(it);
        }
    }
}

bool MeshPool::allocateFromFreeList(std::vector<FreeBlock>& freeList, uint32_t count, uint32_t& outOffset) {
    for (auto it = freeList.begin(); it != freeList.end(); ++it) {
        if (it->count < count) {
            continue;
        }
        outOffset = it->offset;
        if (it->count == count) {
            freeList.erase(it);
        } else {
            it->offset += count;
            it->count -= count;
        }
        return true;
    }
    return false;
}

VkDeviceSize MeshPool::freeListBytesOf(const std::vector<FreeBlock>& freeList, VkDeviceSize stride) {
    uint32_t total = 0;
    for (const FreeBlock& block : freeList) {
        total += block.count;
    }
    return static_cast<VkDeviceSize>(total) * stride;
}

VkDeviceSize MeshPool::freeListBytes() const {
    return freeListBytesOf(vertexFreeList_, vertexStride_) +
           freeListBytesOf(indexFreeList_, sizeof(uint32_t));
}

MeshRange MeshPool::allocate(const void* vertices, uint32_t vertexCount, const uint32_t* indices,
                              uint32_t indexCount) {
    uint32_t vertexOffset = 0;
    if (!allocateFromFreeList(vertexFreeList_, vertexCount, vertexOffset)) {
        if (vertexCursor_ + vertexCount > vertexCapacity_) {
            throw std::runtime_error("MeshPool: out of capacity");
        }
        vertexOffset = vertexCursor_;
        vertexCursor_ += vertexCount;
    }

    uint32_t indexOffset = 0;
    if (!allocateFromFreeList(indexFreeList_, indexCount, indexOffset)) {
        if (indexCursor_ + indexCount > indexCapacity_) {
            throw std::runtime_error("MeshPool: out of capacity");
        }
        indexOffset = indexCursor_;
        indexCursor_ += indexCount;
    }

    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertexCount) * vertexStride_;
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t);

    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    VkBuffer stagingBuffer = createMappedStagingBuffer(context_, vertexBytes + indexBytes, stagingAllocation,
                                                          stagingMapped);
    std::memcpy(stagingMapped, vertices, static_cast<size_t>(vertexBytes));
    std::memcpy(static_cast<uint8_t*>(stagingMapped) + vertexBytes, indices, static_cast<size_t>(indexBytes));

    VkCommandBuffer cmd = beginOneShotCommandBuffer(context_, uploadCommandPool_);

    VkBufferCopy vertexCopy{};
    vertexCopy.srcOffset = 0;
    vertexCopy.dstOffset = static_cast<VkDeviceSize>(vertexOffset) * vertexStride_;
    vertexCopy.size = vertexBytes;
    vkCmdCopyBuffer(cmd, stagingBuffer, vertexBuffer_, 1, &vertexCopy);

    VkBufferCopy indexCopy{};
    indexCopy.srcOffset = vertexBytes;
    indexCopy.dstOffset = static_cast<VkDeviceSize>(indexOffset) * sizeof(uint32_t);
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
    range.vertexOffset = vertexOffset;
    range.vertexCount = vertexCount;
    range.indexOffset = indexOffset;
    range.indexCount = indexCount;

    liveRanges_.emplace(range.id.value, range);
    return range;
}

void MeshPool::free(MeshId id) {
    auto it = liveRanges_.find(id.value);
    if (it == liveRanges_.end()) {
        return;
    }
    const MeshRange range = it->second;
    liveRanges_.erase(it);

    const uint64_t retireAtFrame = frameCounter_ + framesInFlight_;
    vertexPendingFree_.push_back({range.vertexOffset, range.vertexCount, retireAtFrame});
    indexPendingFree_.push_back({range.indexOffset, range.indexCount, retireAtFrame});
}

MeshRange MeshPool::resolve(MeshId id) const {
    auto it = liveRanges_.find(id.value);
    if (it == liveRanges_.end()) {
        throw std::runtime_error("MeshPool: resolve() called with an unknown or freed MeshId");
    }
    return it->second;
}

void MeshPool::beginFrame() {
    ++frameCounter_;

    auto reclaim = [this](std::vector<PendingFree>& pending, std::vector<FreeBlock>& freeList) {
        auto it = pending.begin();
        while (it != pending.end()) {
            if (it->retireAtFrame <= frameCounter_) {
                insertFreeBlock(freeList, {it->offset, it->count});
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
    };
    reclaim(vertexPendingFree_, vertexFreeList_);
    reclaim(indexPendingFree_, indexFreeList_);
}

void MeshPool::packBuffer(VkBuffer buffer, VkDeviceSize stride, std::vector<LiveEntry>& entries, uint32_t& cursor,
                            std::vector<FreeBlock>& freeList, std::vector<PendingFree>& pendingFree,
                            const char* debugName) {
    std::sort(entries.begin(), entries.end(),
              [](const LiveEntry& a, const LiveEntry& b) { return a.oldOffset < b.oldOffset; });

    uint32_t runningOffset = 0;
    bool anyMoved = false;
    for (LiveEntry& entry : entries) {
        entry.newOffset = runningOffset;
        if (entry.newOffset != entry.oldOffset) {
            anyMoved = true;
        }
        runningOffset += entry.count;
    }

    if (anyMoved) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(runningOffset) * stride;
        VmaAllocation scratchAllocation = VK_NULL_HANDLE;
        VkBuffer scratchBuffer = createDeviceLocalBuffer(context_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                          scratchAllocation, debugName);

        VkCommandBuffer cmd = beginOneShotCommandBuffer(context_, uploadCommandPool_);

        // Bounce every live range through scratchBuffer instead of
        // copying buffer -> buffer directly: a live range moving down by
        // less than its own size would otherwise be a same-buffer
        // overlapping-region copy, which vkCmdCopyBuffer disallows.
        for (const LiveEntry& entry : entries) {
            VkBufferCopy copy{};
            copy.srcOffset = static_cast<VkDeviceSize>(entry.oldOffset) * stride;
            copy.dstOffset = static_cast<VkDeviceSize>(entry.newOffset) * stride;
            copy.size = static_cast<VkDeviceSize>(entry.count) * stride;
            vkCmdCopyBuffer(cmd, buffer, scratchBuffer, 1, &copy);
        }

        VkBufferMemoryBarrier2 toTransferRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        toTransferRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toTransferRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toTransferRead.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toTransferRead.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toTransferRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferRead.buffer = scratchBuffer;
        toTransferRead.offset = 0;
        toTransferRead.size = bytes;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &toTransferRead;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkBufferCopy packedCopy{};
        packedCopy.srcOffset = 0;
        packedCopy.dstOffset = 0;
        packedCopy.size = bytes;
        vkCmdCopyBuffer(cmd, scratchBuffer, buffer, 1, &packedCopy);

        keel::vkCheck(vkEndCommandBuffer(cmd), "Failed to end mesh pool compaction command buffer");

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        keel::vkCheck(vkCreateFence(context_.device(), &fenceInfo, nullptr, &fence),
                      "Failed to create mesh pool compaction fence");

        // Waits for every allocate() upload signaled so far: separate
        // submissions to the same queue carry no implicit dependency, so
        // without this an allocate() still in flight could race this
        // copy's read of the same buffer it's writing into.
        VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitInfo.semaphore = timelineSemaphore_;
        waitInfo.value = nextSignalValue_ - 1;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo cmdSubmitInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmdSubmitInfo.commandBuffer = cmd;
        VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &waitInfo;
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &cmdSubmitInfo;
        keel::vkCheck(vkQueueSubmit2(context_.uploadQueue(), 1, &submitInfo, fence),
                      "Failed to submit mesh pool compaction");

        // Synchronous by design - see compact()'s doc comment: this is a
        // rare, explicit, already-GPU-idle operation, not a per-frame one.
        vkWaitForFences(context_.device(), 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(context_.device(), fence, nullptr);
        vkFreeCommandBuffers(context_.device(), uploadCommandPool_, 1, &cmd);
        vmaDestroyBuffer(context_.allocator(), scratchBuffer, scratchAllocation);
    }

    for (const LiveEntry& entry : entries) {
        auto it = liveRanges_.find(entry.meshIdValue);
        if (it == liveRanges_.end()) {
            continue;
        }
        if (buffer == vertexBuffer_) {
            it->second.vertexOffset = entry.newOffset;
        } else {
            it->second.indexOffset = entry.newOffset;
        }
    }

    cursor = runningOffset;
    freeList.clear();
    pendingFree.clear();
}

void MeshPool::compact() {
    std::vector<LiveEntry> vertexEntries;
    std::vector<LiveEntry> indexEntries;
    vertexEntries.reserve(liveRanges_.size());
    indexEntries.reserve(liveRanges_.size());
    for (const auto& [idValue, range] : liveRanges_) {
        vertexEntries.push_back({idValue, range.vertexOffset, 0, range.vertexCount});
        indexEntries.push_back({idValue, range.indexOffset, 0, range.indexCount});
    }

    packBuffer(vertexBuffer_, vertexStride_, vertexEntries, vertexCursor_, vertexFreeList_, vertexPendingFree_,
               "mesh pool compaction scratch (vertex)");
    packBuffer(indexBuffer_, sizeof(uint32_t), indexEntries, indexCursor_, indexFreeList_, indexPendingFree_,
               "mesh pool compaction scratch (index)");
}

} // namespace renderer
