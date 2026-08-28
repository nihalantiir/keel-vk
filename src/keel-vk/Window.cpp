#include "Window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <stdexcept>

namespace keel {

namespace {
constexpr float kTitleUpdateInterval = 0.5f; // seconds between title refreshes
}

Window::Window(const std::string& title, int width, int height) : baseTitle_(title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("Failed to initialize SDL: ") + SDL_GetError());
    }

    window_ = SDL_CreateWindow(title.c_str(), width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        throw std::runtime_error(std::string("Failed to create window: ") + SDL_GetError());
    }
}

Window::~Window() {
    if (window_) {
        SDL_DestroyWindow(window_);
    }
    SDL_Quit();
}

void Window::pollEvents(const EventCallback& onEvent) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (onEvent) {
            onEvent(event);
        }
        switch (event.type) {
            case SDL_EVENT_QUIT:
                quitRequested_ = true;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                resized_ = true;
                break;
            default:
                break;
        }
    }
}

void Window::updateTitle(float deltaTimeSeconds) {
    titleUpdateTimer_ += deltaTimeSeconds;
    ++titleUpdateFrames_;

    if (titleUpdateTimer_ < kTitleUpdateInterval) {
        return;
    }

    const float avgFrameMs = (titleUpdateTimer_ / static_cast<float>(titleUpdateFrames_)) * 1000.0f;
    const float fps = static_cast<float>(titleUpdateFrames_) / titleUpdateTimer_;

    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s - %.2f ms (%.0f FPS)", baseTitle_.c_str(), avgFrameMs, fps);
    SDL_SetWindowTitle(window_, buffer);

    titleUpdateTimer_ = 0.0f;
    titleUpdateFrames_ = 0;
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
        throw std::runtime_error(std::string("Failed to create Vulkan surface: ") + SDL_GetError());
    }
    return surface;
}

std::vector<const char*> Window::getRequiredInstanceExtensions() const {
    Uint32 count = 0;
    char const* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) {
        throw std::runtime_error(std::string("Failed to query required Vulkan extensions: ") + SDL_GetError());
    }
    return std::vector<const char*>(names, names + count);
}

bool Window::consumeResizedFlag() {
    const bool wasResized = resized_;
    resized_ = false;
    return wasResized;
}

void Window::getFramebufferSize(int& width, int& height) const {
    SDL_GetWindowSizeInPixels(window_, &width, &height);
}

} // namespace keel
