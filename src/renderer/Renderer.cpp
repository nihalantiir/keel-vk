#include "Renderer.h"

#include "../keel-vk/DebugUtils.h"
#include "../keel-vk/ShaderModule.h"
#include "../keel-vk/Swapchain.h"
#include "../keel-vk/VkCheck.h"
#include "../keel-vk/VulkanContext.h"
#include "../keel-vk/Window.h"

#if KEEL_VK_IMGUI
#include "../debug/DebugUi.h"
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>

namespace renderer {

namespace {

// 24 vertices (4 per face, not shared) so every face gets a flat, uniform
// hue with no interpolation seams across edges. Faces are wound CCW as seen
// from outside; the pipeline's frontFace matches that directly (see
// createPipeline) since the projection's Y-flip affects near and far faces
// identically and doesn't change which triangles should be culled.
// uv follows the same (0,0),(1,0),(1,1),(0,1) order as each face's 4
// vertices, so the checker texture reads as one full tile per face.
constexpr std::array<Vertex, 24> kVertices = {{
    // +Z front, hue 0
    {{-0.5f, -0.5f, 0.5f}, 0.0f, {0.0f, 0.0f}},
    {{0.5f, -0.5f, 0.5f}, 0.0f, {1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.5f}, 0.0f, {1.0f, 1.0f}},
    {{-0.5f, 0.5f, 0.5f}, 0.0f, {0.0f, 1.0f}},
    // -Z back, hue 60
    {{0.5f, -0.5f, -0.5f}, 60.0f, {0.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, 60.0f, {1.0f, 0.0f}},
    {{-0.5f, 0.5f, -0.5f}, 60.0f, {1.0f, 1.0f}},
    {{0.5f, 0.5f, -0.5f}, 60.0f, {0.0f, 1.0f}},
    // +X right, hue 120
    {{0.5f, -0.5f, 0.5f}, 120.0f, {0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, 120.0f, {1.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, 120.0f, {1.0f, 1.0f}},
    {{0.5f, 0.5f, 0.5f}, 120.0f, {0.0f, 1.0f}},
    // -X left, hue 180
    {{-0.5f, -0.5f, -0.5f}, 180.0f, {0.0f, 0.0f}},
    {{-0.5f, -0.5f, 0.5f}, 180.0f, {1.0f, 0.0f}},
    {{-0.5f, 0.5f, 0.5f}, 180.0f, {1.0f, 1.0f}},
    {{-0.5f, 0.5f, -0.5f}, 180.0f, {0.0f, 1.0f}},
    // +Y top, hue 240
    {{-0.5f, 0.5f, -0.5f}, 240.0f, {0.0f, 0.0f}},
    {{-0.5f, 0.5f, 0.5f}, 240.0f, {1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.5f}, 240.0f, {1.0f, 1.0f}},
    {{0.5f, 0.5f, -0.5f}, 240.0f, {0.0f, 1.0f}},
    // -Y bottom, hue 300
    {{-0.5f, -0.5f, 0.5f}, 300.0f, {0.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, 300.0f, {1.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, 300.0f, {1.0f, 1.0f}},
    {{0.5f, -0.5f, 0.5f}, 300.0f, {0.0f, 1.0f}},
}};

constexpr std::array<uint32_t, 36> kIndices = {{
    0, 1, 2, 0, 2, 3,       // +Z
    4, 5, 6, 4, 6, 7,       // -Z
    8, 9, 10, 8, 10, 11,    // +X
    12, 13, 14, 12, 14, 15, // -X
    16, 17, 18, 16, 18, 19, // +Y
    20, 21, 22, 20, 22, 23, // -Y
}};

struct PushConstants {
    glm::mat4 mvp;
    float time;
    float phaseSpeed;
    uint32_t textureIndex;
};

// 8x8 checkerboard, generated rather than loaded: a real bindless texture
// with no image-loading dependency yet (see docs/wiki Libraries - stb_image
// is deliberately not in this landing).
constexpr uint32_t kCheckerSize = 8;

std::array<uint8_t, kCheckerSize * kCheckerSize * 4> makeCheckerPixels() {
    std::array<uint8_t, kCheckerSize * kCheckerSize * 4> pixels{};
    for (uint32_t y = 0; y < kCheckerSize; ++y) {
        for (uint32_t x = 0; x < kCheckerSize; ++x) {
            const bool light = ((x + y) % 2) == 0;
            const uint8_t value = light ? 255 : 170;
            const size_t offset = (y * kCheckerSize + x) * 4;
            pixels[offset + 0] = value;
            pixels[offset + 1] = value;
            pixels[offset + 2] = value;
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}

glm::vec3 hsv2rgb(float hueDegrees, float saturation, float value) {
    const float h = std::fmod(std::fmod(hueDegrees, 360.0f) + 360.0f, 360.0f) / 60.0f;
    const float c = value * saturation;
    const float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    glm::vec3 rgb;
    if (h < 1.0f) rgb = {c, x, 0.0f};
    else if (h < 2.0f) rgb = {x, c, 0.0f};
    else if (h < 3.0f) rgb = {0.0f, c, x};
    else if (h < 4.0f) rgb = {0.0f, x, c};
    else if (h < 5.0f) rgb = {x, 0.0f, c};
    else rgb = {c, 0.0f, x};
    return rgb + glm::vec3(value - c);
}

} // namespace

Renderer::Renderer(keel::VulkanContext& context, keel::Swapchain& swapchain, keel::Window& window)
    : context_(context), swapchain_(swapchain), window_(window) {
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();
    createGeometryBuffers();
    createTexture();
    createDescriptors();
    createDepthTarget();
    createPipeline();
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(context_.device());

    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_.device(), pipeline_, nullptr);
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(context_.device(), pipelineLayout_, nullptr);
    }

    destroyDepthTarget();
    destroyDescriptors();
    destroyTexture();

    if (indirectBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context_.allocator(), indirectBuffer_, indirectBufferAllocation_);
    }
    if (indexBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context_.allocator(), indexBuffer_, indexBufferAllocation_);
    }
    if (vertexBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context_.allocator(), vertexBuffer_, vertexBufferAllocation_);
    }

    destroySyncObjects();
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(context_.device(), commandPool_, nullptr);
    }
}

void Renderer::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context_.queueFamilies().graphicsFamily.value();

