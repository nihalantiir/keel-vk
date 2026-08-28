#pragma once

#include <glm/glm.hpp>

#include <array>

namespace renderer {

// Five clip-space half-spaces (left, right, bottom, top, near) extracted
// from a combined view-projection matrix. No far plane: paired with
// frame::Camera's infinite-far projection (see the wiki's Rendering
// page), there isn't a far plane to cull against - clip.z never goes
// negative as depth approaches infinity, only toward zero.
//
// Vulkan clip space specifically: the near-plane half-space is
// "0 <= z <= w" collapsed to just "z <= w" (w - z >= 0), not OpenGL's
// symmetric "-w <= z <= w" near+far pair. Deriving this from the wrong
// convention is a real way to get culling silently wrong; see the wiki
// for the derivation instead of re-deriving it in a comment here.
class Frustum {
public:
    explicit Frustum(const glm::mat4& viewProj) {
        const glm::vec4 row0 = rowOf(viewProj, 0);
        const glm::vec4 row1 = rowOf(viewProj, 1);
        const glm::vec4 row2 = rowOf(viewProj, 2);
        const glm::vec4 row3 = rowOf(viewProj, 3);
        planes_[0] = normalizePlane(row3 + row0); // left:   w + x >= 0
        planes_[1] = normalizePlane(row3 - row0); // right:  w - x >= 0
        planes_[2] = normalizePlane(row3 + row1); // bottom: w + y >= 0
        planes_[3] = normalizePlane(row3 - row1); // top:    w - y >= 0
        planes_[4] = normalizePlane(row3 - row2); // near:   w - z >= 0
    }

    // False only when the sphere is entirely outside at least one plane;
    // true for "inside or intersecting", the conservative direction for a
    // cull test to be wrong in.
    bool intersectsSphere(const glm::vec3& center, float radius) const {
        for (const glm::vec4& plane : planes_) {
            const float distance = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
            if (distance < -radius) {
                return false;
            }
        }
        return true;
    }

private:
    // GLM stores mat4 column-major (m[col][row]); this reads one
    // mathematical row across all four columns.
    static glm::vec4 rowOf(const glm::mat4& m, int row) {
        return {m[0][row], m[1][row], m[2][row], m[3][row]};
    }

    static glm::vec4 normalizePlane(const glm::vec4& plane) {
        const float length = glm::length(glm::vec3(plane));
        return length > 0.0f ? plane / length : plane;
    }

    std::array<glm::vec4, 5> planes_{};
};

} // namespace renderer
