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
    const char* calibratedTimestampsExtensionName_ = nullptr;
};

} // namespace keel