    keel::vkCheck(vkCreateCommandPool(context_.device(), &poolInfo, nullptr, &commandPool_),
                  "Failed to create command pool");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_COMMAND_POOL,
                              reinterpret_cast<uint64_t>(commandPool_), "command pool");
}

void Renderer::createCommandBuffers() {
    commandBuffers_.resize(kFramesInFlight);

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kFramesInFlight;

    keel::vkCheck(vkAllocateCommandBuffers(context_.device(), &allocInfo, commandBuffers_.data()),
                  "Failed to allocate command buffers");

    for (int i = 0; i < kFramesInFlight; ++i) {
        const std::string debugName = "frame command buffer " + std::to_string(i);
        keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_COMMAND_BUFFER,
                                  reinterpret_cast<uint64_t>(commandBuffers_[i]), debugName.c_str());
    }
}

void Renderer::createSyncObjects() {
    imageAvailableSemaphores_.resize(kFramesInFlight);
    inFlightFences_.resize(kFramesInFlight);

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // start signaled so the first wait doesn't block forever

    for (int i = 0; i < kFramesInFlight; ++i) {
        keel::vkCheck(vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]),
                      "Failed to create image-available semaphore");
        keel::vkCheck(vkCreateFence(context_.device(), &fenceInfo, nullptr, &inFlightFences_[i]),
                      "Failed to create in-flight fence");
    }

    // One render-finished semaphore per swapchain image (not per frame in
    // flight): a semaphore can't be re-signaled while a previous signal is
    // still unconsumed, which per-frame sizing can violate when present
    // takes longer than a frame.
    renderFinishedSemaphores_.resize(swapchain_.imageCount());
    for (auto& semaphore : renderFinishedSemaphores_) {
        keel::vkCheck(vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &semaphore),
                      "Failed to create render-finished semaphore");
    }

    imagesInFlight_.assign(swapchain_.imageCount(), VK_NULL_HANDLE);
}

