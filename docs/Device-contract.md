# Device contract

keel-vk targets exactly one Vulkan 1.3 device contract. There is no
fallback path for older hardware: `VulkanContext::pickPhysicalDevice()`
rejects any device that doesn't satisfy all of the following, and if no
device satisfies it, the constructor throws listing, per device, which
requirements it was missing.

## Required

Instance API version: `VK_API_VERSION_1_3`.

Vulkan 1.3 core (`VkPhysicalDeviceVulkan13Features`):
- `dynamicRendering`
- `synchronization2`
- `maintenance4`

Vulkan 1.2 core (`VkPhysicalDeviceVulkan12Features`):
- `timelineSemaphore`
- `bufferDeviceAddress`
- `descriptorIndexing`
- `runtimeDescriptorArray`
- `descriptorBindingPartiallyBound`
- `descriptorBindingVariableDescriptorCount`
- `descriptorBindingSampledImageUpdateAfterBind`
- `descriptorBindingStorageBufferUpdateAfterBind`
- `scalarBlockLayout`

Vulkan 1.1 core (`VkPhysicalDeviceVulkan11Features`):
- `shaderDrawParameters` (promoted to core in 1.1, not 1.2, despite sitting
  next to the descriptor-indexing family above)

Vulkan 1.0 base (`VkPhysicalDeviceFeatures`):
- `multiDrawIndirect`
- `drawIndirectFirstInstance`

Device extensions:
- `VK_KHR_swapchain`

Plus a complete graphics+present queue family pair and a surface with at
least one format and present mode.

None of this is used to its full extent by the cube in this landing (no
descriptor arrays, no manual buffer addresses yet). It is required now so
later systems (bindless textures, GPU-driven culling, async transfer) don't
need a device-capability migration later.

## Enabled if present, never required

Probed via `vkEnumerateDeviceExtensionProperties` /
`vkGetPhysicalDeviceFeatures2` after a device is picked; enabled on the
logical device when found, otherwise silently skipped:

- `VK_EXT_memory_budget` (also turns on `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT`)
- `VK_KHR_calibrated_timestamps`, falling back to `VK_EXT_calibrated_timestamps`
  if only the older name is advertised
- `VK_EXT_extended_dynamic_state2`
- `VK_EXT_extended_dynamic_state3`
- `hostQueryReset` (`VkPhysicalDeviceVulkan12Features.hostQueryReset`) -
  reserved for GPU timing queries, not used yet

None of these are wired into any pipeline state or command this landing;
they're enabled so a later system doesn't have to touch device creation
again to reach for them.

## Debug-only

Validation layers (`VK_LAYER_KHRONOS_validation`) and
`VK_EXT_debug_utils` (instance extension, object naming) are enabled only
when `KEEL_VK_DEBUG` is defined, which the CMake build sets for the Debug
configuration only. See `docs/Coding-conventions.md`.

## Picking a device

Among devices that satisfy the full required list, `ratePhysicalDevice()`
scores discrete GPUs above integrated, then by `maxImageDimension2D`, and
picks the highest score. This is unchanged from simple-vk.
