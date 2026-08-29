#include "Renderer.h"

#include "../keel-vk/DebugUtils.h"
#include "../keel-vk/ShaderModule.h"
#include "../keel-vk/Swapchain.h"
#include "../keel-vk/VkCheck.h"
#include "../keel-vk/VulkanContext.h"
#include "../keel-vk/Window.h"
#include "Frustum.h"
#include "TextureArray2D.h"

#if KEEL_VK_IMGUI
#include "../debug/DebugUi.h"
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace renderer {

namespace {

// Per-object data (model matrix, texture index) lives in the instance
// SSBO; only camera- and time-derived values that are the same for
// every instance in this draw stay as push constants.
struct PushConstants {
    glm::mat4 viewProj;
    float time;
    float phaseSpeed;
};

// mat4 (64) + vec4 (16) + vec4 (16) + 4 x uint32 (16) = 112 bytes,
// matching cube.vert's std430 Instance struct field for field.
// scalarBlockLayout is part of the required device contract, but this
// still keeps natural 16-byte alignment throughout rather than relying
// on it. textureKind selects which of the three residency paths
// textureIndex/atlasUvRect should be read as - see TextureRef.h.
struct GpuInstance {
    glm::mat4 model;
    glm::vec4 boundsCenterRadius;
    glm::vec4 atlasUvRect;
    uint32_t textureKind;
    uint32_t textureIndex;
    uint32_t visible;
    uint32_t generation;
};

struct DeviceMemoryBudget {
    VkDeviceSize budgetBytes = 0;
    VkDeviceSize usageBytes = 0;
};

// Sums every DEVICE_LOCAL heap's VmaBudget. VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT
// (set in VulkanContext when VK_EXT_memory_budget is present) makes
// budget/usage reflect the driver's own numbers instead of VMA's
// heap-size estimate; either way this is real device memory state, not
// the small artificial cap maybeEvictDemoTexture actually triggers on.
DeviceMemoryBudget queryDeviceLocalBudget(keel::VulkanContext& context) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(context.physicalDevice(), &memProps);

    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(context.allocator(), budgets.data());

    DeviceMemoryBudget result{};
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            result.budgetBytes += budgets[i].budget;
            result.usageBytes += budgets[i].usage;
        }
    }
    return result;
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

// Used by regenerateActiveTexture()/freeAndReallocateSpareTexture() below
// to give the overlay's demo controls something to write - a small,
// generic pixel generator, not contract-test content itself (compare
// src/client/ContractTest.cpp's checker/stripes/gradient generators,
// which are).
std::vector<uint8_t> makeSolidPattern(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }
    return pixels;
}

} // namespace

Renderer::Renderer(keel::VulkanContext& context, keel::Swapchain& swapchain, keel::Window& window)
    : context_(context), swapchain_(swapchain), window_(window) {
    // Generic default: looking at the origin from a fixed distance along
    // +Z, no input-driven controller (see the wiki's Extending page).
    // Not tuned for any particular scene - src/client/ sets its own eye
    // point through the same public camera()/cameraDistance() accessors
    // the debug overlay already uses. cameraDistance_ (a debug "dolly"
    // slider, see recordWorldPass) scales along cameraHomeDirection_
    // every frame; the direction itself never changes here.
    camera_.position = glm::vec3(0.0f, 0.0f, 3.0f);
    camera_.front = glm::normalize(-camera_.position);
    cameraHomeDirection_ = glm::normalize(camera_.position);
    cameraDistance_ = glm::length(camera_.position);

    createCommandPool();
    createUploadCommandPool();
    createCommandBuffers();
    createSyncObjects();
    createTimestampPool();
    createUploadTimelineSemaphore();
    createMeshPool();          // empty until a caller's allocateMesh() call
    createInstanceResources();
    createTextureStreamer();   // just the streamer's own resident default; no demo content preloaded
    textureArray_ =
        std::make_unique<TextureArray2D>(context_, uploadCommandPool_, 64, 16, uploadTimelineSemaphore_, 1);
    // Empty by default: no entries packed, atlasRect() returns AtlasRect{}
    // until src/client/ registers something worth an atlas page. The
    // shelf packer and UV table still exist and still work - see the
    // wiki's Extending page for how a fork would add content here.
    atlas_ = std::make_unique<Atlas2D>(context_, uploadCommandPool_, 256, std::vector<AtlasEntry>{},
                                        uploadTimelineSemaphore_, 2);
    createResidencyDescriptorSet();
    createDepthTarget();
    createPipeline();
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(context_.device());

    savePipelineCache();
    if (pipelineCache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(context_.device(), pipelineCache_, nullptr);
    }
    if (timestampPool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(context_.device(), timestampPool_, nullptr);
    }

    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_.device(), pipeline_, nullptr);
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(context_.device(), pipelineLayout_, nullptr);
    }

    destroyDepthTarget();
    if (residencyDescriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_.device(), residencyDescriptorPool_, nullptr); // also frees residencySet_
    }
    if (residencySetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context_.device(), residencySetLayout_, nullptr);
    }
    atlas_.reset();
    textureArray_.reset();
    textureStreamer_.reset();

    destroyInstanceResources();
    meshPool_.reset();

    destroySyncObjects();
    if (uploadTimelineSemaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(context_.device(), uploadTimelineSemaphore_, nullptr);
    }
    if (uploadCommandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(context_.device(), uploadCommandPool_, nullptr);
    }
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

