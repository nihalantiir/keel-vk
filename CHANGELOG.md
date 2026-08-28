# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- README: `Keel` heading, centered banner, badge row (License, Release,
  CI, Platforms, C++20, Vulkan 1.3), documentation section now links the
  wiki instead of in-tree docs.
- Moved `docs/*.md` to the wiki (Architecture, Vulkan bootstrap, Device
  contract, Rendering, Shaders, Libraries, Extending, Coding conventions,
  Build, Troubleshooting, plus a rewritten Home). `docs/` no longer exists
  in the repo.
- Repo description and topics set on GitHub.

### Added

- `.github/banner.svg`: a hand-authored isometric cube, hue-distinct
  faces, matching the actual demo's palette.
- Bindless texture on the cube: a 16-slot, update-after-bind sampled-image
  descriptor array (the device contract already required the feature bits;
  this is the first thing to use them), with one slot filled by a
  procedurally generated checker texture uploaded through a one-shot
  transfer. `cube.vert`/`cube.frag` gained a UV attribute and sample the
  bindless array by push-constant index, modulated by the existing
  hue-phase color. Overlay shows slots used.
- `shaderSampledImageArrayNonUniformIndexing` added to the required device
  contract: needed to index a bindless array with `nonuniformEXT`, which
  the earlier descriptor-indexing feature bits alone don't cover.
- Shader compilation now passes `--target-env=vulkan1.3` to `glslc`.

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
  the wiki's Device contract page.
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
- Architecture, Device contract, Rendering, Extending, Libraries, and
  Coding conventions wiki pages.
- `.clang-format` (LLVM base, 4-space indent, 120 columns, left-aligned
  pointers), matching simple-vk exactly.
