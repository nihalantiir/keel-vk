#include "Atlas2D.h"

#include "../keel-vk/DebugUtils.h"
#include "../keel-vk/VkCheck.h"
#include "../keel-vk/VulkanContext.h"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace renderer {

Atlas2D::Atlas2D(keel::VulkanContext& context, VkCommandPool commandPool, uint32_t pageSize,
                  const std::vector<AtlasEntry>& entries, VkSemaphore timelineSemaphore, uint64_t signalValue)
    : context_(context), pageSize_(pageSize) {
    // Composited into one CPU-side page buffer first (simpler than per-rect
    // VkBufferImageCopy regions with row-pitch bookkeeping), then uploaded
    // as a single copy.
    std::vector<uint8_t> page(static_cast<size_t>(pageSize_) * pageSize_ * 4, 0);

    uint32_t cursorX = 0;
    uint32_t cursorY = 0;
    uint32_t shelfHeight = 0;

    for (const AtlasEntry& entry : entries) {
        if (cursorX + entry.width > pageSize_) {
            cursorX = 0;
            cursorY += shelfHeight;
            shelfHeight = 0;
        }
        if (cursorY + entry.height > pageSize_) {
            throw std::runtime_error("Atlas2D: page is full, entry does not fit");
        }

        const uint32_t rectX = cursorX;
        const uint32_t rectY = cursorY;
        const uint8_t* src = static_cast<const uint8_t*>(entry.pixelsRgba8);
        for (uint32_t row = 0; row < entry.height; ++row) {
            uint8_t* dst = page.data() + ((static_cast<size_t>(rectY) + row) * pageSize_ + rectX) * 4;
            std::memcpy(dst, src + static_cast<size_t>(row) * entry.width * 4, entry.width * 4);
        }

        AtlasRect rect{};
        rect.x = rectX;
        rect.y = rectY;
        rect.width = entry.width;
        rect.height = entry.height;
        rect.u0 = static_cast<float>(rectX) / static_cast<float>(pageSize_);
        rect.v0 = static_cast<float>(rectY) / static_cast<float>(pageSize_);
        rect.u1 = static_cast<float>(rectX + entry.width) / static_cast<float>(pageSize_);
        rect.v1 = static_cast<float>(rectY + entry.height) / static_cast<float>(pageSize_);
        rects_.push_back(rect);

        cursorX += entry.width;
        shelfHeight = shelfHeight > entry.height ? shelfHeight : entry.height;
    }

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = static_cast<VkDeviceSize>(page.size());
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    VmaAllocationInfo stagingAllocationInfo{};
    keel::vkCheck(vmaCreateBuffer(context_.allocator(), &bufferInfo, &stagingAllocInfo, &stagingBuffer,
                                   &stagingAllocation, &stagingAllocationInfo),
                  "Failed to create atlas staging buffer");
    std::memcpy(stagingAllocationInfo.pMappedData, page.data(), page.size());

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {pageSize_, pageSize_, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // CONCURRENT when a dedicated transfer queue exists: this upload runs
    // on that queue (see below), and CONCURRENT avoids needing an explicit
    // queue-family ownership transfer to hand the image to the graphics
    // queue for sampling.
    const uint32_t concurrentFamilies[] = {context_.queueFamilies().graphicsFamily.value(),
                                            context_.uploadQueueFamily()};
    if (context_.hasDedicatedTransferQueue()) {
        imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = 2;
        imageInfo.pQueueFamilyIndices = concurrentFamilies;
    } else {
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VmaAllocationCreateInfo imageAllocInfo{};
    imageAllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    keel::vkCheck(vmaCreateImage(context_.allocator(), &imageInfo, &imageAllocInfo, &image_, &allocation_, nullptr),
                  "Failed to create atlas image");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image_),
                              "atlas page (demo)");

    VkCommandBufferAllocateInfo cmdAllocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAllocInfo.commandPool = commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    keel::vkCheck(vkAllocateCommandBuffers(context_.device(), &cmdAllocInfo, &cmd),
                  "Failed to allocate atlas upload command buffer");

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    keel::vkCheck(vkBeginCommandBuffer(cmd, &beginInfo), "Failed to begin atlas upload command buffer");

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier2 toTransferDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toTransferDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toTransferDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toTransferDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toTransferDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDst.image = image_;
    toTransferDst.subresourceRange = range;
    VkDependencyInfo toTransferDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toTransferDependency.imageMemoryBarrierCount = 1;
    toTransferDependency.pImageMemoryBarriers = &toTransferDst;
    vkCmdPipelineBarrier2(cmd, &toTransferDependency);

    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageExtent = {pageSize_, pageSize_, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    VkImageMemoryBarrier2 toShaderRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    // ALL_COMMANDS/MEMORY_READ, not FRAGMENT_SHADER/SHADER_READ: this
    // command buffer may be recorded against a transfer-only queue family
    // (see VulkanContext::uploadQueue), and FRAGMENT_SHADER is not a valid
    // destination stage there. The coarser mask costs nothing on a
    // one-shot startup upload.
    toShaderRead.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = image_;
    toShaderRead.subresourceRange = range;
    VkDependencyInfo toShaderReadDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toShaderReadDependency.imageMemoryBarrierCount = 1;
    toShaderReadDependency.pImageMemoryBarriers = &toShaderRead;
    vkCmdPipelineBarrier2(cmd, &toShaderReadDependency);

    keel::vkCheck(vkEndCommandBuffer(cmd), "Failed to end atlas upload command buffer");

    // Signals timelineSemaphore instead of vkQueueWaitIdle; see
    // TextureArray2D.cpp's matching comment. No CPU wait means the
    // staging/command buffers move into member variables below instead of
    // being destroyed here.
    VkCommandBufferSubmitInfo cmdSubmitInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdSubmitInfo.commandBuffer = cmd;
    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = timelineSemaphore;
    signalInfo.value = signalValue;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdSubmitInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalInfo;
    keel::vkCheck(vkQueueSubmit2(context_.uploadQueue(), 1, &submitInfo, VK_NULL_HANDLE),
                  "Failed to submit atlas upload");

    uploadCommandPool_ = commandPool;
    uploadCommandBuffer_ = cmd;
    stagingBuffer_ = stagingBuffer;
    stagingAllocation_ = stagingAllocation;

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = range;
    keel::vkCheck(vkCreateImageView(context_.device(), &viewInfo, nullptr, &view_), "Failed to create atlas view");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    keel::vkCheck(vkCreateSampler(context_.device(), &samplerInfo, nullptr, &sampler_),
                  "Failed to create atlas sampler");
}

Atlas2D::~Atlas2D() {
    // Only reached after ~Renderer's vkDeviceWaitIdle; see
    // TextureArray2D.cpp's matching destructor comment.
    if (uploadCommandBuffer_ != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(context_.device(), uploadCommandPool_, 1, &uploadCommandBuffer_);
    }
    if (stagingBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context_.allocator(), stagingBuffer_, stagingAllocation_);
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(context_.device(), sampler_, nullptr);
    }
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(context_.device(), view_, nullptr);
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(context_.allocator(), image_, allocation_);
    }
}

} // namespace renderer
