#pragma once

#include <volk.h>

#include <functional>
#include <string>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace keel {

class Window {
public:
    using EventCallback = std::function<void(const SDL_Event&)>;

    Window(const std::string& title, int width, int height);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void pollEvents(const EventCallback& onEvent = {});
    void updateTitle(float deltaTimeSeconds);
    VkSurfaceKHR createSurface(VkInstance instance) const;
    std::vector<const char*> getRequiredInstanceExtensions() const;
    void getFramebufferSize(int& width, int& height) const;

    SDL_Window* handle() const { return window_; }
    bool shouldClose() const { return quitRequested_; }
    bool consumeResizedFlag();

private:
    SDL_Window* window_ = nullptr;
    bool quitRequested_ = false;
    bool resized_ = false;
    std::string baseTitle_;
    float titleUpdateTimer_ = 0.0f;
    int titleUpdateFrames_ = 0;
};

} // namespace keel
