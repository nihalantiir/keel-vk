cmake_minimum_required(VERSION 3.24)
project(keel-vk VERSION 0.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KEEL_VK_IMGUI "Build the Dear ImGui debug overlay (fetches Dear ImGui via CMake FetchContent)" ON)

# Default ON only when this is the top-level project (building this repo
# directly); default OFF when add_subdirectory()'d - a consumer linking
# keel must not inherit keel-vk/keel-vk-server unless they opt in. See
# the wiki's Extending page.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(KEEL_VK_BUILD_APPS_DEFAULT ON)
else()
  set(KEEL_VK_BUILD_APPS_DEFAULT OFF)
endif()
option(KEEL_VK_BUILD_APPS "Build keel-vk and keel-vk-server (this repo's own contract-test exe and headless stub)"
  ${KEEL_VK_BUILD_APPS_DEFAULT})

# Keep MSVC's .pdb next to the executables instead of scattered in build/,
# same directory RUNTIME_OUTPUT_DIRECTORY already uses below.
# CMAKE_CURRENT_BINARY_DIR everywhere in this file, not CMAKE_BINARY_DIR:
# when a consumer add_subdirectory()s this repo, CMAKE_BINARY_DIR is
# THEIR top-level build root, not this project's own build directory -
# see the wiki's Extending page.
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(NOT DEFINED ENV{VULKAN_SDK})
  message(FATAL_ERROR "VULKAN_SDK environment variable is not set. Install the "
                       "Vulkan SDK (the installer sets this for you) and "
                       "reopen your shell before configuring.")
endif()
set(VULKAN_SDK_ROOT "$ENV{VULKAN_SDK}")

# find_package() is tried first for everything; anything not found that way
# (the SDK ships volk/VMA/glm as raw headers/libs, not CMake packages, on
# some platforms) falls back to imported targets built from the raw SDK
# layout below. CI overrides CMAKE_PREFIX_PATH with a vcpkg prefix instead
# of changing any of this.
list(APPEND CMAKE_PREFIX_PATH
  "${VULKAN_SDK_ROOT}"
  "${VULKAN_SDK_ROOT}/cmake"
  "${VULKAN_SDK_ROOT}/lib/cmake"
)

