#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "../frame/Camera.h"
#include "Atlas2D.h"
#include "MeshPool.h"
#include "TextureRef.h"
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

    // View/proj source and floating-origin state. Position/front/up are
    // fixed for the stock cube (no input-driven controller yet, see the
    // wiki's Extending page); live-editable by the debug overlay
    // (origin, near plane) to prove the plumbing without needing input.
    frame::Camera& camera() { return camera_; }

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

    // Which residency path the cube's instance names this frame. All
    // three are visibly sampled by switching this, not just constructed -
    // see the wiki's Rendering page.
    TextureKind& residencyMode() { return demoResidencyKind_; }
    int& demoArrayLayer() { return demoArrayLayer_; }
    int& demoAtlasRectIndex() { return demoAtlasRectIndex_; }

    // GPU scene stats from the most recently recorded frame, for the
    // debug overlay. instanceCount is how many instance slots were
    // written this frame (just the cube today); drawCount is how many of
    // those survived CPU frustum culling and were actually issued through
    // vkCmdDrawIndexedIndirect.
    uint32_t instanceCount() const { return lastInstanceCount_; }
    uint32_t drawCount() const { return lastDrawCount_; }
    uint32_t triangleCount() const { return lastTriangleCount_; }

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
    void createMeshPool();
    void createInstanceResources();
    void destroyInstanceResources();
    void createUploadTimelineSemaphore();
    void createTextureStreamer();
    void createResidencyDescriptorSet();
    void createTimestampPool();
    void createDepthTarget();
    void destroyDepthTarget();
    void createPipeline();
    void loadPipelineCache();
    void savePipelineCache();
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, debug::DebugUi* debugUi);

    // The frame's explicit pass list, in record order. Not a frame-graph:
    // no automatic barrier derivation or resource dependency tracking,
    // just named steps recordCommandBuffer calls in sequence so the order
    // is visible in one place. Acquire (image acquire + fence wait) and
    // Present (the post-barrier + vkQueuePresentKHR) bracket these from
    // drawFrame() itself, outside any command buffer.
    void recordWorldPass(VkCommandBuffer cmd, VkExtent2D extent);
    // Records nothing yet: reserved for GPU-driven work (frustum cull,
    // indirect-command compaction) once there is more than one draw to
    // cull. Kept as an explicit step now so adding that later doesn't
    // reshuffle the pass order.
    void recordComputePass(VkCommandBuffer cmd);
    void recordOverlayPass(VkCommandBuffer cmd, debug::DebugUi* debugUi);

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

    // Signaled by each construction-time texture upload (TextureStreamer's
    // startup flush: value 1; TextureArray2D: value 2; Atlas2D: value 3)
    // instead of each one calling vkQueueWaitIdle. The first frame's
    // graphics-queue submission waits for value 3 (see needsUploadWait_
    // below) so none of the three can be sampled before their upload
    // actually lands, without blocking the CPU during construction.
    VkSemaphore uploadTimelineSemaphore_ = VK_NULL_HANDLE;
    static constexpr uint64_t kUploadTimelineTargetValue = 3;
    bool needsUploadTimelineWait_ = true;
    // Freed in ~Renderer(), not right after submission: see
    // createTextureStreamer()'s comment for why.
    VkCommandBuffer startupTextureUploadCmd_ = VK_NULL_HANDLE;

    std::vector<VkSemaphore> imageAvailableSemaphores_; // one per frame in flight
    std::vector<VkSemaphore> renderFinishedSemaphores_; // one per swapchain image
    std::vector<VkFence> inFlightFences_;               // one per frame in flight
    std::vector<VkFence> imagesInFlight_;               // tracks which fence currently guards each image

    uint32_t currentFrame_ = 0;

    // Cube geometry lives in the shared mesh pool: one vertex buffer, one
    // index buffer, subrange allocation. Static once uploaded, like the
    // single dedicated buffers this replaced.
    std::unique_ptr<MeshPool> meshPool_;
    MeshRange cubeMesh_{};

    // Per-instance data the World pass reads via an SSBO (set 1), indexed
    // by gl_InstanceIndex. Capacity is sized for a real scene even though
    // only slot 0 (the cube) is ever populated this landing; see the
    // wiki's Rendering page. One buffer pair per frame in flight, like
    // simple-vk's old per-frame vertex buffers, since CPU writes a new
    // model/visible/bounds every frame and a previous frame's draw might
    // still be reading the other copy.
    static constexpr uint32_t kMaxInstances = 256;

    // The concrete per-slot layout (GpuInstance: model, bounds, texture
    // index, visible flag, generation) lives in Renderer.cpp next to the
    // code that writes it and cube.vert's matching std430 struct, not
    // here - instanceMapped below is untyped for exactly that reason.
    struct InstanceFrame {
        VkBuffer instanceBuffer = VK_NULL_HANDLE;
        VmaAllocation instanceAllocation = VK_NULL_HANDLE;
        void* instanceMapped = nullptr;
        VkBuffer indirectBuffer = VK_NULL_HANDLE;
        VmaAllocation indirectAllocation = VK_NULL_HANDLE;
        void* indirectMapped = nullptr;
        VkDescriptorSet instanceSet = VK_NULL_HANDLE;
    };
    std::array<InstanceFrame, kFramesInFlight> instanceFrames_{};
    VkDescriptorSetLayout instanceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool instanceDescriptorPool_ = VK_NULL_HANDLE;

    uint32_t lastInstanceCount_ = 0;
    uint32_t lastDrawCount_ = 0;
    uint32_t lastTriangleCount_ = 0;

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

    // Populated once at construction. Both are visibly sampled by the
    // cube now (see residencyMode() above and the wiki's Rendering page),
    // not just constructed to prove the path.
    std::unique_ptr<TextureArray2D> textureArray_;
    std::unique_ptr<Atlas2D> atlas_;

    // set 2: the array/atlas samplers, written once (both are populated
    // once at construction, never streamed). set 0 (bindless) is
    // TextureStreamer's own; set 1 is the per-frame instance SSBO.
    VkDescriptorSetLayout residencySetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool residencyDescriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet residencySet_ = VK_NULL_HANDLE;

    TextureKind demoResidencyKind_ = TextureKind::Bindless;
    int demoArrayLayer_ = 3;
    int demoAtlasRectIndex_ = 0;

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
    frame::Camera camera_{};
    float elapsedTimeSeconds_ = 0.0f;
    uint64_t lastTicks_ = 0;
    bool clockStarted_ = false;
};

} // namespace renderer
