#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace keel {
class VulkanContext;
class Swapchain;
class Window;
} // namespace keel

namespace debug {
class DebugUi;
}

namespace renderer {

struct Vertex {
    float position[3];
    float baseHueDegrees;
    float uv[2];
};

class Renderer {
public:
    Renderer(keel::VulkanContext& context, keel::Swapchain& swapchain, keel::Window& window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void drawFrame(debug::DebugUi* debugUi = nullptr);

    // Live-editable by the debug UI.
    float* clearColor() { return clearColor_; }
    float& phaseSpeed() { return phaseSpeedDegPerSec_; }

    // Read-only: the front face's current hue-cycled color, for the overlay.
    glm::vec3 previewColor() const;

    // Bindless texture slots: how many of the fixed-capacity array are
    // actually registered right now, for the overlay readout.
    static constexpr uint32_t kMaxBindlessTextures = 16;
    uint32_t boundTextureCount() const { return 1; }

    // ImGui's own pipeline must declare this too: it draws inside the same
    // dynamic rendering scope with a depth attachment bound, and Vulkan
    // requires a bound pipeline's declared depth format to match even when
    // that pipeline never tests or writes depth.
    static constexpr VkFormat depthFormat() { return kDepthFormat; }

private:
    static constexpr int kFramesInFlight = 2;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void recreateSyncObjectsForSwapchain();
    void destroySyncObjects();
    void createGeometryBuffers();
    void createTexture();
    void destroyTexture();
    void createDescriptors();
    void destroyDescriptors();
    void createDepthTarget();
    void destroyDepthTarget();
    void createPipeline();
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, debug::DebugUi* debugUi);

    keel::VulkanContext& context_;
    keel::Swapchain& swapchain_;
    keel::Window& window_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    std::vector<VkSemaphore> imageAvailableSemaphores_; // one per frame in flight
    std::vector<VkSemaphore> renderFinishedSemaphores_; // one per swapchain image
    std::vector<VkFence> inFlightFences_;               // one per frame in flight
    std::vector<VkFence> imagesInFlight_;               // tracks which fence currently guards each image

    uint32_t currentFrame_ = 0;

    // Cube geometry is static once uploaded: one buffer each, not one per
    // frame in flight, unlike simple-vk's live-edited triangle vertices.
    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexBufferAllocation_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indexBufferAllocation_ = VK_NULL_HANDLE;

    // Scaffolding for GPU-driven draw submission: a real draw-parameter
    // buffer even though this landing only ever puts one command in it.
    VkBuffer indirectBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indirectBufferAllocation_ = VK_NULL_HANDLE;

    VkImage depthImage_ = VK_NULL_HANDLE;
    VmaAllocation depthImageAllocation_ = VK_NULL_HANDLE;
    VkImageView depthImageView_ = VK_NULL_HANDLE;

    // The cube's one registered texture, in bindless slot 0. Real bindless
    // scaffolding (fixed-capacity descriptor array, update-after-bind) even
    // though only one slot is filled today.
    VkImage textureImage_ = VK_NULL_HANDLE;
    VmaAllocation textureImageAllocation_ = VK_NULL_HANDLE;
    VkImageView textureImageView_ = VK_NULL_HANDLE;
    VkSampler textureSampler_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    float clearColor_[3] = {0.15f, 0.15f, 0.16f};
    float phaseSpeedDegPerSec_ = 60.0f;
    float elapsedTimeSeconds_ = 0.0f;
    uint64_t lastTicks_ = 0;
    bool clockStarted_ = false;
};

} // namespace renderer