find_package(Vulkan REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(volk CONFIG QUIET)
find_package(VulkanMemoryAllocator CONFIG QUIET)
find_package(glm CONFIG QUIET)

# --- volk ---
if(NOT TARGET volk::volk)
  find_path(VOLK_INCLUDE_DIR NAMES volk.h
    PATHS "${VULKAN_SDK_ROOT}/Include/Volk" "${VULKAN_SDK_ROOT}/include/volk" "${VULKAN_SDK_ROOT}/include/Volk"
    NO_DEFAULT_PATH)
  find_library(VOLK_LIBRARY_RELEASE NAMES volk
    PATHS "${VULKAN_SDK_ROOT}/Lib" "${VULKAN_SDK_ROOT}/lib" NO_DEFAULT_PATH)
  find_library(VOLK_LIBRARY_DEBUG NAMES volkd
    PATHS "${VULKAN_SDK_ROOT}/Lib" "${VULKAN_SDK_ROOT}/lib" NO_DEFAULT_PATH)

  if(NOT VOLK_INCLUDE_DIR OR NOT VOLK_LIBRARY_RELEASE)
    message(FATAL_ERROR "Could not find volk under ${VULKAN_SDK_ROOT}. "
                         "Expected volk.h plus a prebuilt volk/volkd library.")
  endif()

  add_library(volk::volk STATIC IMPORTED)
  set_target_properties(volk::volk PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${VOLK_INCLUDE_DIR}"
    IMPORTED_LOCATION "${VOLK_LIBRARY_RELEASE}"
  )
  if(VOLK_LIBRARY_DEBUG)
    set_target_properties(volk::volk PROPERTIES IMPORTED_LOCATION_DEBUG "${VOLK_LIBRARY_DEBUG}")
  endif()
endif()

# --- Vulkan Memory Allocator (header-only) ---
if(NOT TARGET GPUOpen::VulkanMemoryAllocator)
  find_path(VMA_INCLUDE_DIR NAMES vk_mem_alloc.h
    PATHS "${VULKAN_SDK_ROOT}/Include/vma" "${VULKAN_SDK_ROOT}/include/vma"
    NO_DEFAULT_PATH)
  if(NOT VMA_INCLUDE_DIR)
    message(FATAL_ERROR "Could not find vk_mem_alloc.h under ${VULKAN_SDK_ROOT}")
  endif()

  add_library(VulkanMemoryAllocator INTERFACE)
  target_include_directories(VulkanMemoryAllocator INTERFACE "${VMA_INCLUDE_DIR}")
  add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
endif()

# --- GLM (header-only) ---
if(NOT TARGET glm::glm)
  find_path(GLM_INCLUDE_DIR NAMES glm/glm.hpp
    PATHS "${VULKAN_SDK_ROOT}/Include" "${VULKAN_SDK_ROOT}/include"
    NO_DEFAULT_PATH)
  if(NOT GLM_INCLUDE_DIR)
    message(FATAL_ERROR "Could not find glm/glm.hpp under ${VULKAN_SDK_ROOT}")
  endif()

  add_library(glm INTERFACE)
  target_include_directories(glm INTERFACE "${GLM_INCLUDE_DIR}")
  add_library(glm::glm ALIAS glm)
endif()

include(FetchContent)
# CMAKE_CURRENT_SOURCE_DIR, not CMAKE_SOURCE_DIR: this file is meant to be
# add_subdirectory()'d by a consumer (see the wiki's Extending page), and
# CMAKE_SOURCE_DIR would then resolve to the CONSUMER's top-level project
# directory, not this one - external/ needs to land inside this repo
# regardless of who includes it.
set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external" CACHE PATH "" FORCE)

# --- EnTT (header-only, vendored via FetchContent into external/, pinned tag) ---
# Always fetched: keel::World (src/shared/World.h) wraps it and the client
# always uses it, unlike Dear ImGui which is optional.
FetchContent_Declare(
  entt
  GIT_REPOSITORY https://github.com/skypjack/entt.git
  GIT_TAG v4.0.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(entt) # provides the EnTT::EnTT target itself

# --- ENet (vendored via FetchContent into external/, pinned tag) ---
# Always fetched, transport only (src/net/Host.h): keel-vk-server always
# links it, and the client links it too but stays idle unless --connect
# is passed (see src/client/main.cpp).
FetchContent_Declare(
  enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG v1.3.18
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(enet) # provides the "enet" target itself
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
if(WIN32)
  # ENet's own CMakeLists doesn't link these; without them every consumer
  # fails at link time with unresolved Winsock/timer symbols.
  target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()

# --- Dear ImGui (vendored via FetchContent into external/, pinned commit) ---
# Gated behind KEEL_VK_IMGUI: OFF skips the fetch entirely, so configure
# needs no network access and src/debug/ isn't compiled.
if(KEEL_VK_IMGUI)
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG f1cc2ae15e53a861a874c3034aae6798fde194ab # v1.92.9b
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(imgui)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  )
  target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  target_compile_definitions(imgui PUBLIC
    VK_NO_PROTOTYPES
    IMGUI_IMPL_VULKAN_USE_VOLK # backend loads its Vulkan calls through volk instead of linking the loader directly
  )
  target_link_libraries(imgui PUBLIC volk::volk SDL3::SDL3)
endif()

# --- Generated keel/Version.h: this project()'s own version, as
# compile-time constants, so a window title or overlay string can never
# drift from what CMake itself thinks this is being built as. ---
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/keel-vk/Version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/keel/Version.h"
  @ONLY
)

# --- keel-vk-core: layer 0, simple-vk's bootstrap absorbed and tightened.
# Generic Vulkan/SDL setup only - window, instance/device, swapchain,
# shader loading, the VMA implementation unit. Knows nothing about what
# gets drawn. See the wiki's Architecture page. ---
add_library(keel-vk-core STATIC
  src/keel-vk/Window.cpp
  src/keel-vk/VulkanContext.cpp
  src/keel-vk/Swapchain.cpp
  src/keel-vk/ShaderModule.cpp
  src/keel-vk/VmaImpl.cpp
)

# PUBLIC: every one of these defines changes how a header a consumer
# includes (Renderer.h pulls in vk_mem_alloc.h directly, volk.h needs
# VK_NO_PROTOTYPES seen consistently everywhere it's included, and on
# Windows any translation unit reaching windows.h transitively needs
# NOMINMAX or std::max/min break) - a consumer linking keel must see the
# same defines this library was built with, not silently different ones.
target_compile_definitions(keel-vk-core PUBLIC
  VK_NO_PROTOTYPES               # all Vulkan entry points come from volk
  VMA_STATIC_VULKAN_FUNCTIONS=0
  VMA_DYNAMIC_VULKAN_FUNCTIONS=1
  $<$<PLATFORM_ID:Windows>:NOMINMAX>
  $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
)
# PRIVATE: only gates behavior inside this library's own .cpp files
# (validation layers, debug messenger); nothing a consumer's headers read.
target_compile_definitions(keel-vk-core PRIVATE
  $<$<CONFIG:Debug>:KEEL_VK_DEBUG>
)

target_compile_options(keel-vk-core PUBLIC
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

# PUBLIC include dirs: a consumer includes headers as "keel-vk/Window.h",
# "renderer/Renderer.h", etc., same paths this repo's own sources use,
# plus the generated "keel/Version.h" above.
target_include_directories(keel-vk-core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(keel-vk-core PUBLIC
  volk::volk
  GPUOpen::VulkanMemoryAllocator
  SDL3::SDL3
)

# --- keel: layer 1, the foundation this repo adds on top of the
# bootstrap - device contract, texture residency, ECS/net/VFS wraps, the
# optional debug overlay. This is what a fork links. src/client/main.cpp
# is deliberately NOT in this library: it's this repo's own cube
# composition, not something a consumer inherits. ---
set(KEEL_LIB_SOURCES
  src/renderer/Renderer.cpp
  src/renderer/MeshPool.cpp
  src/renderer/Ktx2.cpp
  src/renderer/TextureStreamer.cpp
  src/renderer/TextureArray2D.cpp
  src/renderer/Atlas2D.cpp
  src/shared/Vfs.cpp
  src/net/Host.cpp
  src/client/Config.cpp
  src/client/ActionMap.cpp
)
if(KEEL_VK_IMGUI)
  list(APPEND KEEL_LIB_SOURCES src/debug/DebugUi.cpp)
endif()

add_library(keel STATIC ${KEEL_LIB_SOURCES})

target_compile_definitions(keel PUBLIC
  KEEL_VK_IMGUI=$<BOOL:${KEEL_VK_IMGUI}>
)
# PRIVATE, same as keel-vk-core's own KEEL_VK_DEBUG definition: gates
# behavior inside this library's own .cpp files (Renderer.cpp,
# DebugUi.cpp), nothing a consumer's headers read. Linking keel-vk-core
# does not propagate its PRIVATE compile definitions here - each target
# that checks KEEL_VK_DEBUG needs its own copy of this line.
target_compile_definitions(keel PRIVATE
  $<$<CONFIG:Debug>:KEEL_VK_DEBUG>
)

target_link_libraries(keel PUBLIC
  keel-vk-core
  glm::glm
  EnTT::EnTT
)
target_link_libraries(keel PRIVATE enet) # net::Host.h only forward-declares ENet types, so this stays private
if(KEEL_VK_IMGUI)
  target_link_libraries(keel PUBLIC imgui) # DebugUi.h's own includes need imgui.h visible to consumers
endif()

# --- keel-vk and keel-vk-server: this repo's own contract-test
# executable and headless stub. Gated behind KEEL_VK_BUILD_APPS: a
# consumer that add_subdirectory()s this repo and links keel does not
# get these unless they explicitly opt in - see the wiki's Extending
# page. Everything about the hero cube, the satellite ring, and the demo
# textures lives in keel-vk's own sources (main.cpp, ContractTest.cpp),
# never in keel itself. ---
if(KEEL_VK_BUILD_APPS)
  add_executable(keel-vk src/client/main.cpp src/client/ContractTest.cpp)
  target_link_libraries(keel-vk PRIVATE keel)

  # /INCREMENTAL:NO on Release so MSVC doesn't emit a .ilk next to the
  # shipped exe; incremental linking is a debug-loop convenience only.
  target_link_options(keel-vk PRIVATE
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/INCREMENTAL:NO>
  )

  set_target_properties(keel-vk PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
  )

  # Compiles Host.cpp directly rather than linking keel, which would
  # pull in Vulkan/SDL/glm/EnTT for a program that needs none of it.
  add_executable(keel-vk-server src/server/main.cpp src/net/Host.cpp)
  target_include_directories(keel-vk-server PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(keel-vk-server PRIVATE enet)
  set_target_properties(keel-vk-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
  )
  target_link_options(keel-vk-server PRIVATE
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/INCREMENTAL:NO>
  )
endif()

# --- keel_copy_runtime_assets(target): POST_BUILD copy of <caller>/packages
# next to target's built executable, the same way this repo's own
# packages/ is copied next to keel-vk below. A consumer that
# add_subdirectory()s this repo and links keel calls this with their own
# target name to get the same treatment for their own packages/ - it
# reads CMAKE_CURRENT_SOURCE_DIR at the call site, not this listfile's,
# so it resolves the caller's own packages/ directory, not this repo's.
#
# TEMPORARY: also copies this repo's own compiled cube.vert.spv/cube.frag.spv
# next to target. This exists only because renderer::Renderer::createPipeline()
# still hardcodes those two files as of this commit - once Renderer takes
# its shader paths as a constructor argument instead (see the wiki's
# Extending page, "Pipeline is data"), a consumer supplies its own
# shaders and this copy goes away. Sourced from KEEL_VK_SHADER_OUTPUT_DIR
# (a CACHE variable, set below, so it's visible here regardless of which
# directory scope calls this function). ---
function(keel_copy_runtime_assets target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/packages"
      "$<TARGET_FILE_DIR:${target}>/packages"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${KEEL_VK_SHADER_OUTPUT_DIR}"
      "$<TARGET_FILE_DIR:${target}>/shaders"
    COMMENT "Copying packages/ and shaders/ next to the executable"
  )
  add_dependencies(${target} shaders)
endfunction()

# --- Shader compilation (GLSL -> SPIR-V) ---
if(TARGET Vulkan::glslc)
  set(GLSLC_COMMAND Vulkan::glslc)
else()
  find_program(GLSLC_EXECUTABLE NAMES glslc
    PATHS "${VULKAN_SDK_ROOT}/Bin" "${VULKAN_SDK_ROOT}/bin" NO_DEFAULT_PATH)
  if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "Could not find glslc under ${VULKAN_SDK_ROOT}")
  endif()
  set(GLSLC_COMMAND "${GLSLC_EXECUTABLE}")
endif()

set(SHADER_SOURCES
  shaders/cube.vert
  shaders/cube.frag
)
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin/shaders")
# CACHE INTERNAL, not a plain variable: keel_copy_runtime_assets() above
# may be called from a consumer's own directory scope (after
# add_subdirectory), where a plain variable set here wouldn't be visible.
set(KEEL_VK_SHADER_OUTPUT_DIR "${SHADER_OUTPUT_DIR}" CACHE INTERNAL "")

set(SHADER_SPV_FILES "")
foreach(shader ${SHADER_SOURCES})
  get_filename_component(shader_name "${shader}" NAME)
  set(shader_output "${SHADER_OUTPUT_DIR}/${shader_name}.spv")
  add_custom_command(
    OUTPUT "${shader_output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
    # --target-env=vulkan1.3 so GL_EXT_nonuniform_qualifier and the runtime
    # descriptor array in cube.frag get SPIR-V that actually matches the
    # device contract (descriptor indexing), not the vulkan1.0 default.
    COMMAND ${GLSLC_COMMAND} --target-env=vulkan1.3 "${CMAKE_CURRENT_SOURCE_DIR}/${shader}" -o "${shader_output}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${shader}"
    COMMENT "Compiling shader ${shader}"
    VERBATIM
  )
  list(APPEND SHADER_SPV_FILES "${shader_output}")
endforeach()

add_custom_target(shaders DEPENDS ${SHADER_SPV_FILES})

if(KEEL_VK_BUILD_APPS)
  # --- Copy packages/ and shaders/ next to the executable: keel::Vfs and
  # keel::ShaderModule both mount/load relative to SDL_GetBasePath(), the
  # executable's own directory. Also adds keel-vk's dependency on the
  # shaders custom target. ---
  keel_copy_runtime_assets(keel-vk)

  # --- Windows: copy SDL3.dll next to the built executable ---
  if(WIN32)
    add_custom_command(TARGET keel-vk POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>"
        "$<TARGET_FILE_DIR:keel-vk>"
      COMMENT "Copying SDL3.dll next to the executable"
    )
  endif()
endif()
