# keel-vk

A C++20 Vulkan 1.3 engine scaffold. The successor to
[simple-vk](https://github.com/nihalantiir/simple-vk): the same bootstrap
(window, instance/device, swapchain, dynamic rendering, Dear ImGui debug
overlay), evolved into a perfected core plus the smallest 3D proof that the
setup is real, so games and later systems (ECS, networking, content packs,
game UI) can build on top of it.

## Stack

- **C++20**
- **SDL3**, windowing, input, surface creation
- **Volk**, Vulkan function loading
- **VMA** (Vulkan Memory Allocator), GPU memory allocation
- **GLM**, math
- **Dear ImGui**, debug overlay (vendored, see `docs/Libraries.md`)

SDL3/volk/VMA/GLM ship inside the Vulkan SDK, so there's nothing to fetch
or vendor for those four.

## Prerequisites

- Vulkan SDK installed, with `VULKAN_SDK` set
- CMake >= 3.24 and Ninja
- A C++20 compiler (MSVC, Clang, or GCC)
- Internet access on first configure (Dear ImGui is fetched via CMake,
  unless `KEEL_VK_IMGUI=OFF`)

## Building

```
./scripts/build.ps1 [Debug|Release|RelWithDebInfo]   # Windows
./scripts/build.sh [Debug|Release|RelWithDebInfo]    # Linux
```

Or with CMake presets: `cmake --preset debug && cmake --build --preset debug`.
The executables, `SDL3.dll` (Windows), and compiled shaders all land in
`build/<preset>/bin/`.

### Building without Dear ImGui

`KEEL_VK_IMGUI` CMake option (default `ON`). `OFF` skips fetching Dear
ImGui, no network needed at configure, `src/debug/` isn't compiled:

```
cmake --preset debug -DKEEL_VK_IMGUI=OFF
cmake --build --preset debug
```

The cube still renders, just without the overlay.

### Shipping a build

```
cmake --preset ship
cmake --build --preset ship
```

Equivalent to `-DCMAKE_BUILD_TYPE=Release -DKEEL_VK_IMGUI=OFF`.

## Cleaning

```
./scripts/clean.ps1   # Windows
./scripts/clean.sh    # Linux
```

## Project structure

```
keel-vk/
├── CMakeLists.txt
├── CMakePresets.json
├── CHANGELOG.md
├── docs/                 architecture, device contract, rendering, conventions
├── external/             vendored deps (Dear ImGui, fetched by CMake)
├── packages/             reserved for a future content-pack format
├── scripts/              build/clean helpers for both platforms
├── shaders/              GLSL sources, compiled to SPIR-V at build time
├── .github/workflows/    CI (configure + build, both ImGui on and off)
└── src/
    ├── keel-vk/          Vulkan + SDL bootstrap: Window, VulkanContext, Swapchain, ShaderModule, ...
    ├── renderer/         the cube: pipeline, depth, indirect draw, rotation, color phase
    ├── debug/            Dear ImGui overlay, gated by KEEL_VK_IMGUI
    ├── client/           the cube executable (keel-vk)
    ├── server/           headless stub, no Vulkan dependency, reserved for keel-net
    └── samples/          reserved for future standalone samples
```

`src/keel-vk/` stays generic, `src/renderer/` builds on it to draw,
`src/debug/` overlays introspection on top. See `docs/Architecture.md`.

## Documentation

- `docs/Architecture.md`
- `docs/Device-contract.md`
- `docs/Rendering.md`
- `docs/Extending.md`
- `docs/Libraries.md`
- `docs/Coding-conventions.md`

## License

MIT, see LICENSE.
