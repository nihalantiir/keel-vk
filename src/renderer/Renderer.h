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
#include <string>
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

class TextureArray2D;

struct Vertex {
    float position[3];
    float baseHueDegrees;
    float uv[2];
};

// One instance for setInstances() to draw this frame: a world-space
// transform, a local-space bounding sphere radius (for the CPU frustum
// cull), and which texture it samples. Pure data - Renderer doesn't know
// or care what a "hero" or a "satellite" is; that's src/client/'s
// business (see the wiki's Extending page). model is world-space:
// Renderer applies the floating-origin subtraction internally, the same
// way Camera::view() already does for the eye point.
struct InstanceDesc {
    glm::mat4 model{1.0f};
    float boundsRadius = 0.5f;
    TextureRef texture;
};

// The one graphics pipeline Renderer builds: fixed vertex layout (see
// Vertex above), fixed push-constant/descriptor-set layout, dynamic
// viewport/scissor, reverse-Z depth - all generic infrastructure, not
// content. The actual shader files are content, the same way a mesh or
// a texture is: this repo's own ContractTest.cpp passes
// "shaders/cube.vert.spv"/"shaders/cube.frag.spv"; a fork passes its
// own. Both paths resolve the same way keel::ShaderModule always has -
// relative to SDL_GetBasePath(), never the working directory - and a
// missing file fails the same way it always did: keel::ShaderModule's
// "Failed to open shader file: <resolved path>".
struct PipelineSpec {
    std::string vertPath;
    std::string fragPath;
    const char* vertDebugName = "vertex shader";
    const char* fragDebugName = "fragment shader";
};

class Renderer {
public:
    // pipelineSpec is required, not optional: recordWorldPass binds a
    // pipeline unconditionally every frame, even to draw zero instances,
    // so there's no meaningful "construct without one" state to fall
    // back to (see the wiki's Extending page).
    Renderer(keel::VulkanContext& context, keel::Swapchain& swapchain, keel::Window& window,
             const PipelineSpec& pipelineSpec);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Adds geometry to the shared mesh pool and makes it the mesh every
    // instance in setInstances() draws this frame (one shared mesh per
    // draw, not per-instance mesh selection - see the wiki's Rendering
    // page). Call before the first drawFrame(); MeshPool stays
    // append-only, static geometry only - see the wiki's Rendering page.
    MeshRange allocateMesh(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices,
                            uint32_t indexCount);
    void setMesh(MeshRange mesh) { activeMesh_ = mesh; }

    // Replaces whatever setInstances() drew last frame. Renderer only
    // knows how to cull and draw whatever's in this list - what those
    // instances are (a hero cube, a satellite ring, nothing at all) is
    // entirely src/client/'s business. Throws if instances.size() exceeds
    // kMaxInstances.
    void setInstances(const std::vector<InstanceDesc>& instances);

    void drawFrame(debug::DebugUi* debugUi = nullptr);

    // Live-editable by the debug UI.
    float* clearColor() { return clearColor_; }
    float& phaseSpeed() { return phaseSpeedDegPerSec_; }

    // Freezes hue phase and satellite orbiting (setSimTime stops changing)
    // when true. Driven by client::ActionMap's Pause action; main.cpp
    // gates its own sim-time accumulator on the same flag, so hue phase
    // and the cube's rotation (shared::FixedClock, also gated on this)
    // pause together instead of drifting apart.
    bool& paused() { return paused_; }

    // The renderer's one sim-time clock: hue phase and satellite orbiting
    // both read this, not a second SDL timer of their own. main.cpp calls
    // this once per frame with the same accumulator that gates
    // shared::FixedClock (see the wiki's Rendering page for why this
    // used to be two separate clocks and isn't anymore).
    void setSimTime(float seconds) { elapsedTimeSeconds_ = seconds; }

