#pragma once

#include <volk.h>

#include <stdexcept>
#include <string>

namespace keel {

// Throws std::runtime_error with `message` and the VkResult code if `result`
// is not VK_SUCCESS.
inline void vkCheck(VkResult result, const char* message) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(message) + " (VkResult = " + std::to_string(result) + ")");
    }
}

} // namespace keel
