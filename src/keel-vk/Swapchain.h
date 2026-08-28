#pragma once

#include <volk.h>

#include <cstdint>
#include <vector>

namespace keel {
class VulkanContext;
class Window;

class Swapchain {
public:
    // preferredPresentMode is used if the surface actually supports it;
    // otherwise falls back to MAILBOX, then FIFO (always supported) - see
    // choosePresentMode(). Defaults to MAILBOX to preserve the original
    // simple-vk behavior for callers that don't care.
    Swapchain(VulkanContext& context, Window& window,
              VkPresentModeKHR preferredPresentMode = VK_PRESENT_MODE_MAILBOX_KHR);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    void recreate();

    VkSwapchainKHR handle() const { return swapchain_; }
    VkFormat imageFormat() const { return imageFormat_; }
    VkExtent2D extent() const { return extent_; }
    VkPresentModeKHR presentMode() const { return presentMode_; }
    const std::vector<VkImage>& images() const { return images_; }
    const std::vector<VkImageView>& imageViews() const { return imageViews_; }
    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }

private:
    void create();
    void destroy();
    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>&) const;
    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>&) const;
    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR&) const;

    VulkanContext& context_;
    Window& window_;
    VkPresentModeKHR preferredPresentMode_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat imageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
};

} // namespace keel