void Renderer::recreateSyncObjectsForSwapchain() {
    for (VkSemaphore semaphore : renderFinishedSemaphores_) {
        vkDestroySemaphore(context_.device(), semaphore, nullptr);
    }

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderFinishedSemaphores_.assign(swapchain_.imageCount(), VK_NULL_HANDLE);
    for (auto& semaphore : renderFinishedSemaphores_) {
        keel::vkCheck(vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &semaphore),
                      "Failed to recreate render-finished semaphore");
    }

    imagesInFlight_.assign(swapchain_.imageCount(), VK_NULL_HANDLE);

    destroyDepthTarget();
    createDepthTarget();
}

void Renderer::destroySyncObjects() {
    for (VkSemaphore semaphore : imageAvailableSemaphores_) {
        vkDestroySemaphore(context_.device(), semaphore, nullptr);
    }
    for (VkSemaphore semaphore : renderFinishedSemaphores_) {
        vkDestroySemaphore(context_.device(), semaphore, nullptr);
    }
    for (VkFence fence : inFlightFences_) {
        vkDestroyFence(context_.device(), fence, nullptr);
    }
}

namespace {

VkBuffer createDeviceLocalBufferWithData(keel::VulkanContext& context, const void* data, VkDeviceSize size,
                                          VkBufferUsageFlags usage, VmaAllocation& outAllocation,
                                          const char* debugName) {
    // Host-visible mapped, written once: these buffers are small and static,
    // so a staging upload would only add cost for no benefit here.
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};
    keel::vkCheck(vmaCreateBuffer(context.allocator(), &bufferInfo, &allocInfo, &buffer, &outAllocation,
                                   &allocationInfo),
                  "Failed to create buffer");
    std::memcpy(allocationInfo.pMappedData, data, static_cast<size_t>(size));

    keel::setDebugObjectName(context.device(), VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer), debugName);
    return buffer;
}

} // namespace

void Renderer::createGeometryBuffers() {
    vertexBuffer_ = createDeviceLocalBufferWithData(context_, kVertices.data(), sizeof(kVertices),
                                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBufferAllocation_,
                                                      "cube vertex buffer");
    indexBuffer_ = createDeviceLocalBufferWithData(context_, kIndices.data(), sizeof(kIndices),
                                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBufferAllocation_,
                                                     "cube index buffer");

    // One draw command today, but a real indirect buffer: the scaffold for
    // batching many draws through vkCmdDrawIndexedIndirect later.
    VkDrawIndexedIndirectCommand command{};
    command.indexCount = static_cast<uint32_t>(kIndices.size());
    command.instanceCount = 1;
    command.firstIndex = 0;
    command.vertexOffset = 0;
    command.firstInstance = 0;

    indirectBuffer_ = createDeviceLocalBufferWithData(context_, &command, sizeof(command),
                                                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, indirectBufferAllocation_,
                                                        "cube indirect draw buffer");
}

