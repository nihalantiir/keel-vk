#include "VulkanContext.h"

#include "DebugUtils.h"
#include "VkCheck.h"
#include "Window.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace keel {

namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
constexpr std::array<const char*, 1> kRequiredDeviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

// The device contract keel-vk builds on. Every entry here is checked with
// vkGetPhysicalDeviceFeatures2 before a device is considered suitable; if no
// device satisfies all of them, pickPhysicalDevice() reports exactly which
// ones each candidate was missing rather than a single opaque failure.
struct DeviceFeatureChain {
    VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

    DeviceFeatureChain() {
        features2.pNext = &features11;
        features11.pNext = &features12;
        features12.pNext = &features13;
    }
};

DeviceFeatureChain queryFeatures(VkPhysicalDevice device) {
    DeviceFeatureChain chain;
    vkGetPhysicalDeviceFeatures2(device, &chain.features2);
    return chain;
}

// Returns the human-readable name of every required feature/extension this
// device lacks. Empty means the device satisfies the full device contract.
std::vector<std::string> missingRequirements(VkPhysicalDevice device, VkSurfaceKHR surface,
                                              const QueueFamilyIndices& queues) {
    std::vector<std::string> missing;

    if (!queues.isComplete()) {
        missing.push_back("graphics+present queue families");
    }

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExt(extCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, availableExt.data());
    std::set<std::string> extNames;
    for (const auto& ext : availableExt) {
        extNames.insert(ext.extensionName);
    }
    for (const char* required : kRequiredDeviceExtensions) {
        if (!extNames.count(required)) {
            missing.push_back(std::string(required));
        }
    }

    uint32_t formatCount = 0;
    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    if (formatCount == 0) missing.push_back("surface format");
    if (presentModeCount == 0) missing.push_back("present mode");

    const DeviceFeatureChain chain = queryFeatures(device);

    if (!chain.features13.dynamicRendering) missing.push_back("dynamicRendering");
    if (!chain.features13.synchronization2) missing.push_back("synchronization2");
    if (!chain.features13.maintenance4) missing.push_back("maintenance4");

    // bufferDeviceAddress is deliberately not required: nothing in this
    // template uses GL_EXT_buffer_reference or vkGetBufferDeviceAddress.
    // Enabled when present (see createLogicalDevice), never gated on.
    if (!chain.features12.timelineSemaphore) missing.push_back("timelineSemaphore");
    if (!chain.features12.descriptorIndexing) missing.push_back("descriptorIndexing");
    if (!chain.features12.runtimeDescriptorArray) missing.push_back("runtimeDescriptorArray");
    if (!chain.features12.descriptorBindingPartiallyBound) missing.push_back("descriptorBindingPartiallyBound");
    if (!chain.features12.descriptorBindingVariableDescriptorCount)
        missing.push_back("descriptorBindingVariableDescriptorCount");
    if (!chain.features12.descriptorBindingSampledImageUpdateAfterBind)
        missing.push_back("descriptorBindingSampledImageUpdateAfterBind");
    if (!chain.features12.descriptorBindingStorageBufferUpdateAfterBind)
        missing.push_back("descriptorBindingStorageBufferUpdateAfterBind");
    if (!chain.features12.scalarBlockLayout) missing.push_back("scalarBlockLayout");
    // Required to actually index the bindless sampled-image array
    // (GL_EXT_nonuniform_qualifier / nonuniformEXT in shader code); the
    // descriptor-indexing bits above only make the array itself legal.
    if (!chain.features12.shaderSampledImageArrayNonUniformIndexing)
        missing.push_back("shaderSampledImageArrayNonUniformIndexing");

    // shaderDrawParameters was promoted to core in Vulkan 1.1, not 1.2 - it
    // lives in VkPhysicalDeviceVulkan11Features even though the rest of the
    // descriptor-indexing family below it is 1.2.
    if (!chain.features11.shaderDrawParameters) missing.push_back("shaderDrawParameters");

    VkPhysicalDeviceFeatures features10;
    vkGetPhysicalDeviceFeatures(device, &features10);
    if (!features10.multiDrawIndirect) missing.push_back("multiDrawIndirect");
    if (!features10.drawIndirectFirstInstance) missing.push_back("drawIndirectFirstInstance");
    if (!features10.textureCompressionBC) missing.push_back("textureCompressionBC");

    // BC7 specifically, sampled: textureCompressionBC covers the whole BC
    // family, but this template only ever creates BC7 images (see
    // Ktx2.h/TextureStreamer::allocateCompressed). Checking the one format
    // actually used is more honest than trusting the umbrella feature bit.
    VkFormatProperties bc7Props;
    vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_BC7_UNORM_BLOCK, &bc7Props);
    if (!(bc7Props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        missing.push_back("VK_FORMAT_BC7_UNORM_BLOCK sampled (optimal tiling)");
    }

    return missing;
}

