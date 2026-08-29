#pragma once

// Layer 0 umbrella: the generic Vulkan/SDL bootstrap, nothing else.
// Deliberately does not include renderer/Renderer.h - that pulls in the
// whole scene renderer (mesh pool, texture residency, instances), which
// a consumer only after the bootstrap (or writing their own renderer)
// shouldn't have to compile against. #include "renderer/Renderer.h"
// directly when you actually draw something - see the wiki's Extending
// page ("Populating a scene") and Architecture page ("The library
// boundary").
//
// Not a precompiled header and not meant to become one: this is three
// #include lines for convenience, nothing more. The headers it pulls in
// keep their own forward-declaration discipline unchanged - see each of
// them for what they still only forward-declare.

#include "keel-vk/Swapchain.h"
#include "keel-vk/VulkanContext.h"
#include "keel-vk/Window.h"

#include <keel/Version.h>
