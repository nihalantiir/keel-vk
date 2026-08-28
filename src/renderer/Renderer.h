#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "Atlas2D.h"
#include "TextureStreamer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace keel {
class VulkanContext;
class Swapchain;
class Window;
class Vfs;
} // namespace keel

namespace debug {
class DebugUi;
}

namespace renderer {

class TextureArray2D;

struct Vertex {
    float position[3];
    float baseHueDegrees;
    float uv[2];
};

class Renderer {
public:
    Renderer(keel::VulkanContext& context, keel::Swapchain& swapchain, keel::Window& window, keel::Vfs& vfs);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void drawFrame(debug::DebugUi* debugUi = nullptr);

    // Live-editable by the debug UI.
    float* clearColor() { return clearColor_; }
    float& phaseSpeed() { return phaseSpeedDegPerSec_; }

    // Freezes hue phase (elapsed time stops advancing) when true. Driven
    // by client::ActionMap's Pause action.
    bool& paused() { return paused_; }

    // The cube's model matrix, extracted from a keel::World entity's
    // Transform each frame (see src/client/main.cpp). Identity until the
    // first call.
    void setModel(const glm::mat4& model) { model_ = model; }

    // Read-only: the front face's current hue-cycled color, for the overlay.
    glm::vec3 previewColor() const;

    static constexpr uint32_t kMaxBindlessTextures = 256;
    uint32_t boundTextureCount() const { return textureStreamer_->usedSlots(); }

    // Demo controls for the debug overlay: which of demoTextures_ the cube
    // currently samples, a way to prove update() streams live, and a way
    // to prove free()+allocate() cycle a slot's generation. See
    // Renderer.cpp's createTextureStreamer() for what's registered.
    int& activeDemoTextureIndex() { return activeDemoTextureIndex_; }
    int demoTextureCount() const { return static_cast<int>(demoTextures_.size()); }
    void regenerateActiveTexture();
    void freeAndReallocateSpareTexture();

    uint32_t textureArrayLayerCount() const;
    uint32_t atlasRectCount() const;

    // GPU-side frame time from VK_QUERY_TYPE_TIMESTAMP, distinct from the
    // overlay's existing CPU-side ImGui::GetIO().Framerate reading. Absent
    // on a device without VkPhysicalDeviceLimits::timestampComputeAndGraphics
    // (rare on desktop GPUs, not part of the required device contract).
    bool gpuTimestampsSupported() const { return timestampsSupported_; }
    float gpuFrameTimeMs() const { return lastGpuFrameMs_; }

    // ImGui's own pipeline must declare this too: it draws inside the same
    // dynamic rendering scope with a depth attachment bound, and Vulkan
    // requires a bound pipeline's declared depth format to match even when
    // that pipeline never tests or writes depth.
    static constexpr VkFormat depthFormat() { return kDepthFormat; }

private:
    static constexpr int kFramesInFlight = 2;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    void createCommandPool();
    void createUploadCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void recreateSyncObjectsForSwapchain();
    void destroySyncObjects();
    void createGeometryBuffers();
    void createTextureStreamer();
    void createTimestampPool();
    void createDepthTarget();
    void destroyDepthTarget();
    void createPipeline();
    void loadPipelineCache();
    void savePipelineCache();
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, debug::DebugUi* debugUi);

    keel::VulkanContext& context_;
    keel::Swapchain& swapchain_;
    keel::Window& window_;
    keel::Vfs& vfs_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    // Bound to VulkanContext::uploadQueueFamily(): the dedicated transfer
    // family if the device has one, otherwise the same family as
    // commandPool_. Used only for construction-time one-shot uploads (the
    // texture streamer's startup flush, the array, the atlas); never on
    // the per-frame path.
    VkCommandPool uploadCommandPool_ = VK_NULL_HANDLE;

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

    // The bindless sampled-image array the cube actually samples from
    // (cube.frag: bindlessTextures[textureIndex]). Owns its own descriptor
    // set layout/pool/set; Renderer only asks it for those to build the
    // pipeline layout and bind at draw time.
    std::unique_ptr<TextureStreamer> textureStreamer_;
    std::vector<TextureHandle> demoTextures_; // slots 1..N: checker, stripes, gradient, spare
    int activeDemoTextureIndex_ = 0;
    uint32_t regenerateCounter_ = 0;

    // Constructed, populated, and correctly left unbound to any shader
    // this landing (see Renderer.cpp's createTextureStreamer): both exist
    // to prove the path, not because the cube samples them.
    std::unique_ptr<TextureArray2D> textureArray_;
    std::unique_ptr<Atlas2D> atlas_;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    // Loaded from (and saved back to) a file next to the executable so a
    // second run's pipeline creation can skip driver shader recompilation.
    // Not a correctness requirement (an empty/invalid cache is silently
    // ignored by the driver), purely a warm-start optimization.
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;

    VkQueryPool timestampPool_ = VK_NULL_HANDLE; // 2 queries per frame in flight: start, end
    bool timestampsSupported_ = false;
    float timestampPeriodNs_ = 0.0f;
    float lastGpuFrameMs_ = 0.0f;
    // A freshly created query is uninitialized, not merely "unavailable":
    // reading it before its first vkCmdResetQueryPool + vkCmdWriteTimestamp2
    // is a validation error, not just VK_NOT_READY. Tracks which frame
    // slots have completed that at least once.
    std::array<bool, kFramesInFlight> timestampSlotReady_{};

    float clearColor_[3] = {0.15f, 0.15f, 0.16f};
    float phaseSpeedDegPerSec_ = 60.0f;
    bool paused_ = false;
    glm::mat4 model_{1.0f};
    float elapsedTimeSeconds_ = 0.0f;
    uint64_t lastTicks_ = 0;
    bool clockStarted_ = false;
};

} // namespace renderer
