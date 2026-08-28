#include "ShaderModule.h"

#include "DebugUtils.h"
#include "VkCheck.h"
#include "VulkanContext.h"

#include <SDL3/SDL.h>

#include <fstream>
#include <stdexcept>
#include <vector>

namespace keel {

namespace {

std::vector<char> readSpirvFile(const std::string& relativePath) {
    // Trailing separator included; independent of the process's cwd.
    const char* basePath = SDL_GetBasePath();
    const std::string fullPath = basePath ? (std::string(basePath) + relativePath) : relativePath;

    std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + fullPath);
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("Shader file is empty: " + fullPath);
    }

    std::vector<char> buffer(static_cast<size_t>(size));
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

} // namespace

ShaderModule::ShaderModule(VulkanContext& context, const std::string& relativePath, const char* debugName)
    : context_(context) {
    const std::vector<char> code = readSpirvFile(relativePath);

    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    vkCheck(vkCreateShaderModule(context_.device(), &createInfo, nullptr, &module_),
            "Failed to create shader module");

    setDebugObjectName(context_.device(), VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<uint64_t>(module_),
                        debugName);
}

ShaderModule::~ShaderModule() {
    if (module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(context_.device(), module_, nullptr);
    }
}

} // namespace keel
