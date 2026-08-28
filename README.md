# Keel

<p align="center">
  <img src=".github/banner.svg" alt="keel-vk, a Vulkan 1.3 engine foundation template in C++20" width="100%">
</p>

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/nihalantiir/keel-vk)](https://github.com/nihalantiir/keel-vk/releases)
[![CI](https://github.com/nihalantiir/keel-vk/actions/workflows/ci.yml/badge.svg)](https://github.com/nihalantiir/keel-vk/actions/workflows/ci.yml)
[![Platforms](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-informational)](#prerequisites)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](#stack)
[![Vulkan 1.3](https://img.shields.io/badge/Vulkan-1.3-red)](#stack)

Built on [simple-vk](https://github.com/nihalantiir/simple-vk)'s Vulkan
1.3 + SDL3 boilerplate (window, instance/device, swapchain, dynamic
rendering, optional Dear ImGui). Keel is not a successor that surpasses
simple-vk and doesn't compete with it: if you want a triangle and a clean
device, use simple-vk. Keel is a template for a *custom* Vulkan game
engine - the expensive hardware answers (device contract, bindless/array/
atlas texture residency, an ECS/net/VFS foundation) already chosen, so a
voxel engine, a space sim, or a 3D RPG can start in *another* repo without
re-deciding them.

The executable this repo builds is a contract test, not the product: a
grey-clear, depth-tested, hue-phased cube with an optional debug overlay.
If the cube still runs after a change here, the contract held.

## Stack

Inherited from simple-vk / the Vulkan SDK:

- **C++20**, CMake >= 3.24, Ninja
- **SDL3**, windowing, input, surface creation
- **Volk**, Vulkan function loading
- **VMA** (Vulkan Memory Allocator), GPU memory allocation
- **GLM**, math
- **Dynamic rendering** - no `VkRenderPass`/`VkFramebuffer`
- **Dear ImGui**, optional debug overlay (vendored, see [Libraries](https://github.com/nihalantiir/keel-vk/wiki/Libraries))

What Keel adds on top (the foundation, see [Architecture](https://github.com/nihalantiir/keel-vk/wiki/Architecture)):

- A stricter Vulkan 1.3 device contract (descriptor indexing, timeline
  semaphores, buffer device address, and more - see
  [Device contract](https://github.com/nihalantiir/keel-vk/wiki/Device-contract))
- Bindless + texture-array + atlas residency
- `keel::World` (EnTT), `keel::Vfs`, `shared::FixedClock`, `net::Host` (ENet)

SDL3/volk/VMA/GLM ship inside the Vulkan SDK, so there's nothing to fetch
or vendor for those four.

## Prerequisites

- [Vulkan SDK](https://vulkan.lunarg.com/) installed, with `VULKAN_SDK` set
- CMake >= 3.24 and [Ninja](https://ninja-build.org/)
- A C++20 compiler (MSVC, Clang, or GCC)
- Internet access on first configure (Dear ImGui is fetched via CMake,
  unless `KEEL_VK_IMGUI=OFF`)

## Building

```
./scripts/build.ps1 [Debug|Release|RelWithDebInfo]   # Windows
./scripts/build.sh [Debug|Release|RelWithDebInfo]    # Linux
```

Or with [CMake presets](CMakePresets.json): `cmake --preset debug && cmake --build --preset debug`.
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
├── external/             vendored deps (Dear ImGui, EnTT, ENet, fetched by CMake)
├── packages/             content packs, mounted by keel::Vfs; packages/base/ ships with the template
├── scripts/              build/clean helpers for both platforms
├── shaders/              GLSL sources, compiled to SPIR-V at build time
├── .github/workflows/    CI (configure + build, both ImGui on and off)
└── src/
    ├── keel-vk/          boilerplate layer: simple-vk's bootstrap, absorbed and tightened
    ├── renderer/         foundation: pipeline, depth, bindless/array/atlas textures, indirect draw
    ├── debug/            Dear ImGui overlay, gated by KEEL_VK_IMGUI (debug only, never the product UI)
    ├── shared/           foundation: keel::World (EnTT), keel::Vfs, shared::FixedClock
    ├── net/              foundation: net::Host, a transport-only ENet wrap
    ├── client/           contract-test composition: the cube executable (keel-vk)
    ├── server/           headless transport stub, no Vulkan/SDL dependency
    └── samples/          reserved for future standalone samples
```

`src/keel-vk/` is boilerplate, not "the engine": it stays generic and
never learns about meshes, instances, packs, ENet, or EnTT. `src/renderer/`,
`src/shared/`, and `src/net/` are the foundation layer this template adds.
A game built on Keel lives in its *own* repo, not inside this one. See
[Architecture](https://github.com/nihalantiir/keel-vk/wiki/Architecture).

## Documentation

Deeper docs live on the [wiki](https://github.com/nihalantiir/keel-vk/wiki).
For the boilerplate layer itself, see
[simple-vk's wiki](https://github.com/nihalantiir/simple-vk/wiki).

- [Home](https://github.com/nihalantiir/keel-vk/wiki)
- [Build](https://github.com/nihalantiir/keel-vk/wiki/Build)
- [Architecture](https://github.com/nihalantiir/keel-vk/wiki/Architecture)
- [Vulkan bootstrap](https://github.com/nihalantiir/keel-vk/wiki/Vulkan-bootstrap)
- [Device contract](https://github.com/nihalantiir/keel-vk/wiki/Device-contract)
- [Rendering](https://github.com/nihalantiir/keel-vk/wiki/Rendering)
- [Shaders](https://github.com/nihalantiir/keel-vk/wiki/Shaders)
- [Libraries](https://github.com/nihalantiir/keel-vk/wiki/Libraries)
- [Extending](https://github.com/nihalantiir/keel-vk/wiki/Extending)
- [Coding conventions](https://github.com/nihalantiir/keel-vk/wiki/Coding-conventions)
- [Troubleshooting](https://github.com/nihalantiir/keel-vk/wiki/Troubleshooting)

## License

MIT, see [LICENSE](LICENSE).
