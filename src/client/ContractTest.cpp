#include "ContractTest.h"

#include "../renderer/Ktx2.h"
#include "../shared/Vfs.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>

namespace contract_test {

namespace {

// 24 vertices (4 per face, not shared) so every face gets a flat, uniform
// hue with no interpolation seams across edges. Faces are wound CCW as seen
// from outside; Renderer::createPipeline's frontFace matches that directly
// since the projection's Y-flip affects near and far faces identically and
// doesn't change which triangles should be culled. uv follows the same
// (0,0),(1,0),(1,1),(0,1) order as each face's 4 vertices, so the checker
// texture reads as one full tile per face.
constexpr std::array<renderer::Vertex, 24> kVertices = {{
    // +Z front, hue 0
    {{-0.5f, -0.5f, 0.5f}, 0.0f, {0.0f, 0.0f}},
    {{0.5f, -0.5f, 0.5f}, 0.0f, {1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.5f}, 0.0f, {1.0f, 1.0f}},
    {{-0.5f, 0.5f, 0.5f}, 0.0f, {0.0f, 1.0f}},
    // -Z back, hue 60
    {{0.5f, -0.5f, -0.5f}, 60.0f, {0.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, 60.0f, {1.0f, 0.0f}},
    {{-0.5f, 0.5f, -0.5f}, 60.0f, {1.0f, 1.0f}},
    {{0.5f, 0.5f, -0.5f}, 60.0f, {0.0f, 1.0f}},
    // +X right, hue 120
    {{0.5f, -0.5f, 0.5f}, 120.0f, {0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, 120.0f, {1.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, 120.0f, {1.0f, 1.0f}},
    {{0.5f, 0.5f, 0.5f}, 120.0f, {0.0f, 1.0f}},
    // -X left, hue 180
    {{-0.5f, -0.5f, -0.5f}, 180.0f, {0.0f, 0.0f}},
    {{-0.5f, -0.5f, 0.5f}, 180.0f, {1.0f, 0.0f}},
    {{-0.5f, 0.5f, 0.5f}, 180.0f, {1.0f, 1.0f}},
    {{-0.5f, 0.5f, -0.5f}, 180.0f, {0.0f, 1.0f}},
    // +Y top, hue 240
    {{-0.5f, 0.5f, -0.5f}, 240.0f, {0.0f, 0.0f}},
    {{-0.5f, 0.5f, 0.5f}, 240.0f, {1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.5f}, 240.0f, {1.0f, 1.0f}},
    {{0.5f, 0.5f, -0.5f}, 240.0f, {0.0f, 1.0f}},
    // -Y bottom, hue 300
    {{-0.5f, -0.5f, 0.5f}, 300.0f, {0.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, 300.0f, {1.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, 300.0f, {1.0f, 1.0f}},
    {{0.5f, -0.5f, 0.5f}, 300.0f, {0.0f, 1.0f}},
}};

constexpr std::array<uint32_t, 36> kIndices = {{
    0, 1, 2, 0, 2, 3,       // +Z
    4, 5, 6, 4, 6, 7,       // -Z
    8, 9, 10, 8, 10, 11,    // +X
    12, 13, 14, 12, 14, 15, // -X
    16, 17, 18, 16, 18, 19, // +Y
    20, 21, 22, 20, 22, 23, // -Y
}};

// Local-space bounding sphere radius for the 1x1x1 cube (half-extent 0.5
// on every axis): sqrt(3 * 0.5^2). Rotation doesn't change a sphere's
// radius, so this stays constant regardless of the cube's current
// orientation; only a non-uniform scale would invalidate it.
constexpr float kCubeBoundsRadius = 0.8660254f;

// A ring of small cubes around the hero, not gameplay - population for
// the CPU frustum cull to actually reject something. Radius/count/speed
// are picked to sit at the edge of the default camera's ~45-degree FOV,
// so dollying in (Renderer::cameraDistance()) visibly starts culling
// some of them without needing any other control.
constexpr uint32_t kSatelliteCount = 12;
constexpr float kSatelliteRingRadius = 1.8f;
constexpr float kSatelliteOrbitDegPerSec = 8.0f;
constexpr float kSatelliteScale = 0.35f;

// The contract test's own eye point: same one simple-vk's old lookAt
// used. Renderer's own default is a generic, untuned "look at the
// origin from distance 3" - this repo overrides it here, once, for the
// scene it actually draws.
constexpr glm::vec3 kCameraEye = glm::vec3(2.2f, 1.8f, 2.6f);
constexpr float kPhaseSpeedDegPerSec = 60.0f;

// 8x8 checkerboard, loaded from the base content pack as raw RGBA8 bytes:
// no image-loading dependency (see the wiki's Libraries page - stb_image
// is deliberately not used), just a format known ahead of time.
constexpr uint32_t kCheckerSize = 8;

std::array<uint8_t, kCheckerSize * kCheckerSize * 4> loadCheckerPixels(keel::Vfs& vfs) {
    const std::string path = vfs.resolve("textures/checker.rgba8");

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open checker texture: " + path);
    }

    std::array<uint8_t, kCheckerSize * kCheckerSize * 4> pixels{};
    file.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (file.gcount() != static_cast<std::streamsize>(pixels.size())) {
        throw std::runtime_error("Checker texture has the wrong size: " + path);
    }
    return pixels;
}

// Demo content for the bindless streaming path: generated, not loaded, so
// TextureStreamer::allocate() (via Renderer::registerTexture) has
// more than one real texture to cycle between without needing more
// content-pack assets.
std::vector<uint8_t> makeStripePattern(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const bool light = (x / 2) % 2 == 0;
            const uint8_t value = light ? 235 : 60;
            uint8_t* px = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            px[0] = value;
            px[1] = static_cast<uint8_t>(value * 0.6f);
            px[2] = static_cast<uint8_t>(value * 0.9f);
            px[3] = 255;
        }
    }
    return pixels;
}

std::vector<uint8_t> makeGradientPattern(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* px = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            px[0] = static_cast<uint8_t>(255.0f * x / static_cast<float>(width - 1));
            px[1] = static_cast<uint8_t>(255.0f * y / static_cast<float>(height - 1));
            px[2] = 200;
            px[3] = 255;
        }
    }
    return pixels;
}

std::vector<uint8_t> makeSolidPattern(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }
    return pixels;
}

} // namespace

