#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <unordered_map>
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
    uint32_t vertexCount = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
};

// One DEVICE_LOCAL vertex buffer, one DEVICE_LOCAL index buffer, both
// written through a per-call staging buffer + vkCmdCopyBuffer rather than
// host-visible mapped memory. A free-list allocator: allocate() reuses
// space free() has returned before it ever grows either cursor further.
// free() itself is cheap and never blocks - see its own comment - but
// reclaimed space can still end up fragmented; compact() is the separate,
// deliberately expensive operation that eliminates that by moving live
// data. Neither runs automatically; a caller decides when either is worth
// its cost.
class MeshPool {
public:
    // uploadCommandPool: bound to VulkanContext::uploadQueueFamily(), same
    // pool Renderer's other construction-time uploads use. timelineSemaphore:
    // shared with those same uploads (see Renderer::createUploadTimelineSemaphore);
    // each allocate() call signals it to a fresh, strictly increasing
    // value instead of blocking the CPU. Caller is responsible for
    // waiting the timeline up to the highest value ever signaled before
    // anything might read the ranges those calls wrote. framesInFlight:
    // same role as TextureStreamer's constructor parameter of the same
    // name - free()'d space isn't handed back to allocate() until this
    // many beginFrame() calls have elapsed, so a draw call recorded
    // before the free() can never end up reading data allocate() wrote
    // into what used to be its range.
    MeshPool(keel::VulkanContext& context, VkCommandPool uploadCommandPool, VkSemaphore timelineSemaphore,
             uint64_t firstSignalValue, uint32_t vertexCapacity, uint32_t indexCapacity, VkDeviceSize vertexStride,
             uint32_t framesInFlight);
    ~MeshPool();

    MeshPool(const MeshPool&) = delete;
    MeshPool& operator=(const MeshPool&) = delete;

    // vertices must point at vertexCount * vertexStride bytes. Throws if
    // neither the free list nor the remaining fixed capacity can satisfy
    // either cursor. Never blocks: records a one-shot staging copy and
    // returns after submitting it, signaling the timeline semaphore given
    // at construction.
    MeshRange allocate(const void* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount);

    // Returns id's vertex/index ranges to the free list, reusable once
    // beginFrame() has been called framesInFlight times since (see the
    // constructor's comment). No GPU work, never blocks. A no-op if id is
    // unknown or already freed - the same tradeoff TextureStreamer::free
    // makes for a stale handle.
    void free(MeshId id);

    // id's current range. Compaction moves live data, so a caller must
    // call this fresh every frame rather than caching a MeshRange from
    // allocate() - see compact(). Throws if id is unknown or freed.
    MeshRange resolve(MeshId id) const;

    // Advances the frame counter free()'d ranges are retired against and
    // folds any newly-eligible ones into the reusable free list. Call
    // once per Renderer::drawFrame; needs no command buffer, does no GPU
    // work.
    void beginFrame();

    // Moves every live mesh's data down to a tightly packed prefix of
    // each buffer, eliminating fragmentation the free list couldn't reuse
    // (a live range sitting between two free ones that's never itself
    // freed). Invalidates every previously-resolve()d MeshRange's offsets
    // - re-resolve after calling this.
    //
    // Caller must guarantee no GPU work still reads vertexBuffer()/
    // indexBuffer() before calling (e.g. vkDeviceWaitIdle) - this rewrites
    // live data in place with no per-draw dependency tracking, the same
    // tradeoff ~MeshPool's own destructor already makes for its pending
    // uploads. MeshPool's own still-in-flight allocate() uploads are
    // already handled - this waits on timelineSemaphore_ itself before
    // touching either buffer. Expensive and synchronous by design
    // (submits and waits inline before returning): call rarely, at a
    // controlled boundary (e.g. a level transition), never from inside
    // the per-frame loop.
    void compact();

    // The highest timeline value any allocate() call so far has signaled.
    // Starts at firstSignalValue - 1 (nothing signaled yet) if allocate()
    // is never called.
    uint64_t lastSignaledUploadValue() const { return nextSignalValue_ - 1; }

    // For the debug overlay - mirrors Renderer::residentBytes()/
    // evictionCount()'s role for texture residency.
    uint32_t liveMeshCount() const { return static_cast<uint32_t>(liveRanges_.size()); }
    VkDeviceSize freeListBytes() const;

    VkBuffer vertexBuffer() const { return vertexBuffer_; }
    VkBuffer indexBuffer() const { return indexBuffer_; }

private:
    // A run of count free elements starting at offset, in a buffer's own
    // element units (vertices or indices, not bytes).
    struct FreeBlock {
        uint32_t offset;
        uint32_t count;
    };

    // A free() that hasn't yet cleared its GPU-safety delay - see the
    // constructor's framesInFlight comment.
    struct PendingFree {
        uint32_t offset;
        uint32_t count;
        uint64_t retireAtFrame;
    };

    // One live mesh's current range in a single buffer, used by compact()
    // to sort and repack; meshIdValue ties a repacked entry back to its
    // MeshRange in liveRanges_.
    struct LiveEntry {
        uint32_t meshIdValue;
        uint32_t oldOffset;
        uint32_t newOffset = 0;
        uint32_t count;
    };

    static void insertFreeBlock(std::vector<FreeBlock>& freeList, FreeBlock block);
    // First-fit: returns the offset of a block big enough for count
    // elements and shrinks or removes it, or returns false if nothing
    // in freeList fits.
    static bool allocateFromFreeList(std::vector<FreeBlock>& freeList, uint32_t count, uint32_t& outOffset);
    static VkDeviceSize freeListBytesOf(const std::vector<FreeBlock>& freeList, VkDeviceSize stride);

    // Shared compaction body for one buffer: sorts entries by oldOffset,
    // assigns tightly-packed newOffset values, and - only if anything
    // actually moved - bounces the live data through a temporary scratch
    // buffer (never a same-buffer overlapping copy) before writing
    // entries' newOffset back. Leaves cursor at the packed total and both
    // list arguments empty.
    void packBuffer(VkBuffer buffer, VkDeviceSize stride, std::vector<LiveEntry>& entries, uint32_t& cursor,
                     std::vector<FreeBlock>& freeList, std::vector<PendingFree>& pendingFree, const char* debugName);

    keel::VulkanContext& context_;
    VkCommandPool uploadCommandPool_;
    VkSemaphore timelineSemaphore_;
    uint64_t nextSignalValue_;
    uint32_t framesInFlight_;
    uint64_t frameCounter_ = 0;

    VkDeviceSize vertexStride_;
    uint32_t vertexCapacity_;
    uint32_t indexCapacity_;
    uint32_t vertexCursor_ = 0;
    uint32_t indexCursor_ = 0;
    uint32_t nextMeshId_ = 0;

    // Canonical id -> current range. resolve() reads this directly;
    // allocate()/free()/compact() are its only writers.
    std::unordered_map<uint32_t, MeshRange> liveRanges_;

    std::vector<FreeBlock> vertexFreeList_;
    std::vector<FreeBlock> indexFreeList_;
    std::vector<PendingFree> vertexPendingFree_;
    std::vector<PendingFree> indexPendingFree_;

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