void Renderer::createTexture() {
    const std::array<uint8_t, kCheckerSize * kCheckerSize * 4> pixels = makeCheckerPixels();
    const VkDeviceSize byteSize = pixels.size();

    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    VkBuffer stagingBuffer = createDeviceLocalBufferWithData(context_, pixels.data(), byteSize,
                                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stagingAllocation,
                                                               "checker texture staging buffer");

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {kCheckerSize, kCheckerSize, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    keel::vkCheck(vmaCreateImage(context_.allocator(), &imageInfo, &allocInfo, &textureImage_,
                                  &textureImageAllocation_, nullptr),
                  "Failed to create checker texture image");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(textureImage_),
                              "checker texture");

    // One-shot upload: this texture is static and tiny, so a dedicated
    // transfer path isn't worth it yet (see Renderer opinions in the wiki's
    // Rendering page - a transfer queue is reserved, not required, for a
    // static asset like this one).
    VkCommandBufferAllocateInfo cmdAllocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAllocInfo.commandPool = commandPool_;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    keel::vkCheck(vkAllocateCommandBuffers(context_.device(), &cmdAllocInfo, &cmd),
                  "Failed to allocate one-shot upload command buffer");

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    keel::vkCheck(vkBeginCommandBuffer(cmd, &beginInfo), "Failed to begin upload command buffer");

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier2 toTransferDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toTransferDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toTransferDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toTransferDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toTransferDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDst.image = textureImage_;
    toTransferDst.subresourceRange = range;

    VkDependencyInfo toTransferDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toTransferDependency.imageMemoryBarrierCount = 1;
    toTransferDependency.pImageMemoryBarriers = &toTransferDst;
    vkCmdPipelineBarrier2(cmd, &toTransferDependency);

    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageExtent = {kCheckerSize, kCheckerSize, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer, textureImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    VkImageMemoryBarrier2 toShaderRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toShaderRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = textureImage_;
    toShaderRead.subresourceRange = range;

    VkDependencyInfo toShaderReadDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toShaderReadDependency.imageMemoryBarrierCount = 1;
    toShaderReadDependency.pImageMemoryBarriers = &toShaderRead;
    vkCmdPipelineBarrier2(cmd, &toShaderReadDependency);

    keel::vkCheck(vkEndCommandBuffer(cmd), "Failed to end upload command buffer");

    VkCommandBufferSubmitInfo cmdSubmitInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdSubmitInfo.commandBuffer = cmd;
    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdSubmitInfo;
    keel::vkCheck(vkQueueSubmit2(context_.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE),
                  "Failed to submit texture upload");
    keel::vkCheck(vkQueueWaitIdle(context_.graphicsQueue()), "Failed to wait for texture upload");

    vkFreeCommandBuffers(context_.device(), commandPool_, 1, &cmd);
    vmaDestroyBuffer(context_.allocator(), stagingBuffer, stagingAllocation);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = textureImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = range;
    keel::vkCheck(vkCreateImageView(context_.device(), &viewInfo, nullptr, &textureImageView_),
                  "Failed to create checker texture view");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 0.0f;
    keel::vkCheck(vkCreateSampler(context_.device(), &samplerInfo, nullptr, &textureSampler_),
                  "Failed to create checker texture sampler");
}

void Renderer::destroyTexture() {
    if (textureSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(context_.device(), textureSampler_, nullptr);
        textureSampler_ = VK_NULL_HANDLE;
    }
    if (textureImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(context_.device(), textureImageView_, nullptr);
        textureImageView_ = VK_NULL_HANDLE;
    }
    if (textureImage_ != VK_NULL_HANDLE) {
        vmaDestroyImage(context_.allocator(), textureImage_, textureImageAllocation_);
        textureImage_ = VK_NULL_HANDLE;
    }
}

void Renderer::createDescriptors() {
    // A fixed-capacity, update-after-bind sampled-image array: the device
    // contract already requires update-after-bind, partially-bound, and
    // variable-descriptor-count support, so this is the first thing to
    // actually use it. Only one slot is filled today; the rest stay unused
    // until a content-pack texture loader exists.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = kMaxBindlessTextures;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    const VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                    VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                                    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bindingFlagsInfo.bindingCount = 1;
    bindingFlagsInfo.pBindingFlags = &bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.pNext = &bindingFlagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    keel::vkCheck(vkCreateDescriptorSetLayout(context_.device(), &layoutInfo, nullptr, &descriptorSetLayout_),
                  "Failed to create bindless descriptor set layout");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxBindlessTextures};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    keel::vkCheck(vkCreateDescriptorPool(context_.device(), &poolInfo, nullptr, &descriptorPool_),
                  "Failed to create bindless descriptor pool");

    const uint32_t variableCount = 1; // one texture registered so far
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableCountInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    variableCountInfo.descriptorSetCount = 1;
    variableCountInfo.pDescriptorCounts = &variableCount;

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.pNext = &variableCountInfo;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout_;
    keel::vkCheck(vkAllocateDescriptorSets(context_.device(), &allocInfo, &descriptorSet_),
                  "Failed to allocate bindless descriptor set");

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = textureSampler_;
    imageInfo.imageView = textureImageView_;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);
}

void Renderer::destroyDescriptors() {
    // Freeing the pool also frees the set allocated from it.
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_.device(), descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSet_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context_.device(), descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
}

