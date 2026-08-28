#pragma once

#include <volk.h>

namespace keel {

// Tags a Vulkan object with a name via VK_EXT_debug_utils. Safe to call
// unconditionally: the function pointer is null unless the extension was
// enabled (Debug builds with validation), so this is a no-op otherwise.
inline void setDebugObjectName(VkDevice device, VkObjectType type, uint64_t handle, const char* name) {
    if (!vkSetDebugUtilsObjectNameEXT) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device, &info);
}

} // namespace keel
