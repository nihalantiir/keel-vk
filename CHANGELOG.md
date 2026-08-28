# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.4.0] - 2026-08-28

The GPU scene stops being true only for one instance. The stock cube
gains a small orbiting satellite ring so the frustum cull actually
rejects something; the mesh pool moves off host-visible memory onto real
`DEVICE_LOCAL` buffers; bindless texture residency gains a real (if
artificially capped, for demo purposes) eviction path against the
device's actual `VK_EXT_memory_budget` numbers; and hue phase stops
running on a second clock of its own. No genre content, no gameplay -
still one window, still the hue cube as the hero object.

### Added

- **Satellite ring**: 12 small cubes (`kSatelliteCount` in `Renderer.cpp`)
  orbit the hero in a static ring, cycling through all three
  `TextureKind`s (bindless, array, atlas) round-robin so all three
  residency paths are sampled in one frame regardless of the hero's own
  Residency picker. Not gameplay - population for the CPU frustum cull to
  reject something real instead of always seeing exactly one instance.
- **Camera dolly**: a debug-overlay "Camera distance" slider scales the
  camera along its fixed home direction (never becomes free-fly input).
  Dollying in shrinks how many world units the fixed FOV covers, visibly
  culling satellites - verified against the overlay's own instance/draw
  counts (13 written/13 drawn at rest, 13 written/6 drawn at dolly 1.2),
  not asserted in a comment.
- **Demo-texture eviction**: `Renderer::maybeEvictDemoTexture` frees the
  oldest-touched bindless demo texture once resident bytes exceed a
  small, deliberately artificial cap (2048 bytes - real device VRAM
  budgets are gigabytes, so gating on the real number would never fire in
  this contract test). The hero's current Bindless `TextureRef` is the
  only protected entry; the streamer's own white default (slot 0) was
  never eviction-eligible to begin with. The debug overlay shows both the
  real `VK_EXT_memory_budget` numbers (informational) and the demo cap's
  resident bytes/eviction count (what actually gates eviction).
- **Cull timing**: the debug overlay's "Cull: CPU (N ms)" line times the
  frustum-cull-and-compact loop with `SDL_GetPerformanceCounter`, around
  that loop only.

### Changed

- **Mesh pool is `DEVICE_LOCAL`**: `MeshPool`'s vertex/index buffers moved
  off host-visible mapped memory onto real device-local buffers, written
  through a per-call staging buffer and `vkCmdCopyBuffer`. `allocate()`
  never blocks the CPU: it signals the same shared upload timeline
  semaphore the texture uploads already use (now four values instead of
  three, the mesh pool's cube-mesh upload going first) instead of calling
  `vkQueueWaitIdle`. API shape (`allocate()` -> `MeshRange`, still no
  `free()`) is unchanged.
- **One sim-time clock, not two**: `Renderer` no longer runs its own
  `SDL_GetPerformanceCounter` clock for hue phase. `main()` owns one
  `simTimeSeconds` accumulator, gated by the same pause check that gates
  `shared::FixedClock`, pushed into `Renderer::setSimTime()` once per
  frame. Hue phase, satellite orbiting, and the cube's rotation now pause
  together because they read one clock, not because two independent
  clocks happened to agree.
- `recordComputePass` stays a documented no-op rather than a
  compute-shader compact: reading a GPU-written draw count back for
  `vkCmdDrawIndexedIndirect` without a CPU stall needs
  `vkCmdDrawIndexedIndirectCount`, which isn't in the device contract at
  any tier. Adding it now would be a device-capability migration for a
  compact loop the CPU already does in about 0.03ms at 13 instances.

### Fixed

- README's project tree was missing `src/frame/` (added in 0.3.0, never
  listed). Wiki Rendering still described the pre-0.3.0 push-constant
  texture-index scheme in one section after the rest of the page had
  already moved on.

## [0.3.0] - 2026-08-28

