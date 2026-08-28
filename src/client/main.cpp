#include "../keel-vk/Swapchain.h"
#include "../keel-vk/VulkanContext.h"
#include "../keel-vk/Window.h"
#include "../net/Host.h"
#include "../renderer/Renderer.h"
#include "../shared/Clock.h"
#include "../shared/Components.h"
#include "../shared/Vfs.h"
#include "../shared/World.h"
#include "ActionMap.h"

#if KEEL_VK_IMGUI
#include "../debug/DebugUi.h"
#endif

#include <SDL3/SDL.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

namespace {

// "--connect" alone means 127.0.0.1:7777; "--connect=host" or
// "--connect=host:port" override either part.
bool parseConnectFlag(int argc, char** argv, std::string& address, uint16_t& port) {
    address = "127.0.0.1";
    port = 7777;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--connect", 0) != 0) {
            continue;
        }
        const size_t eq = arg.find('=');
        if (eq != std::string::npos) {
            const std::string value = arg.substr(eq + 1);
            const size_t colon = value.find(':');
            if (colon != std::string::npos) {
                address = value.substr(0, colon);
                port = static_cast<uint16_t>(std::stoi(value.substr(colon + 1)));
            } else {
                address = value;
            }
        }
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string connectAddress;
        uint16_t connectPort = 0;
        const bool wantsNet = parseConnectFlag(argc, argv, connectAddress, connectPort);

        // Stays entirely unconstructed (no ENet init, no host) unless
        // --connect was passed: the client is idle over the network by
        // default.
        std::unique_ptr<net::Host> netHost;
        if (wantsNet) {
            if (!net::Host::initialize()) {
                throw std::runtime_error("Failed to initialize ENet");
            }
            netHost = std::make_unique<net::Host>(0);
            netHost->connect(connectAddress, connectPort);
            std::cout << "keel-vk: connecting to " << connectAddress << ":" << connectPort << std::endl;
        }

        keel::Window window("keel-vk: working (v0.1.0)", 1280, 720);
        keel::Vfs vfs; // mounts packages/ next to the executable
        keel::VulkanContext context(window);
        keel::Swapchain swapchain(context, window);
        renderer::Renderer renderer(context, swapchain, window, vfs);
#if KEEL_VK_IMGUI
        debug::DebugUi debugUi(context, swapchain, window, renderer);
#endif
        client::ActionMap actionMap;
        shared::FixedClock fixedClock;

        keel::World world;
        const keel::EntityId cube = world.createEntity();
        world.addComponent<keel::Transform>(cube);
        world.addComponent<keel::Name>(cube, "Cube");
        world.addComponent<keel::Visible>(cube);

        float heartbeatTimer = 0.0f;

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

            if (netHost) {
                netHost->service(0); // non-blocking: never stall the render loop
                heartbeatTimer += deltaTime;
                if (heartbeatTimer >= 1.0f) {
                    netHost->sendHeartbeat();
                    heartbeatTimer = 0.0f;
                }
            }

            // The cube's Transform is the single source of truth for its
            // model matrix: Renderer only consumes whatever setModel() is
            // given, it doesn't compute rotation itself.
            if (!renderer.paused()) {
                keel::Transform& transform = world.getComponent<keel::Transform>(cube);
                transform.eulerAnglesRadians.y += deltaTime * 0.6f;
                transform.eulerAnglesRadians.x += deltaTime * 0.4f;
            }
            renderer.setModel(keel::toMatrix(world.getComponent<keel::Transform>(cube)));

#if KEEL_VK_IMGUI
            debugUi.beginFrame();
            renderer.drawFrame(&debugUi);
#else
            renderer.drawFrame();
#endif
            window.updateTitle(deltaTime);
        }

        if (wantsNet) {
            net::Host::shutdown();
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
