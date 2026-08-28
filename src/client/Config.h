#pragma once

#include <volk.h>

#include <cstdint>
#include <string>

namespace client {

// Everything the client executable needs to start, gathered in one place
// instead of scattered across main()'s locals and ad hoc CLI parsing.
// Load order: the defaults below, then an optional keel.toml or
// keel.json next to the executable if present, then CLI arguments
// (highest precedence, applied last).
struct Config {
    int windowWidth = 1280;
    int windowHeight = 720;

    // "mailbox" (default), "fifo", or "immediate". Falls back through
    // mailbox -> fifo (always supported) at swapchain creation if the
    // requested mode isn't available - see keel::Swapchain::choosePresentMode
    // and toVkPresentMode() below.
    std::string presentModePreference = "mailbox";

    // Empty means the default: <SDL_GetBasePath()>packages/. Non-empty
    // overrides the packages/ root entirely - see keel::Vfs.
    std::string packageRootOverride;

    // Mirrors the pre-existing --connect flag: false means the client
    // stays entirely idle over the network (see src/net/Host.h).
    bool wantsNet = false;
    std::string connectAddress = "127.0.0.1";
    uint16_t connectPort = 7777;

    // Applies keel.toml/keel.json next to the executable (if present),
    // then argv, on top of the defaults above. Never throws: a missing or
    // unparseable config file is silently skipped, same as an unset flag.
    static Config load(int argc, char** argv);
};

VkPresentModeKHR toVkPresentMode(const std::string& preference);

} // namespace client
