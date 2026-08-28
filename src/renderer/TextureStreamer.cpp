#include "TextureStreamer.h"

#include "../keel-vk/DebugUtils.h"
#include "../keel-vk/VkCheck.h"
#include "../keel-vk/VulkanContext.h"

#include <cstring>
#include <stdexcept>

namespace renderer {

TextureStreamer::TextureStreamer(keel::VulkanContext& context, VkCommandPool commandPool, uint32_t capacity,
                                  uint32_t framesInFlight)
    : context_(context), commandPool_(commandPool), capacity_(capacity), framesInFlight_(framesInFlight) {
    slots_.resize(capacity_);

    createSampler();
    createDescriptorObjects();

    for (auto& entry : stagingRing_) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = kStagingEntryBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocationInfo{};
        keel::vkCheck(vmaCreateBuffer(context_.allocator(), &bufferInfo, &allocInfo, &entry.buffer,
                                       &entry.allocation, &allocationInfo),
                      "Failed to create texture streaming ring buffer");
        entry.mapped = allocationInfo.pMappedData;
    }

    reserveDefaultSlot();
}

TextureStreamer::~TextureStreamer() {
    for (uint32_t i = 0; i < capacity_; ++i) {
        if (slots_[i].inUse) {
            vkDestroyImageView(context_.device(), slots_[i].view, nullptr);
            vmaDestroyImage(context_.allocator(), slots_[i].image, slots_[i].allocation);
        }
    }
    for (const DeferredDestroy& pending : deferredDestroys_) {
        vkDestroyImageView(context_.device(), pending.view, nullptr);
        vmaDestroyImage(context_.allocator(), pending.image, pending.allocation);
    }
    for (auto& entry : stagingRing_) {
        vmaDestroyBuffer(context_.allocator(), entry.buffer, entry.allocation);
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_.device(), descriptorPool_, nullptr); // also frees descriptorSet_
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context_.device(), descriptorSetLayout_, nullptr);
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(context_.device(), sampler_, nullptr);
    }
}

void TextureStreamer::createSampler() {
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 0.0f;
    keel::vkCheck(vkCreateSampler(context_.device(), &samplerInfo, nullptr, &sampler_),
                  "Failed to create bindless texture sampler");
}

void TextureStreamer::createDescriptorObjects() {
    // The device contract already requires update-after-bind, partially
    // bound, and variable-descriptor-count support; this is what actually
    // uses it.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = capacity_;
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

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, capacity_};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    keel::vkCheck(vkCreateDescriptorPool(context_.device(), &poolInfo, nullptr, &descriptorPool_),
                  "Failed to create bindless descriptor pool");

    VkDescriptorSetVariableDescriptorCountAllocateInfo variableCountInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    variableCountInfo.descriptorSetCount = 1;
    variableCountInfo.pDescriptorCounts = &capacity_;

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.pNext = &variableCountInfo;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout_;
    keel::vkCheck(vkAllocateDescriptorSets(context_.device(), &allocInfo, &descriptorSet_),
                  "Failed to allocate bindless descriptor set");
}

void TextureStreamer::reserveDefaultSlot() {
    static constexpr std::array<uint8_t, 4> kWhitePixel = {255, 255, 255, 255};
    createSlotImage(0, 1, 1, "bindless default (white)");
    queueUpload(0, slots_[0].generation, 1, 1, kWhitePixel.data());
}

void TextureStreamer::createSlotImage(uint32_t slotIndex, uint32_t width, uint32_t height, const char* debugName) {
    Slot& slot = slots_[slotIndex];

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    keel::vkCheck(vmaCreateImage(context_.allocator(), &imageInfo, &allocInfo, &slot.image, &slot.allocation,
                                  nullptr),
                  "Failed to create bindless texture image");
    keel::setDebugObjectName(context_.device(), VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(slot.image),
                              debugName);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = slot.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    keel::vkCheck(vkCreateImageView(context_.device(), &viewInfo, nullptr, &slot.view),
                  "Failed to create bindless texture view");

    slot.inUse = true;
}

void TextureStreamer::freeSlotResources(uint32_t slotIndex) {
    Slot& slot = slots_[slotIndex];
    if (slot.image == VK_NULL_HANDLE) {
        return;
    }
    deferredDestroys_.push_back({slot.image, slot.allocation, slot.view, frameCounter_ + framesInFlight_});
    slot.image = VK_NULL_HANDLE;
    slot.allocation = VK_NULL_HANDLE;
    slot.view = VK_NULL_HANDLE;
}

uint32_t TextureStreamer::claimStagingRingEntry(VkDeviceSize sizeBytes) {
    if (sizeBytes > kStagingEntryBytes) {
        throw std::runtime_error("TextureStreamer: texture too large for the staging ring entry size");
    }
    for (uint32_t i = 0; i < kStagingRingSize; ++i) {
        if (!stagingRing_[i].busy) {
            stagingRing_[i].busy = true;
            stagingRing_[i].retireAtFrame = 0; // not yet processed
            return i;
        }
    }
    throw std::runtime_error("TextureStreamer: staging ring exhausted (too many uploads queued at once)");
}

void TextureStreamer::reclaimStagingRing() {
    for (auto& entry : stagingRing_) {
        if (entry.busy && entry.retireAtFrame != 0 && entry.retireAtFrame <= frameCounter_) {
            entry.busy = false;
        }
    }
}