    // View/proj source and floating-origin state. Position/front/up are
    // fixed for the stock cube (no input-driven controller yet, see the
    // wiki's Extending page); live-editable by the debug overlay
    // (origin, near plane) to prove the plumbing without needing input.
    frame::Camera& camera() { return camera_; }

    // Distance from the origin along the camera's fixed home direction -
    // a "dolly" slider, not free-fly input. Recomputed into camera_.position
    // every frame in recordWorldPass; the debug overlay is the only writer.
    float& cameraDistance() { return cameraDistance_; }

    // Read-only: the front face's current hue-cycled color, for the overlay.
    glm::vec3 previewColor() const;

    // Scaffold width, not a guarantee: the array is actually created at
    // min(this, the device's real limit), see bindlessCapacity().
    static constexpr uint32_t kMaxBindlessTextures = 256;
    uint32_t bindlessCapacity() const { return bindlessCapacity_; }
    uint32_t boundTextureCount() const { return textureStreamer_->usedSlots(); }

    // Registers a texture into the registered-texture rotation the overlay's
    // "Cube texture slot" control and eviction (maybeEvictTexture)
    // operate on. Renderer's constructor registers none of its own -
    // src/client/ decides what (if anything) goes in here; an empty
    // rotation is a valid, harmless state (activeTextureSlot() falls
    // back to the streamer's resident default). Safe to call anytime,
    // including mid-frame, same as TextureStreamer::allocate() itself.
    TextureHandle registerTexture(uint32_t width, uint32_t height, const void* pixelsRgba8, const char* debugName);
    TextureHandle registerTextureCompressed(uint32_t width, uint32_t height, VkFormat format, const void* data,
                                             size_t dataSize, const char* debugName);

    // The bindless slot activeTextureIndex() currently points at, or
    // the streamer's resident default (slot 0) if the rotation is empty
    // or the index is out of range. Use this to fill an InstanceDesc's
    // TextureRef for whatever a caller wants to call "the hero."
    uint32_t activeTextureSlot() const;

    // Wraps Atlas2D::rects(), empty-safe: AtlasRect{} if the atlas has no
    // rects packed. index wraps modulo rects().size(), same as the old
    // hardcoded satellite loop did.
    AtlasRect atlasRect(uint32_t index) const;

    // Controls for the debug overlay: which of registeredTextures_ the cube
    // currently samples, a way to prove update() streams live, and a way
    // to prove free()+allocate() cycle a slot's generation. See
    // registerTexture() above for what populates registeredTextures_.
    int& activeTextureIndex() { return activeTextureIndex_; }
    int registeredTextureCount() const { return static_cast<int>(registeredTextures_.size()); }
    void regenerateActiveTexture();
    void freeAndReallocateSpareTexture();

    // The bindless slot of the registeredTextures_ entry at index (wrapping
    // modulo registeredTextureCount()), or 0 (the streamer's resident default)
    // if the rotation is empty. Unlike activeTextureSlot() above,
    // this names an arbitrary position rather than the overlay's current
    // pick - for a caller round-robining several instances through the
    // whole registered set, the way ContractTest.cpp's satellites do.
    uint32_t registeredTextureSlotAt(uint32_t index) const;

    // Real VK_EXT_memory_budget numbers (see VulkanContext::
    // memoryBudgetSupported), for the overlay's "VRAM" line - purely
    // informational, never the eviction trigger. residentBytes/
    // residentCapBytes/evictionCount are the actual eviction state:
    // see maybeEvictTexture in Renderer.cpp and the wiki's Rendering
    // page for why the cap is a small, deliberately artificial constant
    // rather than a fraction of the real device budget.
    bool memoryBudgetSupported() const;
    uint64_t deviceMemoryBudgetBytes() const;
    uint64_t deviceMemoryUsageBytes() const;
    VkDeviceSize residentBytes() const;
    static constexpr VkDeviceSize kResidentCapBytes = 2048;
    VkDeviceSize residentCapBytes() const { return kResidentCapBytes; }
    uint32_t evictionCount() const { return evictionCount_; }

