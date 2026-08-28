#include "../keel-vk/Swapchain.h"
#include "../keel-vk/VulkanContext.h"
#include "../keel-vk/Window.h"
#include "../renderer/Renderer.h"
#include "../shared/Clock.h"
#include "ActionMap.h"

#if KEEL_VK_IMGUI
#include "../debug/DebugUi.h"
#endif

#include <SDL3/SDL.h>

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        keel::Window window("keel-vk: working (v0.1.0)", 1280, 720);
        keel::VulkanContext context(window);
        keel::Swapchain swapchain(context, window);
        renderer::Renderer renderer(context, swapchain, window);
#if KEEL_VK_IMGUI
        debug::DebugUi debugUi(context, swapchain, window, renderer);
#endif
        client::ActionMap actionMap;
        shared::FixedClock fixedClock;

        Uint64 lastTicks = SDL_GetPerformanceCounter();
        const Uint64 frequency = SDL_GetPerformanceFrequency();

        while (!window.shouldClose()) {
#if KEEL_VK_IMGUI
            window.pollEvents([&](const SDL_Event& event) {
                actionMap.processEvent(event);
                debugUi.processEvent(event);
            });
#else
            window.pollEvents([&](const SDL_Event& event) { actionMap.processEvent(event); });
#endif
            for (client::Action action : actionMap.consumeActions()) {
                switch (action) {
                    case client::Action::Pause:
                        renderer.paused() = !renderer.paused();
                        break;
                    case client::Action::Quit:
                        window.requestQuit();
                        break;
                }
            }

            int width = 0, height = 0;
            window.getFramebufferSize(width, height);
            if (width == 0 || height == 0) {
                continue; // minimized: skip rendering until the window has a usable size
            }

            const Uint64 now = SDL_GetPerformanceCounter();
            const float deltaTime = static_cast<float>(now - lastTicks) / static_cast<float>(frequency);
            lastTicks = now;

            fixedClock.advance(deltaTime);
            while (fixedClock.consumeStep()) {
                // No fixed-rate system runs yet; draining keeps the
                // accumulator from growing unbounded if the app is paused
                // in a debugger or otherwise stalls for a long frame.
            }

#if KEEL_VK_IMGUI
            debugUi.beginFrame();
            renderer.drawFrame(&debugUi);
#else
            renderer.drawFrame();
#endif
            window.updateTitle(deltaTime);
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
