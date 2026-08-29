#include "DebugUi.h"

#include "../frame/Camera.h"
#include "../keel-vk/Swapchain.h"
#include "../keel-vk/VkCheck.h"
#include "../keel-vk/VulkanContext.h"
#include "../keel-vk/Window.h"
#include "../renderer/Renderer.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <cstddef>
#include <iterator>
#include <stdexcept>

namespace debug {

namespace {

void checkVkResult(VkResult result) {
    keel::vkCheck(result, "Dear ImGui Vulkan backend error");
}

const char* presentModeLabel(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return "Immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return "Mailbox";
        case VK_PRESENT_MODE_FIFO_KHR:
            return "Fifo";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return "Fifo relaxed";
        default:
            return "Other";
    }
}

} // namespace

DebugUi::DebugUi(keel::VulkanContext& context, keel::Swapchain& swapchain, keel::Window& window,
                  renderer::Renderer& renderer)
    : context_(context), swapchain_(swapchain), renderer_(renderer), colorFormat_(swapchain.imageFormat()) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(context_.physicalDevice(), &props);
    deviceName_ = props.deviceName;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(window.handle())) {
        throw std::runtime_error("Failed to initialize ImGui SDL3 backend");
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = context_.instance();
    initInfo.PhysicalDevice = context_.physicalDevice();
    initInfo.Device = context_.device();
    initInfo.QueueFamily = context_.queueFamilies().graphicsFamily.value();
    initInfo.Queue = context_.graphicsQueue();
    initInfo.DescriptorPoolSize = 64;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = swapchain_.imageCount();
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat_;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = renderer::Renderer::depthFormat();
    initInfo.MinAllocationSize = 1024 * 1024;
    initInfo.CheckVkResultFn = checkVkResult;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        throw std::runtime_error("Failed to initialize ImGui Vulkan backend");
    }
}

