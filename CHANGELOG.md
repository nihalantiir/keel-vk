# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - 2026-08-28

The cube gains an entity behind it, a texture, three ways to store
textures, and a network transport, without becoming a game. Public face
(README, banner) now matches what actually runs, and deeper docs moved to
the wiki.

### Added

- **ECS**: `keel::World` (`src/shared/World.h`), a thin facade over EnTT
  (`skypjack/entt`, pinned `v4.0.0`, vendored, header-only), holding
  engine-level components only (`Transform`, `Name`, `NetId`, `Bounds`, a
  `Visible` tag - see `src/shared/Components.h`). `main()` creates one
  entity with a `Transform`; its matrix, not an internal calculation in
  `Renderer`, now drives the cube's rotation.
- **Input**: `src/client/ActionMap` lifts SDL3 key events into named
  actions. Space toggles pause (freezes rotation and hue phase), Escape
  quits.
- **Fixed timestep**: `src/shared/Clock.h`'s `FixedClock`, a generic
  accumulator not tied to SDL or Vulkan, wired into the client loop and
  reserved for a future fixed-rate system (none exists yet).
- **Networking**: `net::Host` (`src/net/Host.h`), a transport-only ENet
  wrap (`lsalzman/enet`, pinned `v1.3.18`, vendored, built as a small
  static library). `src/net/Protocol.h` defines a one-byte version plus a
  message type; `Heartbeat` is the only message so far. `keel-vk-server`
  always listens on UDP 7777 and remains Vulkan/SDL-free; `keel-vk` (the
  client) stays idle over the network unless launched with `--connect` or
  `--connect=host[:port]`.
- **Content packs**: `keel::Vfs` (`src/shared/Vfs.h`) mounts every
  subdirectory of `packages/` that has a `package.json`. `packages/base/`
  is the first real package; the cube's checker texture now loads through
  it instead of being generated inline.
- **Bindless textures**: the cube samples from a 256-slot,
  update-after-bind sampled-image descriptor array
  (`src/renderer/TextureStreamer`) - the device contract already required
  the feature bits this uses. It's a real streaming pool, not one static
  texture: `allocate`/`update`/`free`, a ring of persistently-mapped
  staging buffers, per-slot generations, and uploads queued and drained
  once per frame without ever blocking the render loop (the one
  `vkQueueWaitIdle` left in the texture path is a single flush before the
  first frame). The debug overlay's texture-slot selector, "Regenerate
  active" button, and "Free + reallocate spare slot" button exercise all
  three live.
- **Two more texture-storage paths**, populated but not yet sampled by
  anything, to prove each works before something needs it:
  `src/renderer/TextureArray2D` (one image, many same-size layers, for
  things like terrain splat layers later) and `src/renderer/Atlas2D` (one
  packed page with a shelf packer and a UV rect table, for UI/glyphs
  later).
- **Dedicated transfer queue**: `VulkanContext` now discovers a
  transfer-only queue family when the device has one and exposes it as
  `uploadQueue()`. Construction-time texture uploads (the streamer's
  startup flush, the array, the atlas) run on it via `VK_SHARING_MODE_CONCURRENT`
  images, falling back to the graphics queue on devices without one. The
  per-frame streaming path deliberately stays on the graphics queue to
  avoid cross-queue synchronization on the hot path.
- **GPU timing**: `VK_QUERY_TYPE_TIMESTAMP` queries around each frame's
  render work, read back once the frame's own fence confirms completion.
  Shown in the overlay as "GPU: N ms" alongside the existing CPU-side FPS
  reading, or "unavailable" on a device without
  `timestampComputeAndGraphics`.
- **Pipeline cache on disk**: saved next to the executable on clean exit,
  loaded on the next run so the driver can skip shader recompilation. Not
  a correctness requirement; an invalid or missing cache is silently
  ignored.
- Public face: README gets a `Keel` H1, a real perspective-cube banner
  (computed from the same camera sense and hue mapping the renderer uses,
  not a flat isometric placeholder), and a badge row. Repo description and
  topics set on GitHub. `docs/*.md` moved to the wiki; `docs/` no longer
  exists in the repo.

### Changed

- `shaderSampledImageArrayNonUniformIndexing` added to the required device
  contract: indexing a bindless array with `nonuniformEXT` needs it
  specifically, on top of the descriptor-indexing bits already required.
- Shader compilation now passes `--target-env=vulkan1.3` to `glslc`, so
  the descriptor-indexing SPIR-V bindless texturing needs actually
  matches the device contract.
- CMake project version (and the window title) is now 0.2.0.

### Fixed

- `Host::service()`'s event-drain loop never re-polled `enet_host_service`,
  so it would only ever see the first queued event per call.
- The texture streamer's startup batch could queue more uploads than the
  staging ring had room for; the resulting exception mid-construction
  leaked already-created vertex/index/indirect buffers (their cleanup
  lives in `~Renderer()`, which never runs on a failed constructor). Fixed
  by sizing the ring to comfortably exceed the startup batch.
- A one-shot upload barrier unconditionally targeted the fragment shader
  stage, which is invalid when that command buffer is recorded against a
  transfer-only queue family - only reproduced once the dedicated
  transfer queue above was actually wired up and exercised on real
  hardware with one.
- Reading back a GPU timestamp query before its first
  `vkCmdWriteTimestamp2` is a validation error, not just "not ready yet":
  a freshly created query is uninitialized, not merely unavailable. Fixed
  by tracking which frame slots have completed at least one write.

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
