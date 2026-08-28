# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-08-28

### Added

- Vulkan 1.3 bootstrap absorbed from simple-vk (`src/keel-vk/`): `Window`,
  `VulkanContext`, `Swapchain`, `ShaderModule`, `vkCheck`,
  `setDebugObjectName`, VMA integration. Same conventions, same
  construction order, same resize-safe swapchain recreation.
- Device contract: `VulkanContext` now requires and verifies dynamic
  rendering, synchronization2, maintenance4, timeline semaphores, buffer
  device address, the full update-after-bind descriptor indexing family,
  scalar block layout, shader draw parameters, and multi-draw indirect
  before picking a physical device; if no device qualifies, the error
  lists which requirements each candidate device was missing.
  `VK_EXT_memory_budget`, calibrated timestamps, extended dynamic state
  2/3, and host query reset are enabled when present, never required. See
  `docs/Device-contract.md`.
- `src/renderer/Renderer`: a perspective, depth-tested, continuously
  rotating cube. 24-vertex indexed mesh (one buffer, uploaded once), drawn
  with `vkCmdDrawIndexedIndirect` against a one-entry indirect buffer as
  scaffolding for GPU-driven batching later. `VK_FORMAT_D32_SFLOAT` depth
  target, recreated alongside the swapchain on resize. MVP, elapsed time,
  and hue phase speed passed as push constants. Image layout transitions
  and queue submission use Vulkan 1.3 synchronization2
  (`vkCmdPipelineBarrier2`, `vkQueueSubmit2`).
- Per-face hue cycling: each face carries a base hue 60 degrees apart from
  its neighbors; `cube.vert` phases it by elapsed time and a live-editable
  phase speed, converting HSV to RGB per vertex.
- `src/debug/DebugUi`: same Dear ImGui integration pattern as simple-vk
  (SDL3 + Vulkan backends via volk, same dynamic rendering pass), overlay
  now also exposes a phase-speed slider and a read-only current-hue swatch
  alongside frame time/FPS, swapchain info, device name, and the clear
  color editor. `KEEL_VK_IMGUI` CMake option (default `ON`) controls
  fetching Dear ImGui and compiling `src/debug/` at all, same switch
  pattern as simple-vk's `SIMPLE_VK_IMGUI`.
- `src/server/`: a headless stub executable (`keel-vk-server`) that links
  neither Vulkan nor SDL, reserved for a future dedicated server built on
  keel-net.
- `ship` CMake preset (Release + `KEEL_VK_IMGUI=OFF`), alongside
  debug/release/relwithdebinfo, mirroring simple-vk.
- `.github/workflows/ci.yml`: configure + build on Windows and Linux, both
  with and without `KEEL_VK_IMGUI`.
- `docs/Architecture.md`, `docs/Device-contract.md`, `docs/Rendering.md`,
  `docs/Extending.md`, `docs/Libraries.md`, `docs/Coding-conventions.md`.
- `.clang-format` (LLVM base, 4-space indent, 120 columns, left-aligned
  pointers), matching simple-vk exactly.