DebugUi::~DebugUi() {
    vkDeviceWaitIdle(context_.device());
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void DebugUi::processEvent(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void DebugUi::beginFrame(float mouseDeltaX, float mouseDeltaY, float moveX, float moveY) {
    lastMouseDeltaX_ = mouseDeltaX;
    lastMouseDeltaY_ = mouseDeltaY;
    lastMoveX_ = moveX;
    lastMoveY_ = moveY;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    drawOverlay();

    ImGui::Render();
}

void DebugUi::render(VkCommandBuffer cmd) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void DebugUi::drawOverlay() {
    ImGui::Begin("keel-vk");

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("%.2f ms (%.0f FPS)", 1000.0f / io.Framerate, io.Framerate);
    if (renderer_.gpuTimestampsSupported()) {
        ImGui::Text("GPU: %.2f ms", renderer_.gpuFrameTimeMs());
    } else {
        ImGui::TextDisabled("GPU: timestamps unavailable");
    }

    const VkExtent2D extent = swapchain_.extent();
    ImGui::Text("Swapchain %ux%u, %s", extent.width, extent.height, presentModeLabel(swapchain_.presentMode()));
    ImGui::Text("Device: %s", deviceName_.c_str());

    ImGui::Separator();
    frame::Camera& camera = renderer_.camera();
    ImGui::TextDisabled("Depth: reverse-Z, infinite far (compare GREATER_OR_EQUAL, clear 0.0)");
    ImGui::SliderFloat("Near plane", &camera.nearPlane, 0.01f, 5.0f);
    ImGui::DragFloat3("Origin (floating-origin offset)", &camera.origin.x, 0.1f);
    ImGui::SliderFloat("Camera distance (dolly)", &renderer_.cameraDistance(), 1.0f, 6.0f);
    ImGui::TextDisabled("Dolly in to shrink frustum coverage - watch drawn/instances below change.");

    ImGui::Text("Axes: mouse (%.1f, %.1f)  move (%.0f, %.0f)", lastMouseDeltaX_, lastMouseDeltaY_, lastMoveX_,
                lastMoveY_);
    ImGui::TextDisabled("Raw client::Axes data - no camera controller reads these yet.");

    ImGui::Separator();
    ImGui::ColorEdit3("Clear color", renderer_.clearColor());
    ImGui::SliderFloat("Phase speed (deg/s)", &renderer_.phaseSpeed(), 0.0f, 360.0f);

    const glm::vec3 preview = renderer_.previewColor();
    const float previewColor[3] = {preview.x, preview.y, preview.z};
    ImGui::ColorEdit3("Cube color (front face)", const_cast<float*>(previewColor),
                       ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop);

    ImGui::Separator();
    ImGui::Text("Instances: %u written, %u drawn (%u triangles)", renderer_.instanceCount(), renderer_.drawCount(),
                renderer_.triangleCount());
    ImGui::Text("Cull: CPU (%.3f ms)", renderer_.cullTimeMs());

    ImGui::Separator();
    static const char* kResidencyModes[] = {"Bindless", "Array", "Atlas"};
    int residencyModeIndex = static_cast<int>(renderer_.residencyMode());
    if (ImGui::Combo("Residency mode", &residencyModeIndex, kResidencyModes,
                      static_cast<int>(std::size(kResidencyModes)))) {
        renderer_.residencyMode() = static_cast<renderer::TextureKind>(residencyModeIndex);
    }
    ImGui::TextDisabled("The cube samples whichever path is selected above - all three are live, not just built.");

    ImGui::Text("Bindless textures: %u / %u slots used", renderer_.boundTextureCount(),
                renderer_.bindlessCapacity());
    if (renderer_.memoryBudgetSupported()) {
        ImGui::Text("VRAM: %.1f / %.1f MB (VK_EXT_memory_budget)",
                    static_cast<double>(renderer_.deviceMemoryUsageBytes()) / (1024.0 * 1024.0),
                    static_cast<double>(renderer_.deviceMemoryBudgetBytes()) / (1024.0 * 1024.0));
    } else {
        ImGui::TextDisabled("VRAM: N/A (VK_EXT_memory_budget not present)");
    }
#ifdef KEEL_VK_DEBUG
    ImGui::Text("Registered textures resident: %llu / %llu bytes (debug cap), %u evicted",
                static_cast<unsigned long long>(renderer_.residentBytes()),
                static_cast<unsigned long long>(renderer_.residentCapBytes()), renderer_.evictionCount());
#else
    ImGui::Text("Registered textures resident: %llu bytes, %u evicted (debug cap inactive outside Debug builds)",
                static_cast<unsigned long long>(renderer_.residentBytes()), renderer_.evictionCount());
#endif
    ImGui::SliderInt("Cube texture slot", &renderer_.activeTextureIndex(), 0, renderer_.registeredTextureCount() - 1,
                      "slot %d");
    if (ImGui::Button("Regenerate active (streamed update)")) {
        renderer_.regenerateActiveTexture();
    }
    if (ImGui::Button("Free + reallocate spare slot")) {
        renderer_.freeAndReallocateSpareTexture();
    }
    ImGui::Text("Texture array: %u layers", renderer_.textureArrayLayerCount());
    if (renderer_.textureArrayLayerCount() > 0) {
        ImGui::SliderInt("Array layer", &renderer_.arrayLayer(), 0,
                          static_cast<int>(renderer_.textureArrayLayerCount()) - 1);
    }
    ImGui::Text("Atlas: %u rects packed", renderer_.atlasRectCount());
    if (renderer_.atlasRectCount() > 0) {
        ImGui::SliderInt("Atlas rect", &renderer_.atlasRectIndex(), 0,
                          static_cast<int>(renderer_.atlasRectCount()) - 1);
    }

    ImGui::Separator();
    ImGui::Checkbox("Show ImGui demo window", &showDemoWindow_);

    ImGui::End();

    if (showDemoWindow_) {
        ImGui::ShowDemoWindow(&showDemoWindow_);
    }
}

} // namespace debug