Corrects how this repo positions itself: Keel is a template for a custom
Vulkan game engine built on [simple-vk](https://github.com/nihalantiir/simple-vk)'s
boilerplate, not a successor that surpasses or replaces it, and the stock
cube executable is a contract test, not the product. That correction
lands first, in the README and wiki; the rest of this release is the
renderer growing the plumbing a real game built on this template would
need immediately - an explicit frame/camera layer, a GPU-driven scene
instead of one hardcoded draw, one handle across all three texture-
residency paths, and a fixed-timestep sim instead of a free-running
rotation.

### Changed

- **Positioning**: README and wiki rewritten so "built on simple-vk,
  template not successor, stock cube is a contract test" is the first
  thing a reader sees. Stack split into "inherited from simple-vk" vs
  "what Keel adds"; the project tree relabels `src/keel-vk` as the
  boilerplate layer, never "the engine". GitHub repo description updated
  to match.
- **Reverse-Z depth**: compare op `GREATER_OR_EQUAL`, depth clears to
  `0.0`, and `frame::Camera::projection()` builds an infinite-far
  projection matrix instead of `glm::perspective`'s finite far plane -
  more uniform depth precision across the visible range, no far-plane
  clip. See the wiki's Rendering page for the derivation.
- **Push constants shrink**: the model matrix and texture index moved out
  of the per-draw push constant block into per-instance GPU data (see
  below); push constants now carry only what's the same across an entire
  draw (camera view-proj, time, phase speed).
- CMake project version (and the window/console title both executables
  print) bumped to 0.3.0.

### Added

- **`frame::Camera`** (`src/frame/Camera.h`): view/proj as a function of
  plain position/front/up/lens data, not a hardcoded `lookAt`/`perspective`
  buried in the render loop. Carries a floating-origin offset, subtracted
  from both the camera's eye point and every world-space model matrix
  before either reaches the GPU - the scaffold a space sim or a large
  open world needs to keep GPU-side coordinates small regardless of how
  far from `(0,0,0)` the world actually extends.
- **An explicit frame pass list**: `Renderer::recordCommandBuffer` now
  calls out to five named steps in order (Acquire, World, Compute,
  Overlay, Present) instead of one long function body. Not a
  frame-graph - no automatic barrier derivation or resource-dependency
  tracking, just a readable, fixed sequence. Compute is a documented
  no-op today, reserved for GPU-driven culling work once there's more
  than one draw to cull.
- **A GPU scene**: `MeshPool` (one vertex buffer, one index buffer,
  subrange-allocated, capacity for far more than the one cube mesh it
  currently holds), a per-frame-in-flight instance buffer (capacity 256,
  only the cube populated), and a CPU frustum cull (`Frustum`, five
  Vulkan-clip-space half-planes, no far plane to match the infinite-far
  projection) that compacts surviving instances into that frame's
  indirect buffer before `vkCmdDrawIndexedIndirect`. The debug overlay
  reports instances written, draws issued, and triangles drawn.
- **Unified texture residency**: `renderer::TextureRef` is the one handle
  type a material or instance names a texture with, whether it actually
  lives in the bindless streaming array, the texture array, or the atlas
  page - the three storage classes themselves are unchanged. The debug
  overlay's new "Residency mode" control switches which path the cube
  samples live, so all three are visibly proven working, not just
  constructed and left unbound like before this release.
- **The cube's rotation moved onto `shared::FixedClock`**: previously
  present but unused, `FixedClock` now actually drives a fixed-timestep
  simulation of the cube's `Transform`, with a spiral-of-death cap on the
  accumulator and render-side interpolation between the last two
  simulated states so motion stays smooth at a frame rate that doesn't
  divide evenly into the fixed step. Pausing (Space) now freezes sim time
  directly; rendering keeps running either way.

### Fixed

- The three construction-time texture uploads (the bindless streamer's
  startup flush, the texture array, the atlas) no longer call
  `vkQueueWaitIdle`. A shared timeline semaphore retires all three on the
  GPU side, once, before the first frame that could sample any of them -
  the CPU never blocks on them at all now, at construction or otherwise.

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