void TextureStreamer::reclaimDeferredDestroys() {
    auto it = deferredDestroys_.begin();
    while (it != deferredDestroys_.end()) {
        if (it->retireAtFrame <= frameCounter_) {
            vkDestroyImageView(context_.device(), it->view, nullptr);
            vmaDestroyImage(context_.allocator(), it->image, it->allocation);
            it = deferredDestroys_.erase(it);
        } else {
            ++it;
        }
    }
}

void TextureStreamer::queueUpload(uint32_t slotIndex, uint32_t generation, uint32_t width, uint32_t height,
                                   const void* pixels) {
    const VkDeviceSize sizeBytes = static_cast<VkDeviceSize>(width) * height * 4;
    const uint32_t ringIndex = claimStagingRingEntry(sizeBytes);
    std::memcpy(stagingRing_[ringIndex].mapped, pixels, static_cast<size_t>(sizeBytes));
    pendingUploads_.push_back({slotIndex, generation, width, height, ringIndex});
}

void TextureStreamer::updateDescriptorSlot(uint32_t slotIndex, VkImageView view) {
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler_;
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.dstArrayElement = slotIndex;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);
}

uint32_t TextureStreamer::findFreeSlotIndex() const {
    for (uint32_t i = 1; i < capacity_; ++i) { // slot 0 is the reserved default
        if (!slots_[i].inUse) {
            return i;
        }
    }
    return capacity_; // sentinel: none free
}

TextureHandle TextureStreamer::allocate(uint32_t width, uint32_t height, const void* pixelsRgba8,
                                         const char* debugName) {
    const uint32_t slotIndex = findFreeSlotIndex();
    if (slotIndex == capacity_) {
        throw std::runtime_error("TextureStreamer: no free bindless slots");
    }

    createSlotImage(slotIndex, width, height, debugName);
    queueUpload(slotIndex, slots_[slotIndex].generation, width, height, pixelsRgba8);

    return TextureHandle{slotIndex, slots_[slotIndex].generation};
}

void TextureStreamer::update(TextureHandle& handle, uint32_t width, uint32_t height, const void* pixelsRgba8,
                              const char* debugName) {
    Slot& slot = slots_[handle.slot];
    if (!slot.inUse || slot.generation != handle.generation) {
        throw std::runtime_error("TextureStreamer::update: stale texture handle");
    }

    freeSlotResources(handle.slot); // old image/view deferred-destroyed, not reused
    createSlotImage(handle.slot, width, height, debugName);
    ++slot.generation;
    queueUpload(handle.slot, slot.generation, width, height, pixelsRgba8);
    handle.generation = slot.generation;
}

void TextureStreamer::free(TextureHandle handle) {
    if (handle.slot == 0) {
        return; // default slot is never freed
    }
    Slot& slot = slots_[handle.slot];
    if (!slot.inUse || slot.generation != handle.generation) {
        return; // stale handle, no-op
    }

    freeSlotResources(handle.slot);
    slot.inUse = false;
    // Point the descriptor back at the default slot so a stale textureIndex
    // (if one is ever left pointing here) samples something defined rather
    // than whatever the next allocation into this slot turns out to be.
    updateDescriptorSlot(handle.slot, slots_[0].view);
}

void TextureStreamer::processUploads(VkCommandBuffer cmd) {
    ++frameCounter_;
    reclaimDeferredDestroys();

    if (pendingUploads_.empty()) {
        reclaimStagingRing();
        return;
    }

    std::vector<VkImageMemoryBarrier2> toTransferBarriers;
    toTransferBarriers.reserve(pendingUploads_.size());
    for (const PendingUpload& upload : pendingUploads_) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = slots_[upload.slot].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toTransferBarriers.push_back(barrier);
    }
    VkDependencyInfo toTransferDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toTransferDependency.imageMemoryBarrierCount = static_cast<uint32_t>(toTransferBarriers.size());
    toTransferDependency.pImageMemoryBarriers = toTransferBarriers.data();
    vkCmdPipelineBarrier2(cmd, &toTransferDependency);

    std::vector<VkImageMemoryBarrier2> toShaderReadBarriers;
    toShaderReadBarriers.reserve(pendingUploads_.size());
    for (const PendingUpload& upload : pendingUploads_) {
        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copyRegion.imageExtent = {upload.width, upload.height, 1};
        vkCmdCopyBufferToImage(cmd, stagingRing_[upload.ringIndex].buffer, slots_[upload.slot].image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = slots_[upload.slot].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toShaderReadBarriers.push_back(barrier);

        stagingRing_[upload.ringIndex].retireAtFrame = frameCounter_ + framesInFlight_;
    }
    VkDependencyInfo toShaderReadDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toShaderReadDependency.imageMemoryBarrierCount = static_cast<uint32_t>(toShaderReadBarriers.size());
    toShaderReadDependency.pImageMemoryBarriers = toShaderReadBarriers.data();
    vkCmdPipelineBarrier2(cmd, &toShaderReadDependency);

    for (const PendingUpload& upload : pendingUploads_) {
        updateDescriptorSlot(upload.slot, slots_[upload.slot].view);
    }

    pendingUploads_.clear();
    reclaimStagingRing();
}

uint32_t TextureStreamer::usedSlots() const {
    uint32_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.inUse) {
            ++count;
        }
    }
    return count;
}

} // namespace renderer