void Renderer::createUploadCommandPool() {
    // A separate pool from commandPool_, not because the queue family
    // differs (it doesn't, see VulkanContext::uploadQueueFamily), but to
    // keep construction-time throwaway command buffers out of the pool
    // the per-frame reused ones live in.
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context_.uploadQueueFamily();

    keel::vkCheck(vkCreateCommandPool(context_.device(), &poolInfo, nullptr, &uploadCommandPool_),
                  "Failed to create upload command pool");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_COMMAND_POOL,
                              reinterpret_cast<uint64_t>(uploadCommandPool_), "upload command pool");
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

void Renderer::createTimestampPool() {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(context_.physicalDevice(), &props);
    // Not part of the required device contract: nearly universal on
    // desktop GPUs, but a device could conformantly lack it.
    timestampsSupported_ = props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0.0f;
    if (!timestampsSupported_) {
        return;
    }
    timestampPeriodNs_ = props.limits.timestampPeriod;

    VkQueryPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = static_cast<uint32_t>(kFramesInFlight) * 2; // start + end per frame in flight
    keel::vkCheck(vkCreateQueryPool(context_.device(), &poolInfo, nullptr, &timestampPool_),
                  "Failed to create timestamp query pool");
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

// Host-visible mapped, persistently: every buffer this file creates below
// is either written once (mesh pool) or rewritten every frame (instance /
// indirect), never staged through a device-local copy, so there is no
// benefit to VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE here.
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

void Renderer::createMeshPool() {
    // Capacity is a scaffold, not a measurement: room for more than a
    // single small mesh, so MeshPool::allocate has real headroom to
    // demonstrate subrange allocation rather than exactly fitting one
    // caller. Empty until a caller's allocateMesh() call - see the wiki's
    // Extending page. firstSignalValue starts at 3, strictly after
    // kResidencyConstructionUploadCount (TextureArray2D=1, Atlas2D=2 -
    // see Renderer.h), since allocateMesh() may be called any number of
    // times after construction and must never collide with those two
    // fixed values.
    meshPool_ = std::make_unique<MeshPool>(context_, uploadCommandPool_, uploadTimelineSemaphore_, 3, 8192, 16384,
                                            sizeof(Vertex));
}

MeshRange Renderer::allocateMesh(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices,
                                  uint32_t indexCount) {
    activeMesh_ = meshPool_->allocate(vertices, vertexCount, indices, indexCount);
    return activeMesh_;
}

void Renderer::setInstances(const std::vector<InstanceDesc>& instances) {
    if (instances.size() > kMaxInstances) {
        throw std::runtime_error("Renderer::setInstances: exceeds kMaxInstances");
    }
    instances_ = instances;
}

void Renderer::createInstanceResources() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    keel::vkCheck(vkCreateDescriptorSetLayout(context_.device(), &layoutInfo, nullptr, &instanceSetLayout_),
                  "Failed to create instance descriptor set layout");

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<uint32_t>(kFramesInFlight)};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = static_cast<uint32_t>(kFramesInFlight);
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    keel::vkCheck(vkCreateDescriptorPool(context_.device(), &poolInfo, nullptr, &instanceDescriptorPool_),
                  "Failed to create instance descriptor pool");

    for (int i = 0; i < kFramesInFlight; ++i) {
        InstanceFrame& frame = instanceFrames_[static_cast<size_t>(i)];

        const std::string instanceName = "instance buffer " + std::to_string(i);
        frame.instanceBuffer =
            createMappedBuffer(context_, sizeof(GpuInstance) * kMaxInstances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                frame.instanceAllocation, frame.instanceMapped, instanceName.c_str());

        const std::string indirectName = "indirect buffer " + std::to_string(i);
        frame.indirectBuffer = createMappedBuffer(context_, sizeof(VkDrawIndexedIndirectCommand) * kMaxInstances,
                                                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, frame.indirectAllocation,
                                                    frame.indirectMapped, indirectName.c_str());

        VkDescriptorSetAllocateInfo setAllocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAllocInfo.descriptorPool = instanceDescriptorPool_;
        setAllocInfo.descriptorSetCount = 1;
        setAllocInfo.pSetLayouts = &instanceSetLayout_;
        keel::vkCheck(vkAllocateDescriptorSets(context_.device(), &setAllocInfo, &frame.instanceSet),
                      "Failed to allocate instance descriptor set");

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = frame.instanceBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(GpuInstance) * kMaxInstances;

        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = frame.instanceSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);
    }
}