    uint32_t textureArrayLayerCount() const;
    uint32_t atlasRectCount() const;

    // Which residency path the overlay's "Residency mode" control
    // currently picks - Renderer doesn't read this itself; it's state
    // for a caller building its own InstanceDesc to consult (see
    // ContractTest.cpp's hero for the pattern). All three are visibly
    // sampled by switching this, not just constructed - see the wiki's
    // Rendering page.
    TextureKind& residencyMode() { return residencyKind_; }
    int& arrayLayer() { return arrayLayer_; }
    int& atlasRectIndex() { return atlasRectIndex_; }

    // GPU scene stats from the most recently recorded frame, for the
    // debug overlay. instanceCount is how many instance slots were
    // written this frame; drawCount is how many of those survived CPU
    // frustum culling and were actually issued through
    // vkCmdDrawIndexedIndirect. cullTimeMs is CPU wall time for the cull
    // + compaction loop only, not the whole frame - see the wiki's
    // Rendering page for why this stays CPU-side instead of a compute
    // pass.
    uint32_t instanceCount() const { return lastInstanceCount_; }
    uint32_t drawCount() const { return lastDrawCount_; }
    uint32_t triangleCount() const { return lastTriangleCount_; }
    float cullTimeMs() const { return lastCullTimeMs_; }

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
    // Frees the oldest unused entry in registeredTextures_ (never
    // activeTextureIndex()'s current entry when residencyMode() is
    // Bindless-kind) once residentBytes() exceeds kResidentCapBytes.
    // Called once per frame from recordWorldPass, before anything
    // indexing registeredTextures_ by position for this frame's instances.
    void maybeEvictTexture();
    void createTimestampPool();
    void createDepthTarget();
    void destroyDepthTarget();
    void createPipeline(const PipelineSpec& spec);
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
    // Records nothing: frustum cull and indirect-command compaction stay
    // CPU-side (recordWorldPass) rather than a compute shader here. Not a
    // gap - a deliberate call at the current instance count, recorded on
    // the wiki's Rendering page. Kept as an explicit step so moving the
    // cull onto the GPU later doesn't reshuffle the pass order.
    void recordComputePass(VkCommandBuffer cmd);
    void recordOverlayPass(VkCommandBuffer cmd, debug::DebugUi* debugUi);

    keel::VulkanContext& context_;
    keel::Swapchain& swapchain_;
    keel::Window& window_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    // Same queue family as commandPool_ (see VulkanContext::uploadQueueFamily),
    // kept separate to isolate construction-time one-shot uploads (the
    // mesh pool, the array, the atlas) from the per-frame reused command
    // buffers.
    VkCommandPool uploadCommandPool_ = VK_NULL_HANDLE;

    // Signaled by each construction-time GPU upload instead of any of
    // them calling vkQueueWaitIdle. TextureArray2D and Atlas2D are always
    // constructed by Renderer itself and always signal 1 and 2
    // respectively (kResidencyConstructionUploadCount). allocateMesh()
    // is caller-driven (src/client/, not Renderer's constructor) and may
    // be called zero or more times after construction, so MeshPool's
    // firstSignalValue starts at 3 - strictly after the two fixed values
    // above, so its own calls can never collide with them - and its
    // highest-signaled value (MeshPool::lastSignaledUploadValue()) is
    // read fresh on every drawFrame(), not baked in once, so a
    // setup-time allocateMesh() call is always covered no matter when
    // relative to the first frame it happens.
    VkSemaphore uploadTimelineSemaphore_ = VK_NULL_HANDLE;
    static constexpr uint64_t kResidencyConstructionUploadCount = 2;
    // The highest upload-timeline value a graphics submission has
    // already waited for; drawFrame() only adds a wait semaphore when
    // the current target exceeds this, and bumps it afterward.
    uint64_t lastWaitedUploadValue_ = 0;

