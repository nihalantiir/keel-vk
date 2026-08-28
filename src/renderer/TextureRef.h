#pragma once

#include <glm/vec4.hpp>

#include <cstdint>

namespace renderer {

// The one handle type the rest of the renderer names a texture by,
// regardless of which residency path actually stores it. Internals stay
// three separate classes (TextureStreamer's bindless array,
// TextureArray2D's single sampler2DArray, Atlas2D's single packed page,
// see the wiki's Rendering page) - a material or instance only needs to
// carry one of these to name a texture in any of them.
enum class TextureKind : uint32_t {
    Bindless = 0,
    Array = 1,
    Atlas = 2,
};

struct TextureRef {
    TextureKind kind = TextureKind::Bindless;
    uint32_t index = 0;                            // bindless slot (Bindless) or layer (Array); unused for Atlas
    glm::vec4 atlasUvRect{0.0f, 0.0f, 1.0f, 1.0f}; // u0, v0, u1, v1; only meaningful for Atlas
};

} // namespace renderer
