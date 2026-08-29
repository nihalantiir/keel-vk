// Headless: does not link Vulkan or SDL. Reserved for a future dedicated
// server once keel-net grows beyond this transport-only wrap.
#include "../net/Host.h"

#include <keel/Version.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        std::cout << "keel-vk-server: working (v" << keel::kVersionString << ")" << std::endl;

        if (!net::Host::initialize()) {
            throw std::runtime_error("Failed to initialize ENet");
        }

        constexpr uint16_t kListenPort = 7777;
        net::Host host(kListenPort);
        std::cout << "keel-vk-server: listening on UDP " << kListenPort << std::endl;

        // No signal handling yet: this exits via being killed, not a
        // graceful shutdown path. Fine for a transport-only stub.
        for (;;) {
            host.service(1000);
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