renderer::PipelineSpec pipelineSpec() {
    renderer::PipelineSpec spec;
    spec.vertPath = "shaders/cube.vert.spv";
    spec.fragPath = "shaders/cube.frag.spv";
    spec.vertDebugName = "cube.vert";
    spec.fragDebugName = "cube.frag";
    return spec;
}

void spawnScene(renderer::Renderer& renderer, keel::Vfs& vfs) {
    renderer.allocateMesh(kVertices.data(), static_cast<uint32_t>(kVertices.size()), kIndices.data(),
                           static_cast<uint32_t>(kIndices.size()));

    renderer.camera().position = kCameraEye;
    renderer.camera().front = glm::normalize(-kCameraEye);
    renderer.cameraDistance() = glm::length(kCameraEye);
    renderer.phaseSpeed() = kPhaseSpeedDegPerSec;

    const std::array<uint8_t, kCheckerSize * kCheckerSize * 4> checkerPixels = loadCheckerPixels(vfs);
    renderer.registerTexture(kCheckerSize, kCheckerSize, checkerPixels.data(), "checker (packages/base)");

    const std::vector<uint8_t> stripes = makeStripePattern(16, 16);
    renderer.registerTexture(16, 16, stripes.data(), "stripes (generated)");

    const std::vector<uint8_t> gradient = makeGradientPattern(16, 16);
    renderer.registerTexture(16, 16, gradient.data(), "gradient (generated)");

    const std::vector<uint8_t> spare = makeSolidPattern(8, 8, 200, 200, 200);
    renderer.registerTexture(8, 8, spare.data(), "spare (generated)");

    // The one cooked-format fixture: BC7, read from a real KTX2 container
    // (see Ktx2.h) instead of the raw-bytes-known-ahead-of-time trick the
    // RGBA8 demo textures above use. Lands in the same bindless rotation
    // as everything else - sampling is format-agnostic once the image is
    // resident, so no shader change was needed to add this.
    const renderer::Ktx2Image bc7Fixture = renderer::loadKtx2(vfs.resolve("textures/demo_bc7.ktx2"));
    if (bc7Fixture.format != VK_FORMAT_BC7_UNORM_BLOCK) {
        throw std::runtime_error("demo_bc7.ktx2: expected VK_FORMAT_BC7_UNORM_BLOCK");
    }
    renderer.registerTextureCompressed(bc7Fixture.width, bc7Fixture.height, bc7Fixture.format,
                                            bc7Fixture.data.data(), bc7Fixture.data.size(),
                                            "demo (BC7, packages/base)");
}