void Renderer::destroyInstanceResources() {
    for (InstanceFrame& frame : instanceFrames_) {
        if (frame.indirectBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(context_.allocator(), frame.indirectBuffer, frame.indirectAllocation);
        }
        if (frame.instanceBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(context_.allocator(), frame.instanceBuffer, frame.instanceAllocation);
        }
    }
    if (instanceDescriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_.device(), instanceDescriptorPool_, nullptr);
    }
    if (instanceSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context_.device(), instanceSetLayout_, nullptr);
    }
}

void Renderer::createUploadTimelineSemaphore() {
    VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphoreInfo.pNext = &typeInfo;
    keel::vkCheck(vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &uploadTimelineSemaphore_),
                  "Failed to create upload timeline semaphore");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_SEMAPHORE,
                              reinterpret_cast<uint64_t>(uploadTimelineSemaphore_), "upload timeline semaphore");
}

void Renderer::createTextureStreamer() {
    // kMaxBindlessTextures is a scaffold width, not a guarantee: a device
    // could conformantly report a smaller
    // maxDescriptorSetUpdateAfterBindSampledImages/per-stage limit. Shrink
    // to whatever the device actually supports rather than creating a
    // descriptor set layout the device can't allocate.
    bindlessCapacity_ = std::min(kMaxBindlessTextures, context_.maxBindlessSampledImages());
    textureStreamer_ = std::make_unique<TextureStreamer>(context_, commandPool_, bindlessCapacity_, kFramesInFlight);
    // No demo content preloaded: registerDemoTexture()/registerDemoTextureCompressed()
    // are how a caller adds any. Even the streamer's own resident default
    // (slot 0, queued inside TextureStreamer's constructor) isn't flushed
    // here with a special one-time command buffer - the normal per-frame
    // processUploads() call in recordCommandBuffer, which always runs
    // before anything that might sample a slot, covers it exactly the
    // same way it covers every later allocate()/update()/free() call.
}

TextureHandle Renderer::registerDemoTexture(uint32_t width, uint32_t height, const void* pixelsRgba8,
                                             const char* debugName) {
    demoTextures_.push_back(textureStreamer_->allocate(width, height, pixelsRgba8, debugName));
    demoTextureBytes_.push_back(static_cast<VkDeviceSize>(width) * height * 4);
    demoTextureLastUsedFrame_.push_back(demoTextures_.size() - 1); // deterministic creation order, ties break by insertion
    return demoTextures_.back();
}

TextureHandle Renderer::registerDemoTextureCompressed(uint32_t width, uint32_t height, VkFormat format,
                                                        const void* data, size_t dataSize, const char* debugName) {
    demoTextures_.push_back(textureStreamer_->allocateCompressed(width, height, format, data, dataSize, debugName));
    demoTextureBytes_.push_back(static_cast<VkDeviceSize>(dataSize));
    demoTextureLastUsedFrame_.push_back(demoTextures_.size() - 1);
    return demoTextures_.back();
}

