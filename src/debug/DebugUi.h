#pragma once

#include <volk.h>

#include <string>

union SDL_Event;

namespace keel {
class VulkanContext;
class Swapchain;
class Window;
} // namespace keel

namespace renderer {
class Renderer;
}

namespace debug {

class DebugUi {
public:
    DebugUi(keel::VulkanContext& context, keel::Swapchain& swapchain, keel::Window& window,
            renderer::Renderer& renderer);
    ~DebugUi();

    DebugUi(const DebugUi&) = delete;
    DebugUi& operator=(const DebugUi&) = delete;

    void processEvent(const SDL_Event& event);

    // Builds this frame's UI. Call once per frame before Renderer::drawFrame().
    // The four values are client::Axes' raw fields, shown verbatim in the
    // overlay - defaults are fine for a caller that doesn't have an
    // ActionMap to read them from.
    void beginFrame(float mouseDeltaX = 0.0f, float mouseDeltaY = 0.0f, float moveX = 0.0f, float moveY = 0.0f);

    // Records the finalized draw data into an already-active dynamic
    // rendering pass.
    void render(VkCommandBuffer cmd);

private:
    void drawOverlay();

    keel::VulkanContext& context_;
    keel::Swapchain& swapchain_;
    renderer::Renderer& renderer_;

    std::string deviceName_;
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    bool showDemoWindow_ = false;

    float lastMouseDeltaX_ = 0.0f;
    float lastMouseDeltaY_ = 0.0f;
    float lastMoveX_ = 0.0f;
    float lastMoveY_ = 0.0f;
};

} // namespace debug
