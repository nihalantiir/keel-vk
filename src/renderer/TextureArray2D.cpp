#include "TextureArray2D.h"

#include "../keel-vk/DebugUtils.h"
#include "../keel-vk/VkCheck.h"
#include "../keel-vk/VulkanContext.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace renderer {

namespace {

// Rainbow-ish palette generator (three sine waves 120 degrees out of
// phase), not HSV: this is throwaway demo content for an unsampled
// resource, not worth sharing conversion code with the cube's real
// hue-phase shader logic.
void fillLayerColor(uint8_t* dst, uint32_t tileSize, uint32_t layerIndex) {
    const float phase = static_cast<float>(layerIndex) * 2.4f;
    const uint8_t r = static_cast<uint8_t>(128.0f + 127.0f * std::sin(phase));
    const uint8_t g = static_cast<uint8_t>(128.0f + 127.0f * std::sin(phase + 2.0944f));
    const uint8_t b = static_cast<uint8_t>(128.0f + 127.0f * std::sin(phase + 4.1888f));
    for (uint32_t i = 0; i < tileSize * tileSize; ++i) {
        dst[i * 4 + 0] = r;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = b;
        dst[i * 4 + 3] = 255;
    }
}

} // namespace

TextureArray2D::TextureArray2D(keel::VulkanContext& context, VkCommandPool commandPool, uint32_t tileSize,
                                uint32_t layerCount, VkSemaphore timelineSemaphore, uint64_t signalValue)
    : context_(context), tileSize_(tileSize), layerCount_(layerCount) {
    const VkDeviceSize layerBytes = static_cast<VkDeviceSize>(tileSize_) * tileSize_ * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(layerBytes) * layerCount_);
    for (uint32_t layer = 0; layer < layerCount_; ++layer) {
        fillLayerColor(pixels.data() + layer * layerBytes, tileSize_, layer);
    }

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = static_cast<VkDeviceSize>(pixels.size());
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
                  "Failed to create texture array staging buffer");
    std::memcpy(stagingAllocationInfo.pMappedData, pixels.data(), pixels.size());

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {tileSize_, tileSize_, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layerCount_;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // EXCLUSIVE: this upload and the graphics queue that samples it are
    // the same queue family (see the wiki's Rendering page for why 0.6.0
    // dropped the dedicated transfer queue), so no ownership transfer.
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo imageAllocInfo{};
    imageAllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    keel::vkCheck(vmaCreateImage(context_.allocator(), &imageInfo, &imageAllocInfo, &image_, &allocation_, nullptr),
                  "Failed to create texture array image");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image_),
                              "texture array (demo)");

    VkCommandBufferAllocateInfo cmdAllocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAllocInfo.commandPool = commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    keel::vkCheck(vkAllocateCommandBuffers(context_.device(), &cmdAllocInfo, &cmd),
                  "Failed to allocate texture array upload command buffer");

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    keel::vkCheck(vkBeginCommandBuffer(cmd, &beginInfo), "Failed to begin texture array upload command buffer");

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layerCount_};

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

    // One copy for every layer: consecutive layerBytes-sized chunks in the
    // staging buffer map directly onto consecutive array layers.
    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layerCount_};
    copyRegion.imageExtent = {tileSize_, tileSize_, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    VkImageMemoryBarrier2 toShaderRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    // ALL_COMMANDS/MEMORY_READ, not FRAGMENT_SHADER/SHADER_READ: costs
    // nothing on a one-shot startup upload and avoids naming a specific
    // consumer stage that could change.
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

    keel::vkCheck(vkEndCommandBuffer(cmd), "Failed to end texture array upload command buffer");

    // Signals timelineSemaphore to signalValue instead of vkQueueWaitIdle:
    // Renderer waits for the shared upload timeline once, on the GPU side,
    // before the first frame that might sample any of the three
    // construction-time texture uploads - see the wiki's Rendering page.
    // No CPU wait here means the staging buffer and command buffer aren't
    // safe to destroy yet: they're kept as members and freed in the
    // destructor instead, which only ever runs after ~Renderer's
    // vkDeviceWaitIdle has already guaranteed the GPU is idle.
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
                  "Failed to submit texture array upload");

    uploadCommandPool_ = commandPool;
    uploadCommandBuffer_ = cmd;
    stagingBuffer_ = stagingBuffer;
    stagingAllocation_ = stagingAllocation;

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = range;
    keel::vkCheck(vkCreateImageView(context_.device(), &viewInfo, nullptr, &view_),
                  "Failed to create texture array view");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    keel::vkCheck(vkCreateSampler(context_.device(), &samplerInfo, nullptr, &sampler_),
                  "Failed to create texture array sampler");
}

TextureArray2D::~TextureArray2D() {
    // Only reached after ~Renderer's vkDeviceWaitIdle, so the construction
    // upload this class fired off without waiting on it is long since
    // complete by now - safe to free unconditionally.
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