bool checkValidationLayerSupport() {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());

    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, kValidationLayerName) == 0) {
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                              void* /*userData*/) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << "[Vulkan] " << callbackData->pMessage << std::endl;
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT makeDebugMessengerCreateInfo() {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
    return createInfo;
}

int ratePhysicalDevice(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 1000;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 100;
    }
    score += static_cast<int>(props.limits.maxImageDimension2D);
    return score;
}

} // namespace

VulkanContext::VulkanContext(Window& window) {
    createInstance(window);
    setupDebugMessenger();
    createSurface(window);
    pickPhysicalDevice();
    createLogicalDevice();
    createAllocator();
}

VulkanContext::~VulkanContext() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (debugMessenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

void VulkanContext::createInstance(Window& window) {
    vkCheck(volkInitialize(), "Failed to initialize volk (is a Vulkan loader installed?)");

#ifdef KEEL_VK_DEBUG
    validationEnabled_ = checkValidationLayerSupport();
    if (!validationEnabled_) {
        std::cerr << "Validation layer requested but not available; continuing without it.\n";
    }
#endif

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "keel-vk";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
    appInfo.pEngineName = "keel-vk";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 2, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = window.getRequiredInstanceExtensions();
    if (validationEnabled_) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    // Chaining a messenger here also covers vkCreateInstance/vkDestroyInstance;
    // setupDebugMessenger() installs the persistent one for everything else.
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (validationEnabled_) {
        static const char* layer = kValidationLayerName;
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &layer;

        debugCreateInfo = makeDebugMessengerCreateInfo();
        createInfo.pNext = &debugCreateInfo;
    }

    vkCheck(vkCreateInstance(&createInfo, nullptr, &instance_), "Failed to create Vulkan instance");

    volkLoadInstance(instance_);
}

void VulkanContext::setupDebugMessenger() {
    if (!validationEnabled_) {
        return;
    }

    VkDebugUtilsMessengerCreateInfoEXT createInfo = makeDebugMessengerCreateInfo();
    vkCheck(vkCreateDebugUtilsMessengerEXT(instance_, &createInfo, nullptr, &debugMessenger_),
            "Failed to set up debug messenger");
}

void VulkanContext::createSurface(Window& window) {
    surface_ = window.createSurface(instance_);
}

QueueFamilyIndices VulkanContext::findQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
        if (presentSupport) {
            indices.presentFamily = i;
        }
    }

    return indices;
}

