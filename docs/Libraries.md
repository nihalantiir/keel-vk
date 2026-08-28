# Libraries

## SDK-provided, same policy as simple-vk

SDL3, volk, VMA, and GLM are resolved through the installed Vulkan SDK
(`$VULKAN_SDK`): `find_package()` first, falling back to imported targets
built from the raw SDK directory layout if no CMake package is found.
Nothing here reaches for vcpkg or the network on a developer machine. CI is
the one exception: it uses vcpkg for these four because the unattended
Vulkan SDK installer doesn't reliably provide them as installable
components on hosted runners (see `.github/workflows/ci.yml`).

Versions confirmed current as of 2026-08-28 (not pinned in `CMakeLists.txt`
since they come from whatever SDK/vcpkg the machine has, same as
simple-vk):

- SDL3: `release-3.4.14`
- volk: `vulkan-sdk-1.4.357.0`
- VMA (`GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator`): `v3.4.0`
- GLM: `1.0.3`

## Dear ImGui: vendored, optional

FetchContent, pinned to commit `f1cc2ae15e53a861a874c3034aae6798fde194ab`
(tag `v1.92.9b`), gated entirely behind `KEEL_VK_IMGUI` (default `ON`).
`OFF` skips the fetch, needs no network at configure, and excludes
`src/debug/` from the build. Checkout lands under `external/`, not
committed (`.gitignore` excludes `external/*` except `external/README.md`).
Built with `IMGUI_IMPL_VULKAN_USE_VOLK` so the Vulkan backend calls go
through volk instead of linking the loader directly.

## Game UI: researched, not adopted this landing

ImGui in `src/debug/` is a debug overlay. It is not, and will not become,
the game UI layer - pulling debug tooling and shippable UI through the
same immediate-mode library tends to entangle debug-only assumptions
(always-on demo window, developer-grade defaults) into UI real players see.

For a future game UI module, in preference order:

1. **Clay** (`nicbarker/clay`) first - a single-header C layout library
   that emits layout commands for the caller to render, not a renderer
   itself. Fits keel-vk's "core doesn't own rendering decisions above the
   cube" posture: a Clay module would consume keel-vk's pipeline/buffer
   primitives the same way `src/renderer/` does, rather than bringing its
   own.
2. **Nuklear** (`Immediate-Mode-UI/Nuklear`) if Clay's layout-only scope
   turns out to be the blocker (e.g. immediate widget state is needed
   sooner than a custom render layer on top of Clay).
3. **RmlUi** (`mikke89/RmlUi`) only as an isolated sample, never the
   default. Its stock Vulkan backend creates its own instance, device, and
   swapchain - it does not integrate with an existing Vulkan context the
   way Clay or Nuklear would, so it cannot be the default UI path without
   either forking that backend or running a second, disconnected Vulkan
   context alongside keel-vk's.

None of the three are added this landing.

## Not in this landing

EnTT, ENet, miniaudio, cgltf, stb_image, nlohmann/json, doctest, Tracy,
any physics or scripting library. See `docs/Extending.md` for where ECS
(EnTT) and networking (ENet) are expected to land as separate modules.