void Renderer::createDepthTarget() {
    const VkExtent2D extent = swapchain_.extent();

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kDepthFormat;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    keel::vkCheck(vmaCreateImage(context_.allocator(), &imageInfo, &allocInfo, &depthImage_, &depthImageAllocation_,
                                  nullptr),
                  "Failed to create depth image");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(depthImage_),
                              "depth image");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = depthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kDepthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    keel::vkCheck(vkCreateImageView(context_.device(), &viewInfo, nullptr, &depthImageView_),
                  "Failed to create depth image view");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(depthImageView_),
                              "depth image view");
}

void Renderer::destroyDepthTarget() {
    if (depthImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(context_.device(), depthImageView_, nullptr);
        depthImageView_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vmaDestroyImage(context_.allocator(), depthImage_, depthImageAllocation_);
        depthImage_ = VK_NULL_HANDLE;
    }
}

void Renderer::createPipeline() {
    keel::ShaderModule vertShader(context_, "shaders/cube.vert.spv", "cube.vert");
    keel::ShaderModule fragShader(context_, "shaders/cube.frag.spv", "cube.frag");

    VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertShader.handle();
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragShader.handle();
    fragStage.pName = "main";

    const VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, baseHueDegrees)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
    };

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(attributes));
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Dynamic viewport/scissor: the pipeline never needs to be recreated
    // when the swapchain is resized, only its extent changes.
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates));
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Geometry is wound CCW as seen from outside each face (the standard
    // convention), and frontFace is left at COUNTER_CLOCKWISE to match:
    // the projection's Y-flip (see the mvp computation in
    // recordCommandBuffer) mirrors clip-space Y, which would reverse the
    // apparent winding of every triangle equally, near and far faces alike
    // - it does not change which faces are near vs. far, so the original
    // CCW-outward authoring is still what the rasterizer should treat as
    // front-facing after the flip.
    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;

    // Covers both stages: mvp/time/phaseSpeed are read in the vertex shader,
    // textureIndex in the fragment shader, all from the one push constant
    // block declared identically in both.
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    keel::vkCheck(vkCreatePipelineLayout(context_.device(), &layoutInfo, nullptr, &pipelineLayout_),
                  "Failed to create pipeline layout");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                              reinterpret_cast<uint64_t>(pipelineLayout_), "cube pipeline layout");

    const VkFormat colorFormat = swapchain_.imageFormat();
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = static_cast<uint32_t>(std::size(stages));
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;

    keel::vkCheck(vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_),
                  "Failed to create graphics pipeline");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline_),
                              "cube pipeline");
}

void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, debug::DebugUi* debugUi) {
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    keel::vkCheck(vkBeginCommandBuffer(cmd, &beginInfo), "Failed to begin command buffer");

    const VkImage image = swapchain_.images()[imageIndex];
    const VkImageView imageView = swapchain_.imageViews()[imageIndex];
    const VkExtent2D extent = swapchain_.extent();

    VkImageMemoryBarrier2 toColorAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toColorAttachment.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toColorAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColorAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toColorAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColorAttachment.image = image;
    toColorAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Depth loadOp is CLEAR, so previous contents never matter: transitioning
    // from UNDEFINED every frame is valid and avoids tracking layout state
    // across frames.
    VkImageMemoryBarrier2 toDepthAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toDepthAttachment.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toDepthAttachment.dstStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toDepthAttachment.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toDepthAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDepthAttachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    toDepthAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDepthAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDepthAttachment.image = depthImage_;
    toDepthAttachment.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    const VkImageMemoryBarrier2 preBarriers[] = {toColorAttachment, toDepthAttachment};
    VkDependencyInfo preDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    preDependency.imageMemoryBarrierCount = static_cast<uint32_t>(std::size(preBarriers));
    preDependency.pImageMemoryBarriers = preBarriers;
    vkCmdPipelineBarrier2(cmd, &preDependency);

    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = imageView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{clearColor_[0], clearColor_[1], clearColor_[2], 1.0f}};

    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = depthImageView_;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                               0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // model_ is set each frame in main() from a keel::World entity's
    // Transform (see src/shared/Components.h's toMatrix); Renderer only
    // consumes it here.
    const glm::mat4 view = glm::lookAt(glm::vec3(2.2f, 1.8f, 2.6f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
    // GLM's clip space assumes OpenGL's Y-up NDC; Vulkan's NDC Y points down.
    // Flipping this one entry is the standard fix instead of reaching for a
    // GLM Vulkan-specific build.
    proj[1][1] *= -1.0f;

    PushConstants pushConstants{};
    pushConstants.mvp = proj * view * model_;
    pushConstants.time = elapsedTimeSeconds_;
    pushConstants.phaseSpeed = phaseSpeedDegPerSec_;
    pushConstants.textureIndex = 0; // the checker texture's bindless slot
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                        sizeof(pushConstants), &pushConstants);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &offset);
    vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(cmd, indirectBuffer_, 0, 1, sizeof(VkDrawIndexedIndirectCommand));