std::vector<renderer::InstanceDesc> buildInstances(renderer::Renderer& renderer, const glm::mat4& heroModel,
                                                     float elapsedTimeSeconds) {
    std::vector<renderer::InstanceDesc> instances;
    instances.reserve(1 + kSatelliteCount);

    // The hero: whichever of the three residency paths the debug
    // overlay's "Residency mode" control currently selects - visible
    // proof all three actually work, not just constructed. See
    // TextureRef.h.
    renderer::InstanceDesc hero;
    hero.model = heroModel;
    hero.boundsRadius = kCubeBoundsRadius;
    hero.texture.kind = renderer.residencyMode();
    switch (renderer.residencyMode()) {
        case renderer::TextureKind::Array: {
            const uint32_t layerCount = renderer.textureArrayLayerCount();
            hero.texture.index = layerCount == 0 ? 0 : static_cast<uint32_t>(renderer.demoArrayLayer()) % layerCount;
            break;
        }
        case renderer::TextureKind::Atlas: {
            const renderer::AtlasRect rect = renderer.atlasRect(static_cast<uint32_t>(renderer.demoAtlasRectIndex()));
            hero.texture.atlasUvRect = glm::vec4(rect.u0, rect.v0, rect.u1, rect.v1);
            break;
        }
        case renderer::TextureKind::Bindless:
        default:
            hero.texture.index = renderer.activeDemoTextureSlot();
            break;
    }
    instances.push_back(hero);

    // Satellites: a small static ring around the hero, not gameplay,
    // just enough population for the frustum cull to reject something
    // real instead of always seeing exactly one instance. Same mesh as
    // the hero, smaller scale, cycling through all three residency kinds
    // round-robin so bindless/array/atlas are all sampled in one frame
    // regardless of the hero's own residency-mode picker. See the wiki's
    // Rendering page.
    for (uint32_t i = 0; i < kSatelliteCount; ++i) {
        const float baseAngleDeg = static_cast<float>(i) * (360.0f / static_cast<float>(kSatelliteCount));
        const float angleRad = glm::radians(baseAngleDeg + elapsedTimeSeconds * kSatelliteOrbitDegPerSec);
        const glm::vec3 worldPos(kSatelliteRingRadius * std::cos(angleRad), 0.0f,
                                  kSatelliteRingRadius * std::sin(angleRad));

        renderer::InstanceDesc satellite;
        satellite.model =
            glm::translate(glm::mat4(1.0f), worldPos) * glm::scale(glm::mat4(1.0f), glm::vec3(kSatelliteScale));
        satellite.boundsRadius = kCubeBoundsRadius * kSatelliteScale;

        switch (i % 3) {
            case 0:
                satellite.texture.kind = renderer::TextureKind::Bindless;
                satellite.texture.index = renderer.demoTextureSlotAt(i);
                break;
            case 1: {
                const uint32_t layerCount = renderer.textureArrayLayerCount();
                satellite.texture.kind = renderer::TextureKind::Array;
                satellite.texture.index = layerCount == 0 ? 0 : i % layerCount;
                break;
            }
            case 2:
            default: {
                const renderer::AtlasRect rect = renderer.atlasRect(i);
                satellite.texture.kind = renderer::TextureKind::Atlas;
                satellite.texture.atlasUvRect = glm::vec4(rect.u0, rect.v0, rect.u1, rect.v1);
                break;
            }
        }
        instances.push_back(satellite);
    }

    return instances;
}

} // namespace contract_test