void Renderer::createResidencyDescriptorSet() {
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<uint32_t>(std::size(bindings));
    layoutInfo.pBindings = bindings;
    keel::vkCheck(vkCreateDescriptorSetLayout(context_.device(), &layoutInfo, nullptr, &residencySetLayout_),
                  "Failed to create residency descriptor set layout");

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    keel::vkCheck(vkCreateDescriptorPool(context_.device(), &poolInfo, nullptr, &residencyDescriptorPool_),
                  "Failed to create residency descriptor pool");

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = residencyDescriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &residencySetLayout_;
    keel::vkCheck(vkAllocateDescriptorSets(context_.device(), &allocInfo, &residencySet_),
                  "Failed to allocate residency descriptor set");

    // Written once: both images are populated once at construction, never
    // streamed, so there's nothing to update after this.
    VkDescriptorImageInfo arrayImageInfo{};
    arrayImageInfo.sampler = textureArray_->sampler();
    arrayImageInfo.imageView = textureArray_->view();
    arrayImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo atlasImageInfo{};
    atlasImageInfo.sampler = atlas_->sampler();
    atlasImageInfo.imageView = atlas_->view();
    atlasImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = residencySet_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &arrayImageInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = residencySet_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &atlasImageInfo;
    vkUpdateDescriptorSets(context_.device(), static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
}

void Renderer::regenerateActiveTexture() {
    if (activeDemoTextureIndex_ < 0 || activeDemoTextureIndex_ >= static_cast<int>(demoTextures_.size())) {
        return;
    }
    ++regenerateCounter_;
    const glm::vec3 color =
        hsv2rgb(static_cast<float>((regenerateCounter_ * 47) % 360), 0.75f, 1.0f); // 47: an arbitrary non-divisor of 360
    const std::vector<uint8_t> pixels =
        makeSolidPattern(16, 16, static_cast<uint8_t>(color.r * 255.0f), static_cast<uint8_t>(color.g * 255.0f),
                          static_cast<uint8_t>(color.b * 255.0f));
    textureStreamer_->update(demoTextures_[static_cast<size_t>(activeDemoTextureIndex_)], 16, 16, pixels.data(),
                              "regenerated (streamed)");
    demoTextureBytes_[static_cast<size_t>(activeDemoTextureIndex_)] = 16 * 16 * 4;
    demoTextureLastUsedFrame_[static_cast<size_t>(activeDemoTextureIndex_)] = frameCounter_;
}

void Renderer::freeAndReallocateSpareTexture() {
    // Index into demoTextures_ by position, same as the "regenerate" and
    // "residency picker" controls - eviction (maybeEvictDemoTexture) can
    // shift what's actually at this position, same as it can for any
    // other index-based demo control. Debug-overlay convenience button,
    // not a stable slot identity.
    constexpr size_t kSpareIndex = 3; // see createTextureStreamer: checker, stripes, gradient, spare
    if (demoTextures_.size() <= kSpareIndex) {
        return;
    }
    textureStreamer_->free(demoTextures_[kSpareIndex]);
    ++regenerateCounter_;
    const glm::vec3 color = hsv2rgb(static_cast<float>((regenerateCounter_ * 83) % 360), 0.6f, 0.9f);
    const std::vector<uint8_t> pixels =
        makeSolidPattern(8, 8, static_cast<uint8_t>(color.r * 255.0f), static_cast<uint8_t>(color.g * 255.0f),
                          static_cast<uint8_t>(color.b * 255.0f));
    demoTextures_[kSpareIndex] = textureStreamer_->allocate(8, 8, pixels.data(), "spare (reallocated)");
    demoTextureBytes_[kSpareIndex] = 8 * 8 * 4;
    demoTextureLastUsedFrame_[kSpareIndex] = frameCounter_;
}

bool Renderer::memoryBudgetSupported() const {
    return context_.memoryBudgetSupported();
}

uint64_t Renderer::deviceMemoryBudgetBytes() const {
    return queryDeviceLocalBudget(context_).budgetBytes;
}

uint64_t Renderer::deviceMemoryUsageBytes() const {
    return queryDeviceLocalBudget(context_).usageBytes;
}

VkDeviceSize Renderer::demoResidentBytes() const {
    VkDeviceSize total = 0;
    for (VkDeviceSize bytes : demoTextureBytes_) {
        total += bytes;
    }
    return total;
}

void Renderer::maybeEvictDemoTexture() {
    if (demoTextures_.empty()) {
        return;
    }

    bool shouldEvict = false;
#ifdef KEEL_VK_DEBUG
    // Debug-only: an artificially tiny cap so eviction is something a
    // developer can actually trigger and watch happen without first
    // filling gigabytes of real VRAM. Never active outside a Debug
    // build - see the wiki's Rendering page for why 2048 is deliberately
    // unrelated to any real budget number.
    shouldEvict = demoResidentBytes() > kDemoResidentCapBytes;
#endif
    if (!shouldEvict && memoryBudgetSupported()) {
        // The real trigger, gated on VK_EXT_memory_budget actually being
        // present: eviction only fires here if this demo's few kilobytes
        // of content somehow pushed a real device at or over its real
        // budget, which never happens at this content's scale - this
        // path exists to be correct if it ever did, not to be visibly
        // exercised by this contract test. Without the extension there
        // is no real number to trigger on, so nothing here pretends
        // there is (see the overlay's "VRAM: N/A" line).
        shouldEvict = deviceMemoryUsageBytes() >= deviceMemoryBudgetBytes();
    }
    if (!shouldEvict) {
        return;
    }

    // activeDemoTextureIndex()'s current entry is the only protected one
    // (slot 0, the streamer's own white default, is never in
    // demoTextures_ at all - see createTextureStreamer). Everything else
    // is eviction-eligible; a caller indexing demoTextures_ by position
    // for its own instances (as ContractTest.cpp's satellites do,
    // re-deriving their index from demoTextures_.size() every frame)
    // just sees a reshuffled rotation after an eviction, not a dangling
    // handle.
    const bool activeIndexProtects = demoResidencyKind_ == TextureKind::Bindless && activeDemoTextureIndex_ >= 0 &&
                                      static_cast<size_t>(activeDemoTextureIndex_) < demoTextures_.size();
    const size_t protectedIndex =
        activeIndexProtects ? static_cast<size_t>(activeDemoTextureIndex_) : demoTextures_.size();

    size_t oldestIndex = demoTextures_.size();
    uint64_t oldestFrame = UINT64_MAX;
    for (size_t i = 0; i < demoTextures_.size(); ++i) {
        if (i == protectedIndex) {
            continue;
        }
        if (demoTextureLastUsedFrame_[i] < oldestFrame) {
            oldestFrame = demoTextureLastUsedFrame_[i];
            oldestIndex = i;
        }
    }
    if (oldestIndex == demoTextures_.size()) {
        return; // nothing eligible (only the protected entry remains)
    }

    textureStreamer_->free(demoTextures_[oldestIndex]);
    demoTextures_.erase(demoTextures_.begin() + static_cast<std::ptrdiff_t>(oldestIndex));
    demoTextureBytes_.erase(demoTextureBytes_.begin() + static_cast<std::ptrdiff_t>(oldestIndex));
    demoTextureLastUsedFrame_.erase(demoTextureLastUsedFrame_.begin() + static_cast<std::ptrdiff_t>(oldestIndex));
    ++evictionCount_;

    if (activeDemoTextureIndex_ >= static_cast<int>(demoTextures_.size())) {
        activeDemoTextureIndex_ = demoTextures_.empty() ? 0 : static_cast<int>(demoTextures_.size()) - 1;
    }
}

uint32_t Renderer::textureArrayLayerCount() const {
    return textureArray_->layerCount();
}

uint32_t Renderer::activeDemoTextureSlot() const {
    const bool hasActive =
        activeDemoTextureIndex_ >= 0 && activeDemoTextureIndex_ < static_cast<int>(demoTextures_.size());
    return hasActive ? demoTextures_[static_cast<size_t>(activeDemoTextureIndex_)].slot : 0;
}

uint32_t Renderer::demoTextureSlotAt(uint32_t index) const {
    return demoTextures_.empty() ? 0 : demoTextures_[index % demoTextures_.size()].slot;
}

AtlasRect Renderer::atlasRect(uint32_t index) const {
    const std::vector<AtlasRect>& rects = atlas_->rects();
    return rects.empty() ? AtlasRect{} : rects[index % rects.size()];
}

uint32_t Renderer::atlasRectCount() const {
    return static_cast<uint32_t>(atlas_->rects().size());
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

void Renderer::loadPipelineCache() {
    const char* basePath = SDL_GetBasePath();
    const std::string path = (basePath ? std::string(basePath) : std::string()) + "pipeline_cache.bin";

    std::vector<char> data;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        const std::streamsize size = file.tellg();
        if (size > 0) {
            data.resize(static_cast<size_t>(size));
            file.seekg(0);
            file.read(data.data(), size);
        }
    }

    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cacheInfo.initialDataSize = data.size();
    cacheInfo.pInitialData = data.empty() ? nullptr : data.data();
    // An empty/mismatched-device cache is silently ignored by the driver
    // (falls back to building from scratch), so no validity check is
    // needed here beyond "did the file exist".
    keel::vkCheck(vkCreatePipelineCache(context_.device(), &cacheInfo, nullptr, &pipelineCache_),
                  "Failed to create pipeline cache");
}

void Renderer::savePipelineCache() {
    if (pipelineCache_ == VK_NULL_HANDLE) {
        return;
    }
    size_t size = 0;
    vkGetPipelineCacheData(context_.device(), pipelineCache_, &size, nullptr);
    if (size == 0) {
        return;
    }
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(context_.device(), pipelineCache_, &size, data.data()) != VK_SUCCESS) {
        return;
    }

    const char* basePath = SDL_GetBasePath();
    const std::string path = (basePath ? std::string(basePath) : std::string()) + "pipeline_cache.bin";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file.is_open()) {
        file.write(data.data(), static_cast<std::streamsize>(size));
    }
}

