#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <string>

namespace keel {

struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 eulerAnglesRadians{0.0f};
    glm::vec3 scale{1.0f};
};

struct Name {
    std::string value;
};

// Reserved for keel-net: not written or read by anything yet.
struct NetId {
    uint32_t value = 0;
};

// Local-space half-extents, for future culling/picking. Not used by the
// renderer yet; the cube's actual draw geometry is unrelated to this.
struct Bounds {
    glm::vec3 halfExtents{0.5f};
};

// Tag: presence means "extract this entity's Transform for rendering."
struct Visible {};

inline glm::mat4 toMatrix(const Transform& transform) {
    glm::mat4 model = glm::translate(glm::mat4(1.0f), transform.position);
    model = glm::rotate(model, transform.eulerAnglesRadians.y, glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, transform.eulerAnglesRadians.x, glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, transform.eulerAnglesRadians.z, glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, transform.scale);
    return model;
}

} // namespace keel
