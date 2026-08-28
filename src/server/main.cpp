// Headless stub: does not link Vulkan or SDL. Reserved for a future
// dedicated server once keel-net exists; today it only proves the split
// between the client (renders) and server (never touches a GPU) builds.
#include <cstdlib>
#include <iostream>

int main() {
    std::cout << "keel-vk-server: working (v0.1.0)" << std::endl;
    return EXIT_SUCCESS;
}