void Renderer::createPipeline() {
    loadPipelineCache();

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

    // Reverse-Z: near is 1.0, far (infinity) is 0.0 (see frame::Camera::
    // projection), depth clears to 0.0 below, so "closer" means "greater".
    // GREATER_OR_EQUAL, not GREATER, so a fragment exactly at the clear
    // value (nothing drawn there yet) still compares correctly at the
    // first draw into a pixel.
    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;

    // viewProj/time/phaseSpeed are read in the vertex shader only: per-
    // instance data (model, texture index) lives in the instance SSBO,
    // so the fragment shader doesn't touch push constants at all (it
    // reads the texture index cube.vert forwards as a varying).
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    // set 0: the bindless texture array (TextureStreamer). set 1: the
    // per-instance SSBO, one descriptor set per frame in flight, bound at
    // draw time in recordWorldPass. set 2: the array/atlas samplers
    // (createResidencyDescriptorSet), written once.
    const VkDescriptorSetLayout bindlessSetLayout = textureStreamer_->descriptorSetLayout();
    const VkDescriptorSetLayout setLayouts[] = {bindlessSetLayout, instanceSetLayout_, residencySetLayout_};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = static_cast<uint32_t>(std::size(setLayouts));
    layoutInfo.pSetLayouts = setLayouts;
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

    keel::vkCheck(vkCreateGraphicsPipelines(context_.device(), pipelineCache_, 1, &pipelineInfo, nullptr, &pipeline_),
                  "Failed to create graphics pipeline");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline_),
                              "cube pipeline");
}

