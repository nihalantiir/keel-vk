#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <optional>

namespace keel {

class Window;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    // A queue family with TRANSFER but not GRAPHICS: present on most
    // discrete GPUs, absent on many integrated ones. Optional by design;
    // callers doing a one-shot upload should prefer it when present and
    // fall back to the graphics queue otherwise (see
    // VulkanContext::uploadQueue()).
    std::optional<uint32_t> dedicatedTransferFamily;
    bool isComplete() const { return graphicsFamily.has_value() && presentFamily.has_value(); }
};

class VulkanContext {
public:
    explicit VulkanContext(Window& window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance instance() const { return instance_; }
    VkSurfaceKHR surface() const { return surface_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    VkQueue presentQueue() const { return presentQueue_; }
    VmaAllocator allocator() const { return allocator_; }
    const QueueFamilyIndices& queueFamilies() const { return queueFamilyIndices_; }

    // For one-shot uploads only (see the wiki's Rendering page): the
    // dedicated transfer queue if the device has one, the graphics queue
    // otherwise. Never used for the per-frame streaming path, which stays
    // on the graphics queue/command buffer to avoid cross-queue
    // synchronization on the hot path.
    bool hasDedicatedTransferQueue() const { return queueFamilyIndices_.dedicatedTransferFamily.has_value(); }
    VkQueue uploadQueue() const { return hasDedicatedTransferQueue() ? transferQueue_ : graphicsQueue_; }
    uint32_t uploadQueueFamily() const {
        return hasDedicatedTransferQueue() ? queueFamilyIndices_.dedicatedTransferFamily.value()
                                            : queueFamilyIndices_.graphicsFamily.value();
    }

    // Whether VK_EXT_memory_budget was found and enabled (see the device
    // contract's "enabled if present, never required" list). When true,
    // VmaBudget::budget/usage (queried via vmaGetHeapBudgets, since
    // VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT is set alongside this)
    // reflect the driver's actual reported numbers instead of VMA's own
    // heap-size estimate.
    bool memoryBudgetSupported() const { return memoryBudgetSupported_; }

    // The device's real cap on the bindless sampled-image array
    // (min of maxDescriptorSetUpdateAfterBindSampledImages and the
    // matching per-stage limit, both from
    // VkPhysicalDeviceDescriptorIndexingProperties), queried at device
    // pick time. TextureStreamer sizes its array to
    // min(its own scaffold width, this), never past what the device
    // actually reports.
    uint32_t maxBindlessSampledImages() const { return maxBindlessSampledImages_; }

private:
    void createInstance(Window& window);
    void setupDebugMessenger();
    void createSurface(Window& window);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator();
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkQueue transferQueue_ = VK_NULL_HANDLE; // VK_NULL_HANDLE if no dedicated family was found
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    QueueFamilyIndices queueFamilyIndices_;
    bool validationEnabled_ = false;

    // Set while picking the physical device, consumed by createLogicalDevice()
    // so the enabled extension list matches exactly what was probed present.
    bool memoryBudgetSupported_ = false;
    bool calibratedTimestampsSupported_ = false;
    bool extendedDynamicState2Supported_ = false;
    bool extendedDynamicState3Supported_ = false;
    bool hostQueryResetSupported_ = false;
    bool bufferDeviceAddressSupported_ = false;
    uint32_t maxBindlessSampledImages_ = 0;
    const char* calibratedTimestampsExtensionName_ = nullptr;
};

} // namespace keel
