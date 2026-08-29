#pragma once

// Layer 1 umbrella: the ECS/net/VFS/config/input wraps, plus Camera and
// the one handle type a material or instance names a texture with -
// everything in "the foundation" except the renderer itself. Deliberately
// does not include renderer/Renderer.h - see keel.hpp's comment for why
// that stays a separate, explicit include.
//
// Not a precompiled header and not meant to become one: this is a list
// of #include lines for convenience, nothing more.

#include "client/ActionMap.h"
#include "client/Config.h"
#include "frame/Camera.h"
#include "net/Host.h"
#include "renderer/TextureRef.h"
#include "shared/Clock.h"
#include "shared/Vfs.h"
#include "shared/World.h"