void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, debug::DebugUi* debugUi) {
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    keel::vkCheck(vkBeginCommandBuffer(cmd, &beginInfo), "Failed to begin command buffer");

    // Drains any allocate()/update()/free() calls queued this frame (e.g.
    // from the debug overlay) before anything below might sample the slot
    // they touch. Never blocks: no vkQueueWaitIdle anywhere in this path.
    textureStreamer_->processUploads(cmd);

    if (timestampsSupported_) {
        // Reset before write, not read: the corresponding
        // vkGetQueryPoolResults call happens in drawFrame(), after this
        // frame slot's fence wait guarantees the previous use of these two
        // queries has completed.
        vkCmdResetQueryPool(cmd, timestampPool_, currentFrame_ * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampPool_, currentFrame_ * 2 + 0);
        timestampSlotReady_[currentFrame_] = true;
    }

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
    depthAttachment.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: far, not near

    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);

    // Explicit pass order for this frame. Acquire (image acquire + fence
    // wait) already happened in drawFrame(), before this command buffer
    // started recording; Present (the post-barrier below + the eventual
    // vkQueuePresentKHR) happens after. See Renderer.h for why this is a
    // named list of steps and not a frame-graph.
    recordWorldPass(cmd, extent);
    recordComputePass(cmd);
    recordOverlayPass(cmd, debugUi);

    vkCmdEndRendering(cmd);

    if (timestampsSupported_) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampPool_, currentFrame_ * 2 + 1);
    }

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

