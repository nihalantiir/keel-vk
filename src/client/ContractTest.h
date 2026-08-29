#pragma once

#include "../renderer/Renderer.h"

#include <glm/mat4x4.hpp>

#include <vector>

namespace keel {
class Vfs;
}

namespace contract_test {

// Everything about the hero cube, the satellite ring, and the demo
// textures - this repo's own contract test, not part of the keel
// library. A consumer linking only keel and never calling into this
// file gets an empty world and a grey clear; see the wiki's Extending
// page. Lives in src/client/ specifically because it's exe-only
// composition, the same way main.cpp is.

// The cube shaders this contract test draws with. Renderer's pipeline
// isn't content-agnostic (fixed vertex layout, descriptor sets, dynamic
// rendering formats - see PipelineSpec's own comment), but the shader
// files it loads are: this is where this repo picks its own. Call
// before constructing Renderer; the result is a required constructor
// argument.
renderer::PipelineSpec pipelineSpec();

// Allocates the cube mesh, registers the demo bindless/BC7 textures, and
// sets the camera/phase-speed defaults this contract test is actually
// tuned for. Call once, after constructing Renderer and before the
// first drawFrame().
void spawnScene(renderer::Renderer& renderer, keel::Vfs& vfs);

// The hero (from heroModel, this frame's World-driven transform) plus
// the satellite ring, cycling through all three residency kinds so
// bindless/array/atlas are all sampled every frame regardless of the
// hero's own residency-mode picker. Call every frame, then pass the
// result to Renderer::setInstances().
std::vector<renderer::InstanceDesc> buildInstances(renderer::Renderer& renderer, const glm::mat4& heroModel,
                                                     float elapsedTimeSeconds);

} // namespace contract_test
