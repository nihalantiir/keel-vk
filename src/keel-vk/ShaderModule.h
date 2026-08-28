#pragma once

#include <volk.h>

#include <string>

namespace keel {
class VulkanContext;

// Loads compiled SPIR-V resolved against the executable's directory
// (SDL_GetBasePath()), not the process's current working directory.
class ShaderModule {
public:
    ShaderModule(VulkanContext& context, const std::string& relativePath, const char* debugName);
    ~ShaderModule();

    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;

    VkShaderModule handle() const { return module_; }

private:
    VulkanContext& context_;
    VkShaderModule module_ = VK_NULL_HANDLE;
};

} // namespace keel