void Renderer::recordWorldPass(VkCommandBuffer cmd, VkExtent2D extent) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                               0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Dolly: scale along the fixed home direction, don't change it. A
    // debug slider, not free-fly input - see the wiki's Rendering page
    // for why dollying in is what actually demonstrates the frustum cull
    // below (a fixed angular FOV covers fewer world units the closer the
    // camera gets, so the satellite ring's edges start exceeding it).
    camera_.position = cameraHomeDirection_ * cameraDistance_;
    camera_.front = glm::normalize(-camera_.position);

    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    const glm::mat4 viewProj = camera_.projection(aspect) * camera_.view();

    PushConstants pushConstants{};
    pushConstants.viewProj = viewProj;
    pushConstants.time = elapsedTimeSeconds_;
    pushConstants.phaseSpeed = phaseSpeedDegPerSec_;
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConstants), &pushConstants);

    // Demo-texture eviction bookkeeping: marks the overlay's currently
    // previewed Bindless slot as freshly used, then frees the oldest
    // unused entry if total demo resident bytes are over the cap. Runs
    // before the instance-writing loop below so anything indexing
    // demoTextures_ by position (as ContractTest.cpp's satellites do)
    // sees the post-eviction state this same frame, not a stale index.
    // Independent of instances_ - this bookkeeping happens whether or not
    // anything actually samples that slot this frame.
    ++frameCounter_;
    const bool hasActiveDemoTexture =
        activeDemoTextureIndex_ >= 0 && activeDemoTextureIndex_ < static_cast<int>(demoTextures_.size());
    if (demoResidencyKind_ == TextureKind::Bindless && hasActiveDemoTexture) {
        demoTextureLastUsedFrame_[static_cast<size_t>(activeDemoTextureIndex_)] = frameCounter_;
    }
    maybeEvictDemoTexture();

    // Write this frame's instances, exactly as setInstances() last set
    // them. Renderer doesn't know or care what they represent - see
    // InstanceDesc's comment and the wiki's Extending page. The buffer,
    // cull loop, and compaction are sized for a larger population than
    // any single caller necessarily uses (kMaxInstances) - see the wiki's
    // Rendering page for why that scaffolding exists.
    InstanceFrame& frame = instanceFrames_[static_cast<size_t>(currentFrame_)];
    auto* instances = static_cast<GpuInstance*>(frame.instanceMapped);
    const uint32_t writtenInstanceCount = static_cast<uint32_t>(instances_.size());
    for (uint32_t i = 0; i < writtenInstanceCount; ++i) {
        const InstanceDesc& desc = instances_[i];
        // desc.model is world-space; origin has to be subtracted from it
        // the same way camera_ already subtracts it from the eye point in
        // Camera::view(), or the object and the camera would drift apart
        // as origin moves.
        const glm::mat4 originAdjustedModel = glm::translate(glm::mat4(1.0f), -camera_.origin) * desc.model;
        instances[i].model = originAdjustedModel;
        instances[i].boundsCenterRadius = glm::vec4(glm::vec3(originAdjustedModel[3]), desc.boundsRadius);
        instances[i].textureKind = static_cast<uint32_t>(desc.texture.kind);
        instances[i].textureIndex = desc.texture.index;
        instances[i].atlasUvRect = desc.texture.atlasUvRect;
        instances[i].visible = 1;
        instances[i].generation = 0;
    }

    // CPU frustum cull against each instance's bounding sphere, compacting
    // surviving draws to the front of the indirect buffer rather than
    // zeroing culled entries in place: vkCmdDrawIndexedIndirect below is
    // issued with exactly the surviving count, so a culled instance costs
    // nothing on the GPU side, not even a zero-instanceCount draw. No
    // Hi-Z, no occlusion query - bounds-only, CPU-side. Stays CPU-side
    // rather than a compute pass at this instance count; see the wiki's
    // Rendering page for the decision and why.
    const uint64_t cullStartTicks = SDL_GetPerformanceCounter();
    const Frustum frustum(viewProj);
    auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(frame.indirectMapped);
    uint32_t triangleCount = 0;
    uint32_t drawCount = 0;
    for (uint32_t i = 0; i < writtenInstanceCount; ++i) {
        if (!instances[i].visible) {
            continue;
        }
        const glm::vec3 center(instances[i].boundsCenterRadius);
        const float radius = instances[i].boundsCenterRadius.w;
        if (!frustum.intersectsSphere(center, radius)) {
            continue;
        }
        VkDrawIndexedIndirectCommand& command = commands[drawCount];
        command.indexCount = activeMesh_.indexCount;
        command.instanceCount = 1;
        command.firstIndex = activeMesh_.indexOffset;
        command.vertexOffset = static_cast<int32_t>(activeMesh_.vertexOffset);
        command.firstInstance = i; // slot index, read back by cube.vert via gl_InstanceIndex
        ++drawCount;
        triangleCount += activeMesh_.indexCount / 3;
    }
    const uint64_t cullEndTicks = SDL_GetPerformanceCounter();
    lastCullTimeMs_ = static_cast<float>(cullEndTicks - cullStartTicks) * 1000.0f /
                       static_cast<float>(SDL_GetPerformanceFrequency());
    lastInstanceCount_ = writtenInstanceCount;
    lastDrawCount_ = drawCount;
    lastTriangleCount_ = triangleCount;

    const VkDescriptorSet sets[] = {textureStreamer_->descriptorSet(), frame.instanceSet, residencySet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0,
                             static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);

    const VkDeviceSize offset = 0;
    const VkBuffer vertexBuffer = meshPool_->vertexBuffer();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, meshPool_->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    if (drawCount > 0) {
        vkCmdDrawIndexedIndirect(cmd, frame.indirectBuffer, 0, drawCount, sizeof(VkDrawIndexedIndirectCommand));
    }
}

