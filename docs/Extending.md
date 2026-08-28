# Extending

New systems go under a new top-level module (`src/<name>/`), or into
`src/client/` if they're specific to running the client. Do not thicken
`src/keel-vk/`: it stays the generic Vulkan/SDL bootstrap, exactly as it
was in simple-vk's `src/core/`.

Things that explicitly do not belong in `src/keel-vk/` (or anywhere in
this repo yet), and where they're expected to land once they exist:

- **ECS components and world state** -> a future `keel-shared` module
  wrapping EnTT. Not started this landing.
- **Network peers/messages** -> a future `keel-net` module wrapping ENet.
  `src/server/` is reserved for a dedicated server built on it, with zero
  Vulkan dependency (see `src/server/main.cpp` - it links nothing from
  `src/keel-vk/`, `src/renderer/`, or `src/debug/` today).
- **Content packs** -> `packages/`, format not designed yet.
- **Game UI** (Clay, Nuklear, RmlUi) -> see `docs/Libraries.md`. ImGui in
  `src/debug/` is debug-only and is not the game UI layer.
- **Cameras as controllers, glTF loading, audio** -> not started. The
  cube's "camera" is time-driven data (a fixed view + perspective matrix
  recomputed from `elapsedTimeSeconds_`), not an input-driven controller;
  a real camera system is a `src/renderer/` or `src/client/` concern when
  it arrives, still not `src/keel-vk/`.

## Adding a pipeline

Follow `renderer::Renderer::createPipeline()`: load SPIR-V through
`keel::ShaderModule` (path relative to `SDL_GetBasePath()`, never cwd),
chain a `VkPipelineRenderingCreateInfo` onto `VkGraphicsPipelineCreateInfo`
(no render pass), keep viewport/scissor dynamic so resize never requires a
pipeline rebuild. Add new shader sources under `shaders/` and to
`SHADER_SOURCES` in `CMakeLists.txt`.

## Adding a draw

If it's static geometry, follow `createGeometryBuffers()`: one VMA buffer,
host-visible mapped, written once. If it changes every frame, follow
simple-vk's old triangle pattern instead - one buffer per frame in flight
(`kFramesInFlight`), to avoid racing a frame still in flight on the GPU
against the next frame's CPU write. Either way, batch it into the existing
indirect buffer (or a new one) rather than adding a second
`vkCmdDrawIndexed` call, to keep the GPU-driven scaffold meaningful as more
draws are added.

## Adding a debug panel

`ImGui::Begin()` / `ImGui::End()` inside `DebugUi::drawOverlay()`, same as
simple-vk. Expose whatever it needs to read or write as a public accessor
on `Renderer` (see `clearColor()`, `phaseSpeed()`, `previewColor()`), not
by handing `DebugUi` raw access to `Renderer`'s private state.

## Asset paths

Resolve the same way `keel::ShaderModule` does: relative to
`SDL_GetBasePath()`, never the process's current working directory.
`assets/` does not exist yet in this repo (nothing loads runtime content
yet); add it when something does.
