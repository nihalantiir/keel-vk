# Rendering

## Dynamic rendering, no render pass

Same as simple-vk: no `VkRenderPass`, no `VkFramebuffer`, anywhere.
`VkPipelineRenderingCreateInfo` is chained onto the graphics pipeline at
creation (declaring the color format and the `VK_FORMAT_D32_SFLOAT` depth
format), and `vkCmdBeginRendering`/`vkCmdEndRendering` bracket the draw each
frame. The pipeline is created once and never rebuilt across a resize; only
viewport, scissor, and the depth target's extent change.

Dear ImGui's pipeline (built inside `ImGui_ImplVulkan_Init`) must also
declare the depth format even though it never tests or writes depth:
Vulkan requires a bound pipeline's declared depth-attachment format to
match the depth attachment actually bound in `VkRenderingInfo` whenever one
is bound, regardless of whether that pipeline touches it. Skipping this
is a validation error (`VUID-vkCmdDrawIndexed-dynamicRenderingUnusedAttachments-08914`)
that only shows up with validation layers on, so it's called out here.

## Synchronization2

Image layout transitions use `vkCmdPipelineBarrier2` with
`VkImageMemoryBarrier2` (stage/access as `VkPipelineStageFlags2` /
`VkAccessFlags2`), and queue submission uses `vkQueueSubmit2` with
`VkSemaphoreSubmitInfo` / `VkCommandBufferSubmitInfo`, not the Vulkan 1.0
equivalents. Presentation still uses `vkQueuePresentKHR` with a binary
semaphore: sync2 doesn't touch the WSI present path.

Semaphore sizing is unchanged from simple-vk: `renderFinishedSemaphores_`
is sized per swapchain image, not per frame in flight, because a binary
semaphore can't be re-signaled while a previous signal is still
unconsumed, and present can outlast a frame.

## Depth

One `VK_FORMAT_D32_SFLOAT` image + view, allocated through VMA
(`VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`), recreated alongside the swapchain
on resize. Depth test and write are both on, compare op `LESS`. The depth
attachment's `loadOp` is `CLEAR` every frame, so the image is transitioned
from `VK_IMAGE_LAYOUT_UNDEFINED` every frame rather than tracking its
previous layout: since the clear discards prior contents anyway, this is
valid and simpler than state tracking.

## The cube

24 vertices (4 per face, not shared across faces) so every face gets a
flat, uniform hue with no interpolation seams at edges - the alternative
(8 shared corner vertices) would blend adjacent faces' hues across each
edge. 36 indices, one static vertex buffer and one static index buffer
(unlike simple-vk's triangle, the cube's geometry never changes after
upload, so there's no need for one buffer per frame in flight).

Each face carries a base hue offset 60 degrees apart (0, 60, 120, ...,
300). `cube.vert` computes `hue = baseHue + time * phaseSpeed` and converts
HSV to RGB per vertex; since a face's four vertices share the same base
hue, the whole face renders as one smoothly time-varying flat color.
`phaseSpeed` and the resulting `mvp` matrix arrive via push constants
(`mat4 mvp; float time; float phaseSpeed;`), not a UBO - the whole block is
well under the 128-byte minimum guaranteed push constant size.

The cube tumbles continuously around two axes (`elapsedTimeSeconds_ * 0.6`
around Y, `* 0.4` around X) with no input. `Renderer` tracks its own
elapsed time internally via `SDL_GetPerformanceCounter`, so
`Renderer::drawFrame()` keeps the exact same signature and call site as
simple-vk's (`drawFrame(&debugUi)` / `drawFrame()`).

### Winding and the projection Y-flip

Geometry is authored CCW as seen from outside each face - the ordinary
convention. `cube.vert`'s `mvp` includes a projection built with
`glm::perspective` (which assumes OpenGL's Y-up clip space) followed by
`proj[1][1] *= -1` to correct for Vulkan's Y-down clip space, instead of
reaching for a Vulkan-specific GLM build. That flip is a reflection
(determinant -1) applied identically to every vertex in the scene: it does
not change which of the cube's faces are near the camera versus far from
it, so it does not change which triangles should be culled. The pipeline's
`frontFace` is therefore left at `VK_FRONT_FACE_COUNTER_CLOCKWISE` to match
the geometry's actual winding, with `cullMode = VK_CULL_MODE_BACK_BIT`.
Setting `frontFace` to `CLOCKWISE` here (treating the flip as something
that needs compensating in the rasterizer state) culls the near faces
instead of the far ones - the cube renders with its far/inside walls
visible and its near walls missing, which looks like a solid shape from
some angles and inside-out from others as it rotates.

## Indirect draw

`vkCmdDrawIndexedIndirect` against a one-entry `VkDrawIndexedIndirectCommand`
buffer, even though there is exactly one draw. This is deliberate scaffold
for GPU-driven rendering later (batching many draws into one indirect
buffer); the record-time cost of the indirection is negligible for one
draw and the code shape doesn't need to change when a second draw is
added.

## Resize

`Swapchain::recreate()` is unchanged from simple-vk: `vkDeviceWaitIdle`,
destroy, recreate. `Renderer::recreateSyncObjectsForSwapchain()` additionally
destroys and recreates the depth target at the new extent. The pipeline
itself is never touched (see above). A `0x0` framebuffer (minimized window)
is caught in `main()` before `drawFrame()` is even called, same as
simple-vk.

## Debug overlay

Same call order as simple-vk: `debugUi.beginFrame()` before
`renderer.drawFrame(&debugUi)`, so ImGui's writes into
`Renderer::clearColor()` / `phaseSpeed()` land before that frame's push
constants and clear value are built. `DebugUi::render(cmd)` is called
inside the same dynamic rendering scope as the cube, after the cube's draw
call, so the overlay always draws on top.
