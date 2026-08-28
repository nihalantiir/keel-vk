# Architecture

keel-vk absorbs simple-vk's bootstrap and extends it into a small proof
that the setup can carry a real 3D scene: a perspective, depth-tested,
continuously rotating cube, drawn via `vkCmdDrawIndexedIndirect`, with hue
that phases over time. It is still not a game engine.

## Layers

```
src/keel-vk/  ->  src/renderer/  ->  src/debug/
                        ^
src/client/   main, wires the above together
src/server/   headless stub, does not depend on keel-vk/renderer/debug at all
```

`src/keel-vk/` is the direct continuation of simple-vk's `src/core/`:
`Window`, `VulkanContext`, `Swapchain`, `ShaderModule`, `vkCheck`,
`setDebugObjectName`, the VMA implementation unit. It knows nothing about
what gets drawn. No gameplay types, no ECS, no net peers, no packs. See
`docs/Extending.md` for where those go instead.

`src/renderer/` owns the cube: pipeline, depth target, geometry buffers,
the indirect draw buffer, push constants, the rotation and hue-phase math.
It knows how to draw a frame; it doesn't know why.

`src/debug/` is the Dear ImGui overlay, gated entirely behind
`KEEL_VK_IMGUI`. It depends on `renderer::Renderer` directly (a forward
declaration in `Renderer.h`, the real include only in `Renderer.cpp`) so it
can read and write the renderer's live-editable state; that coupling is the
feature, not an accident.

`src/client/main.cpp` is the only place all of these are constructed
together, in order, on the stack. No smart pointers for engine objects:
lifetime is construction order, and teardown is the automatic reverse.

`src/server/` does not include or link anything above. It is a stub today
(prints a version string and exits) reserved for a future dedicated server
built on `keel-net`, once that exists, with zero Vulkan dependency.

## What's still not here

No camera controller (the cube's view is a fixed orbit point driven by
time, not input), no scene graph, no asset pipeline, no ECS, no
networking, no audio, no glTF loading. See `docs/Extending.md`.