void VulkanContext::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        throw std::runtime_error("No Vulkan-capable GPU found");
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestScore = -1;
    std::ostringstream rejectionReport;

    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        const QueueFamilyIndices queues = findQueueFamilies(device);
        const std::vector<std::string> missing = missingRequirements(device, surface_, queues);

        if (!missing.empty()) {
            rejectionReport << "  " << props.deviceName << ": missing ";
            for (size_t i = 0; i < missing.size(); ++i) {
                rejectionReport << missing[i] << (i + 1 < missing.size() ? ", " : "");
            }
            rejectionReport << "\n";
            continue;
        }

        const int score = ratePhysicalDevice(device);
        if (score > bestScore) {
            bestScore = score;
            best = device;
        }
    }

    if (best == VK_NULL_HANDLE) {
        throw std::runtime_error("No GPU satisfies the keel-vk device contract:\n" + rejectionReport.str());
    }

    physicalDevice_ = best;
    queueFamilyIndices_ = findQueueFamilies(physicalDevice_);

    // Probe optional extensions now; createLogicalDevice() enables exactly
    // the ones found present here.
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExt(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, availableExt.data());
    std::set<std::string> extNames;
    for (const auto& ext : availableExt) {
        extNames.insert(ext.extensionName);
    }

    memoryBudgetSupported_ = extNames.count(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) > 0;
    extendedDynamicState2Supported_ = extNames.count(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) > 0;
    extendedDynamicState3Supported_ = extNames.count(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) > 0;

    if (extNames.count(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) {
        calibratedTimestampsSupported_ = true;
        calibratedTimestampsExtensionName_ = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    } else if (extNames.count(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) {
        calibratedTimestampsSupported_ = true;
        calibratedTimestampsExtensionName_ = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    }

    const DeviceFeatureChain chain = queryFeatures(physicalDevice_);
    hostQueryResetSupported_ = chain.features12.hostQueryReset == VK_TRUE;
    bufferDeviceAddressSupported_ = chain.features12.bufferDeviceAddress == VK_TRUE;

    // The real cap on the bindless sampled-image array TextureStreamer
    // builds: min of the whole-set limit and the per-stage limit, since
    // it's bound in one stage (fragment) but the set-wide limit still
    // applies. See VulkanContext::maxBindlessSampledImages().
    VkPhysicalDeviceDescriptorIndexingProperties descriptorIndexingProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &descriptorIndexingProps;
    vkGetPhysicalDeviceProperties2(physicalDevice_, &props2);
    maxBindlessSampledImages_ = std::min(descriptorIndexingProps.maxDescriptorSetUpdateAfterBindSampledImages,
                                          descriptorIndexingProps.maxPerStageDescriptorUpdateAfterBindSampledImages);
}

void VulkanContext::createLogicalDevice() {
    std::set<uint32_t> uniqueFamilies = {queueFamilyIndices_.graphicsFamily.value(),
                                          queueFamilyIndices_.presentFamily.value()};

    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queueCreateInfos.push_back(info);
    }

    VkPhysicalDeviceFeatures features10{};
    features10.multiDrawIndirect = VK_TRUE;
    features10.drawIndirectFirstInstance = VK_TRUE;

    VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    features11.shaderDrawParameters = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.timelineSemaphore = VK_TRUE;
    features12.bufferDeviceAddress = bufferDeviceAddressSupported_ ? VK_TRUE : VK_FALSE; // optional, unused
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    features12.scalarBlockLayout = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.hostQueryReset = hostQueryResetSupported_ ? VK_TRUE : VK_FALSE; // optional, reserved for GPU timing

    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    features13.maintenance4 = VK_TRUE;

    features11.pNext = &features12;
    features12.pNext = &features13;

    std::vector<const char*> deviceExtensions(kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end());
    if (memoryBudgetSupported_) deviceExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (calibratedTimestampsSupported_) deviceExtensions.push_back(calibratedTimestampsExtensionName_);
    if (extendedDynamicState2Supported_) deviceExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    if (extendedDynamicState3Supported_) deviceExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &features11;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &features10;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    vkCheck(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "Failed to create logical device");

    volkLoadDevice(device_); // switch to fast device-level dispatch

    vkGetDeviceQueue(device_, queueFamilyIndices_.graphicsFamily.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, queueFamilyIndices_.presentFamily.value(), 0, &presentQueue_);

    setDebugObjectName(device_, VK_OBJECT_TYPE_DEVICE, reinterpret_cast<uint64_t>(device_), "keel-vk device");
    if (graphicsQueue_ == presentQueue_) {
        setDebugObjectName(device_, VK_OBJECT_TYPE_QUEUE, reinterpret_cast<uint64_t>(graphicsQueue_),
                            "graphics/present queue");
    } else {
        setDebugObjectName(device_, VK_OBJECT_TYPE_QUEUE, reinterpret_cast<uint64_t>(graphicsQueue_),
                            "graphics queue");
        setDebugObjectName(device_, VK_OBJECT_TYPE_QUEUE, reinterpret_cast<uint64_t>(presentQueue_),
                            "present queue");
    }
}

void VulkanContext::createAllocator() {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.physicalDevice = physicalDevice_;
    info.device = device_;
    info.instance = instance_;
    info.pVulkanFunctions = &functions;
    if (memoryBudgetSupported_) {
        info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    if (bufferDeviceAddressSupported_) {
        info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    vkCheck(vmaCreateAllocator(&info, &allocator_), "Failed to create VMA allocator");
}

} // namespace keel