void Renderer::recordComputePass(VkCommandBuffer cmd) {
    static_cast<void>(cmd);
}

void Renderer::recordOverlayPass(VkCommandBuffer cmd, debug::DebugUi* debugUi) {
#if KEEL_VK_IMGUI
    if (debugUi) {
        debugUi->render(cmd);
    }
#else
    static_cast<void>(cmd);
    static_cast<void>(debugUi);
#endif
}

void Renderer::drawFrame(debug::DebugUi* debugUi) {
    // A 0x0 framebuffer (typically a minimized window) can't back a
    // valid swapchain image or viewport - VkViewport requires width/height
    // > 0. Skip the frame entirely rather than recording invalid Vulkan
    // calls. main.cpp already skips its own sim-time advance and never
    // calls drawFrame() at all in this case; this guard is what keeps a
    // caller that doesn't replicate that check safe instead of hitting
    // undefined behavior on minimize.
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    window_.getFramebufferSize(framebufferWidth, framebufferHeight);
    if (framebufferWidth == 0 || framebufferHeight == 0) {
        return;
    }

    const VkDevice device = context_.device();

    vkWaitForFences(device, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

    // This frame slot's previous pair of timestamp writes is now
    // guaranteed complete - except on this slot's very first use, when the
    // queries have never been reset (an uninitialized query is a
    // validation error to read, not just VK_NOT_READY).
    if (timestampsSupported_ && timestampSlotReady_[currentFrame_]) {
        uint64_t timestamps[2] = {0, 0};
        const VkResult queryResult =
            vkGetQueryPoolResults(device, timestampPool_, currentFrame_ * 2, 2, sizeof(timestamps), timestamps,
                                   sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (queryResult == VK_SUCCESS) {
            lastGpuFrameMs_ = static_cast<float>(timestamps[1] - timestamps[0]) * timestampPeriodNs_ / 1.0e6f;
        }
    }

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

    VkSemaphoreSubmitInfo waitSemaphoreInfos[2]{};
    waitSemaphoreInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitSemaphoreInfos[0].semaphore = imageAvailableSemaphores_[currentFrame_];
    waitSemaphoreInfos[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    uint32_t waitSemaphoreCount = 1;

    // Retires construction-time GPU uploads (TextureArray2D, Atlas2D,
    // and whatever allocateMesh() has signaled so far - see
    // kResidencyConstructionUploadCount's comment in Renderer.h) on the
    // GPU side, once per distinct target value, before a submission that
    // might read any of them. Recomputed every frame rather than only
    // once: a setup-time allocateMesh() call can happen any time between
    // construction and the first drawFrame(), and this way it's covered
    // regardless of exactly when. Once the target stops growing (the
    // normal steady state), this adds nothing to the submit.
    const uint64_t currentUploadTarget =
        std::max<uint64_t>(kResidencyConstructionUploadCount, meshPool_->lastSignaledUploadValue());
    if (currentUploadTarget > lastWaitedUploadValue_) {
        waitSemaphoreInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitSemaphoreInfos[1].semaphore = uploadTimelineSemaphore_;
        waitSemaphoreInfos[1].value = currentUploadTarget;
        waitSemaphoreInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        waitSemaphoreCount = 2;
        lastWaitedUploadValue_ = currentUploadTarget;
    }

    VkSemaphoreSubmitInfo signalSemaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalSemaphoreInfo.semaphore = renderFinishedSemaphores_[imageIndex];
    signalSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo commandBufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandBufferInfo.commandBuffer = commandBuffers_[currentFrame_];

    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.waitSemaphoreInfoCount = waitSemaphoreCount;
    submitInfo.pWaitSemaphoreInfos = waitSemaphoreInfos;
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
