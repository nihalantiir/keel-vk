# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.9.0] - 2026-08-29

A consumer that `add_subdirectory()`s this repo gets `keel` and nothing
else, and supplies its own shaders. This is the close: past this
release, this repo only takes bugfixes.

### Added

- `KEEL_VK_BUILD_APPS` CMake option: gates building `keel-vk` and
  `keel-vk-server` (this repo's own contract-test executable and
  headless stub). Defaults ON only when this repo is the top-level
  project; OFF whenever it's `add_subdirectory()`'d, so a consumer
  linking `keel` no longer inherits this repo's own binaries unless it
  explicitly opts in.
- `renderer::PipelineSpec`: `Renderer`'s pipeline shader paths are now a
  required constructor argument instead of hardcoded to
  `shaders/cube.vert.spv`/`shaders/cube.frag.spv`. The pipeline's shape
  (vertex layout, descriptor sets, dynamic rendering formats) stays
  fixed; only which compiled shaders it loads is a consumer's own
  choice. This repo's own `ContractTest::pipelineSpec()` picks the cube
  shaders.

### Changed

- `keel_copy_runtime_assets(target)` no longer copies this repo's
  compiled shaders to every caller - only a consumer's own `packages/`.
  `keel-vk` copies its own cube shaders separately, next to itself only.
- `Renderer::registerDemoTexture()`/`registerDemoTextureCompressed()`
  renamed to `registerTexture()`/`registerTextureCompressed()` - "demo"
  never meant anything to a consumer.
- Wiki caught up across Home, Architecture, Rendering, Extending, and
  Troubleshooting: `KEEL_VK_BUILD_APPS`, the pluggable pipeline, and the
  rename are documented; stale claims that a consumer inherits this
  repo's executables or its compiled shaders are gone.

## [0.8.0] - 2026-08-29

A stranger can start a new repo on Keel in one sitting without editing
Renderer.cpp to delete the cube.

### Added

- `keel-vk-core` (layer 0) and `keel` (layer 1) are now separate CMake
  static libraries; `keel` links `keel-vk-core`, so linking `keel` is
  enough for both. A consumer gets there with
  `add_subdirectory(third_party/keel-vk)` + `target_link_libraries(mygame PRIVATE keel)`
  - verified against a real external project, not just built in place.
  `keel_copy_runtime_assets(target)` copies a consumer's own `packages/`
  and this repo's compiled shaders next to their executable.
- `renderer::Renderer` no longer constructs any mesh, texture, or
  instance content. `allocateMesh()`/`setMesh()`,
  `registerDemoTexture()`/`registerDemoTextureCompressed()`, and
  `setInstances()` are the extension points a caller uses instead. A
  bare `Renderer` with none of those called draws an empty scene: grey
  clear, 0 instances, no crash. The hero cube, satellite ring, and demo
  textures that used to live inside `Renderer` moved to
  `src/client/ContractTest.cpp` - this repo's own contract test, not
  something a fork inherits.
- `keel/Version.h` is generated from this project's own CMake version at
  configure time; the window title and server startup log read it
  instead of a hand-maintained literal, so they can't drift from what
  was actually built.
- `keel/keel.hpp` and `keel/foundation.hpp`: two thin umbrella headers
  (a handful of `#include` lines each, not a precompiled header) for the
  layer 0 bootstrap and the non-renderer parts of layer 1. Neither
  includes `renderer/Renderer.h` - include that directly when you draw.
- CI configures (not builds - no GPU on hosted runners) a small fixture
  that `add_subdirectory()`s the repo root and links `keel`, proving the
  target is actually discoverable from outside this repo, not just
  inside it.

### Changed

- `packages/base/package.json`'s `name` is now `keel-vk-template-base`,
  explicitly marked as template content a fork replaces.
- `Renderer::drawFrame()` now checks the framebuffer size itself and
  returns immediately on a `0x0` framebuffer (a minimized window),
  instead of relying on the caller to replicate that guard - a fork that
  doesn't copy `main()`'s own check no longer hits an invalid
  `VkViewport`.
- `Vfs`'s missing-`packages/` error and `ShaderModule`'s missing-shader
  error already fail loudly with a resolved path; both are now
  documented on the wiki's Troubleshooting page with the exact fix.

### Fixed

- `KEEL_VK_DEBUG` wasn't propagating to the `keel` target during the
  library split - only `keel-vk-core` had it, so validation-gated
  behavior in `Renderer.cpp`/`DebugUi.cpp` silently went dark unless a
  consumer also set it directly. Every `CMAKE_SOURCE_DIR`/`CMAKE_BINARY_DIR`
  in this repo's `CMakeLists.txt` is now `CMAKE_CURRENT_SOURCE_DIR`/
  `CMAKE_CURRENT_BINARY_DIR`, so `add_subdirectory` keeps this repo's own
  build artifacts (`external/`, shaders, PDBs, the exe) self-contained
  instead of landing in a consumer's build root. Both bugs would have
  hit a real fork in its first few minutes; caught by actually running
  the `add_subdirectory` scenario, not just building it in place.