    std::vector<VkSemaphore> imageAvailableSemaphores_; // one per frame in flight
    std::vector<VkSemaphore> renderFinishedSemaphores_; // one per swapchain image
    std::vector<VkFence> inFlightFences_;               // one per frame in flight
    std::vector<VkFence> imagesInFlight_;               // tracks which fence currently guards each image

    uint32_t currentFrame_ = 0;

    // Geometry lives in the shared mesh pool: one vertex buffer, one
    // index buffer, subrange allocation. allocateMesh()/setMesh() are the
    // only way activeMesh_ changes - Renderer's constructor leaves the
    // pool empty; see the wiki's Extending page for why.
    std::unique_ptr<MeshPool> meshPool_;
    MeshRange activeMesh_{};

    // This frame's instances, set by setInstances(). Pure data Renderer
    // culls and draws; nothing here decides what an instance means.
    std::vector<InstanceDesc> instances_;

    // Per-instance data the World pass reads via an SSBO (set 1), indexed
    // by gl_InstanceIndex. Capacity (256) is a scaffold, not a
    // measurement - the largest population any of this template's demo
    // content ever wrote, with headroom, not a hard requirement. One
    // buffer pair per frame in flight, like simple-vk's old per-frame
    // vertex buffers, since CPU writes a new model/visible/bounds every
    // frame and a previous frame's draw might still be reading the other
    // copy.
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
    float lastCullTimeMs_ = 0.0f;

    VkImage depthImage_ = VK_NULL_HANDLE;
    VmaAllocation depthImageAllocation_ = VK_NULL_HANDLE;
    VkImageView depthImageView_ = VK_NULL_HANDLE;

    // The bindless sampled-image array the cube actually samples from
    // (cube.frag: bindlessTextures[textureIndex]). Owns its own descriptor
    // set layout/pool/set; Renderer only asks it for those to build the
    // pipeline layout and bind at draw time.
    std::unique_ptr<TextureStreamer> textureStreamer_;
    uint32_t bindlessCapacity_ = 0; // set in createTextureStreamer(), see bindlessCapacity()
    // Empty until registerTexture()/registerTextureCompressed()
    // add something - see those methods' comments in Renderer.h.
    std::vector<TextureHandle> registeredTextures_;
    // Parallel to registeredTextures_: byte size (for residentBytes()) and
    // the frame each entry was last activeTextureIndex()'s pick while
    // residencyMode() was Bindless-kind (for maybeEvictTexture's
    // oldest-first pick). Kept in lockstep by every function that grows,
    // shrinks, or replaces registeredTextures_.
    std::vector<VkDeviceSize> registeredTextureBytes_;
    std::vector<uint64_t> registeredTextureLastUsedFrame_;
    uint64_t frameCounter_ = 0;
    uint32_t evictionCount_ = 0;
    int activeTextureIndex_ = 0;
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

    TextureKind residencyKind_ = TextureKind::Bindless;
    int arrayLayer_ = 3;
    int atlasRectIndex_ = 0;

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
    // 0: no hue animation until a caller sets phaseSpeed() explicitly.
    // The animation machinery itself (cube.vert reading Vertex's
    // baseHueDegrees against time+phaseSpeed) is generic; 60 deg/s was
    // this repo's own contract-test choice, not a library default.
    float phaseSpeedDegPerSec_ = 0.0f;
    bool paused_ = false;
    frame::Camera camera_{};
    // Set once in the constructor from the original fixed eye point;
    // cameraDistance_ (below) scales along it every frame in
    // recordWorldPass. Dollying in shrinks the frustum's coverage in
    // world units, which is what actually culls satellites - see the
    // wiki's Rendering page.
    glm::vec3 cameraHomeDirection_{0.0f, 0.0f, 1.0f};
    float cameraDistance_ = 1.0f;
    // Set externally by setSimTime(), not accumulated here - see that
    // setter's comment.
    float elapsedTimeSeconds_ = 0.0f;
};

} // namespace renderer
