#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace frame {

// Data, not a controller: something else (main(), later an input system)
// writes position/front/up/lens parameters each frame. Camera only turns
// that into view/proj. No SDL timer, no input polling in here.
struct Camera {
    glm::vec3 position{0.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};

    float verticalFovRadians = glm::radians(45.0f);
    float nearPlane = 0.1f;

    // Floating origin: subtracted from world-space translations before
    // they reach the GPU (both here and by whoever builds a model matrix),
    // so world coordinates far from (0,0,0) don't lose float precision in
    // the view/proj pipeline. Zero, and therefore a no-op, until something
    // moves it.
    glm::vec3 origin{0.0f};

    glm::mat4 view() const {
        const glm::vec3 eye = position - origin;
        return glm::lookAt(eye, eye + front, up);
    }

    // Reverse-Z, infinite far plane: near maps to clip-space z/w = 1,
    // infinity maps to 0. Pairs with a depth clear of 0.0 and compare op
    // GREATER_OR_EQUAL (see renderer::Renderer::createPipeline). This
    // trades the usual float-depth precision loss near the far plane for
    // uniform precision across the whole visible range, and removes the
    // far-plane clip entirely. Derivation recorded on the wiki's Rendering
    // page rather than here.
    glm::mat4 projection(float aspect) const {
        const float f = 1.0f / std::tan(verticalFovRadians * 0.5f);
        glm::mat4 proj(0.0f);
        proj[0][0] = f / aspect;
        proj[1][1] = f;
        proj[2][2] = 0.0f;
        proj[2][3] = -1.0f;
        proj[3][2] = nearPlane;
        // GLM's convention assumes OpenGL's Y-up clip space; Vulkan's NDC Y
        // points down. Flipping this one entry is the standard fix, same
        // as the finite-far projection this replaces.
        proj[1][1] *= -1.0f;
        return proj;
    }
};

} // namespace frame