## [0.7.0] - 2026-08-29

Cooked textures, real budget policy.

### Added

- `renderer::loadKtx2` reads a narrow subset of KTX2 (one level, no
  supercompression, no array layers, no cubemaps) - enough to back
  `packages/base/textures/demo_bc7.ktx2`, a BC7-compressed fixture baked
  by `tools/bake_ktx2_fixture.py`. `VK_FORMAT_BC7_UNORM_BLOCK` sampled is
  now part of the device contract. `TextureStreamer::allocateCompressed`
  shares `allocate()`'s slot/descriptor/staging machinery for a caller-
  specified format instead of assuming RGBA8; bindless sampling is
  format-agnostic, so the fixture drops into the existing demo-texture
  rotation with no shader change. The RGBA8 checker stays as the
  uncompressed, contract-test path.

### Changed

- `maybeEvictDemoTexture`'s 2048-byte artificial cap now only runs
  inside `KEEL_VK_DEBUG` - it models a developer's ability to trigger
  eviction on demand, not real memory pressure, so it no longer acts
  outside a Debug build. When `VK_EXT_memory_budget` is present, a
  second, real trigger checks actual device usage against actual budget
  (correct if a fork's content ever approaches it; never fires at this
  demo's scale). The overlay's VRAM line reads "N/A" instead of an
  estimated number when the extension is missing.

## [0.6.0] - 2026-08-28

Limits, queues, config, axes. No new rendering paths.

### Added

- `client::Config` gathers window size, present-mode preference, a
  packages-root override, and `--connect` into one struct. Load order:
  defaults, then an optional `keel.toml`/`keel.json` next to the
  executable, then CLI arguments. No parser dependency: two small
  hand-rolled readers, same tradeoff as `Vfs`'s existing `package.json`
  extractor.
- `client::Axes`: mouse-delta and WASD (as a -1..1 pair) axis bindings on
  `ActionMap`, alongside the existing digital actions. Pure data - no
  camera controller reads them yet; the debug overlay shows the raw
  values so the plumbing is visible without one.

### Changed

- The device pick now queries `VkPhysicalDeviceDescriptorIndexingProperties`
  and clamps the bindless array to what the device actually supports
  instead of assuming 256 is always safe. `bufferDeviceAddress` is
  enabled if present but no longer required - nothing in this codebase
  uses it.
- The dedicated transfer queue is gone. Every upload (textures, mesh
  data) goes through the graphics queue with `EXCLUSIVE` image sharing;
  the old `CONCURRENT` sharing mode masked a real synchronization gap
  that a dedicated queue would have needed release/acquire barriers to
  close correctly, for bandwidth that was never more than theoretical at
  this project's upload sizes.
- `keel::Vfs` throws on a missing packages root instead of silently
  mounting nothing - a typo'd override or an uncopied `packages/` now
  fails loudly at startup.
- `keel::Swapchain` takes a preferred present mode (falls back mailbox,
  then fifo) instead of hardcoding mailbox-or-fifo.

### Fixed

- Comments in `Renderer.cpp` referencing an internal "slice 2" milestone
  name, meaningless to a fresh reader, reworded to describe the current
  state directly. Wiki `Home`'s Layer 1 description was missing
  `src/frame/`. Wiki `Build` didn't say which CMake presets are actually
  ship-clean on MSVC (`release`/`ship`; `relwithdebinfo` keeps
  incremental linking on purpose).

## [0.5.0] - 2026-08-28

Hygiene release. No new systems, no architecture changes.

### Changed

- `.gitignore` gains MSVC/CMake build-byproduct patterns (`*.pdb`,
  `*.ilk`, `CMakeCache.txt`, `CMakeFiles/`, and similar) as defense in
  depth; `build/` already covered the normal case.
- PDB output directory set to `bin/`, next to the executables, instead of
  wherever MSVC would otherwise scatter them.
- `keel-vk` and `keel-vk-server` link with `/INCREMENTAL:NO` on Release,
  so a ship build doesn't emit a `.ilk` next to the shipped exe.
- `scripts/clean.*` also sweep stray `imgui.ini`/`pipeline_cache.bin`/
  `*.ilk`/`pdb/` left at repo root, the common case when the exe runs
  with the repo root as its working directory.
- Removed `src/samples/`, an empty directory holding only a placeholder
  README; the client executable has been the only sample since 0.1.0.

### Fixed

- Comments and wiki pages referencing "this landing" (a specific past
  development session, meaningless to a reader who wasn't in it) rewritten
  in plain, session-independent language. Two of them were also flatly
  wrong by the time they were found: a `Renderer.h` comment claimed only
  one instance slot is ever populated (13 are, since the 0.4.0 satellite
  ring), and a shader comment claimed a single instance made
  `nonuniformEXT` optional (it's required, since instances now sample
  different bindless slots).
- Wiki `Shaders.md` still described the pre-0.3.0 push-constant scheme
  with no texture sampling mentioned at all. `Packages.md` pointed at a
  `Renderer::createTexture()` that doesn't exist. `shaders/README.md`'s
  build output path predated CMake presets.

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