#if KEEL_VK_IMGUI
    if (debugUi) {
        debugUi->render(cmd);
    }
#else
    static_cast<void>(debugUi);
#endif

    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier2 toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = image;
    toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo postDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    postDependency.imageMemoryBarrierCount = 1;
    postDependency.pImageMemoryBarriers = &toPresent;
    vkCmdPipelineBarrier2(cmd, &postDependency);

    keel::vkCheck(vkEndCommandBuffer(cmd), "Failed to end command buffer");
}

void Renderer::drawFrame(debug::DebugUi* debugUi) {
    const uint64_t now = SDL_GetPerformanceCounter();
    if (!clockStarted_) {
        lastTicks_ = now;
        clockStarted_ = true;
    }
    const float dt =
        static_cast<float>(now - lastTicks_) / static_cast<float>(SDL_GetPerformanceFrequency());
    lastTicks_ = now;
    if (!paused_) {
        elapsedTimeSeconds_ += dt;
    }

    const VkDevice device = context_.device();

    vkWaitForFences(device, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    const VkResult acquireResult = vkAcquireNextImageKHR(
        device, swapchain_.handle(), UINT64_MAX, imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE, &imageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_.recreate();
        recreateSyncObjectsForSwapchain();
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

    // If this image is still being read by a previous frame's submission,
    // wait for that frame to finish before reusing it.
    if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(device, 1, &imagesInFlight_[imageIndex], VK_TRUE, UINT64_MAX);
    }
    imagesInFlight_[imageIndex] = inFlightFences_[currentFrame_];

    vkResetFences(device, 1, &inFlightFences_[currentFrame_]);
    vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);
    recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex, debugUi);

    VkSemaphoreSubmitInfo waitSemaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitSemaphoreInfo.semaphore = imageAvailableSemaphores_[currentFrame_];
    waitSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalSemaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalSemaphoreInfo.semaphore = renderFinishedSemaphores_[imageIndex];
    signalSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo commandBufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandBufferInfo.commandBuffer = commandBuffers_[currentFrame_];

    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitSemaphoreInfo;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalSemaphoreInfo;

    keel::vkCheck(vkQueueSubmit2(context_.graphicsQueue(), 1, &submitInfo, inFlightFences_[currentFrame_]),
                  "Failed to submit draw command buffer");

    const VkSemaphore presentWaitSemaphores[] = {renderFinishedSemaphores_[imageIndex]};
    const VkSwapchainKHR swapchains[] = {swapchain_.handle()};
    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = presentWaitSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(context_.presentQueue(), &presentInfo);

    // consumeResizedFlag() must run unconditionally (not as an || operand)
    // so a pending resize is never left unconsumed by short-circuiting.
    const bool windowResized = window_.consumeResizedFlag();
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || windowResized) {
        swapchain_.recreate();
        recreateSyncObjectsForSwapchain();
    } else if (presentResult != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swapchain image");
    }

    currentFrame_ = (currentFrame_ + 1) % kFramesInFlight;
}

glm::vec3 Renderer::previewColor() const {
    return hsv2rgb(phaseSpeedDegPerSec_ * elapsedTimeSeconds_, 0.75f, 1.0f);
}

} // namespace renderer
