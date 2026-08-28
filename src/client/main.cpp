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
#include "Config.h"

#if KEEL_VK_IMGUI
#include "../debug/DebugUi.h"
#endif

#include <SDL3/SDL.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    try {
        const client::Config config = client::Config::load(argc, argv);

        // Stays entirely unconstructed (no ENet init, no host) unless
        // --connect (or connect_address in keel.toml/json) was set: the
        // client is idle over the network by default.
        std::unique_ptr<net::Host> netHost;
        if (config.wantsNet) {
            if (!net::Host::initialize()) {
                throw std::runtime_error("Failed to initialize ENet");
            }
            netHost = std::make_unique<net::Host>(0);
            netHost->connect(config.connectAddress, config.connectPort);
            std::cout << "keel-vk: connecting to " << config.connectAddress << ":" << config.connectPort
                       << std::endl;
        }

        keel::Window window("keel-vk: working (v0.6.0)", config.windowWidth, config.windowHeight);
        keel::Vfs vfs(config.packageRootOverride); // mounts packages/ next to the executable, or the override
        keel::VulkanContext context(window);
        keel::Swapchain swapchain(context, window, client::toVkPresentMode(config.presentModePreference));
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

        // The Transform one step behind cube's current one, for
        // interpolating render state between sim steps (see FixedClock::
        // alpha()). Starts equal so the first frame's lerp is a no-op.
        keel::Transform previousTransform = world.getComponent<keel::Transform>(cube);

        // The renderer's one sim-time clock (hue phase, satellite orbiting):
        // accumulated here, gated on the same pause flag as fixedClock
        // below, and pushed to Renderer via setSimTime() each frame -
        // not a second SDL timer of Renderer's own. See the wiki's
        // Rendering page for why this used to be two clocks.
        float simTimeSeconds = 0.0f;

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
            const client::Axes axes = actionMap.consumeAxes();

            int width = 0, height = 0;
            window.getFramebufferSize(width, height);
            if (width == 0 || height == 0) {
                continue; // minimized: skip rendering until the window has a usable size
            }

            const Uint64 now = SDL_GetPerformanceCounter();
            const float deltaTime = static_cast<float>(now - lastTicks) / static_cast<float>(frequency);
            lastTicks = now;

            // Pause freezes sim time: advance() isn't called at all, so the
            // accumulator stops growing and consumeStep() never fires
            // again below until unpaused. Rendering keeps happening either
            // way (drawFrame() runs unconditionally further down).
            if (!renderer.paused()) {
                fixedClock.advance(deltaTime);
                simTimeSeconds += deltaTime;
            }
            renderer.setSimTime(simTimeSeconds);
            while (fixedClock.consumeStep()) {
                keel::Transform& transform = world.getComponent<keel::Transform>(cube);
                previousTransform = transform;
                const float stepDelta = fixedClock.fixedDelta();
                transform.eulerAnglesRadians.y += stepDelta * 0.6f;
                transform.eulerAnglesRadians.x += stepDelta * 0.4f;
            }

            if (netHost) {
                netHost->service(0); // non-blocking: never stall the render loop
                heartbeatTimer += deltaTime;
                if (heartbeatTimer >= 1.0f) {
                    netHost->sendHeartbeat();
                    heartbeatTimer = 0.0f;
                }
            }

            // The cube's Transform is the sim's source of truth, updated
            // only in fixed steps above; what Renderer actually draws is
            // interpolated between the last two steps by however far the
            // accumulator has drifted into the next one (FixedClock::
            // alpha()), so motion stays smooth at a frame rate that
            // doesn't divide evenly into the fixed step.
            const keel::Transform& currentTransform = world.getComponent<keel::Transform>(cube);
            const keel::Transform renderTransform =
                keel::lerp(previousTransform, currentTransform, fixedClock.alpha());
            renderer.setModel(keel::toMatrix(renderTransform));

#if KEEL_VK_IMGUI
            debugUi.beginFrame(axes.mouseDeltaX, axes.mouseDeltaY, axes.moveX, axes.moveY);
            renderer.drawFrame(&debugUi);
#else
            (void)axes; // nothing to feed it to without the overlay
            renderer.drawFrame();
#endif
            window.updateTitle(deltaTime);
        }

        if (config.wantsNet) {
            net::Host::shutdown();
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
