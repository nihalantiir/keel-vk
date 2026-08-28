cmake_minimum_required(VERSION 3.24)
project(keel-vk VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KEEL_VK_IMGUI "Build the Dear ImGui debug overlay (fetches Dear ImGui via CMake FetchContent)" ON)

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
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/external" CACHE PATH "" FORCE)

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

set(KEEL_VK_CLIENT_SOURCES
  src/client/main.cpp
  src/client/ActionMap.cpp
  src/keel-vk/Window.cpp
  src/keel-vk/VulkanContext.cpp
  src/keel-vk/Swapchain.cpp
  src/keel-vk/ShaderModule.cpp
  src/keel-vk/VmaImpl.cpp
  src/renderer/Renderer.cpp
  src/renderer/MeshPool.cpp
  src/renderer/TextureStreamer.cpp
  src/renderer/TextureArray2D.cpp
  src/renderer/Atlas2D.cpp
  src/net/Host.cpp
  src/shared/Vfs.cpp
)
if(KEEL_VK_IMGUI)
  list(APPEND KEEL_VK_CLIENT_SOURCES src/debug/DebugUi.cpp)
endif()

add_executable(keel-vk ${KEEL_VK_CLIENT_SOURCES})

target_include_directories(keel-vk PRIVATE src)

target_compile_definitions(keel-vk PRIVATE
  VK_NO_PROTOTYPES               # all Vulkan entry points come from volk
  VMA_STATIC_VULKAN_FUNCTIONS=0
  VMA_DYNAMIC_VULKAN_FUNCTIONS=1
  KEEL_VK_IMGUI=$<BOOL:${KEEL_VK_IMGUI}>
  $<$<CONFIG:Debug>:KEEL_VK_DEBUG>    # enables validation layers + debug messenger
  $<$<PLATFORM_ID:Windows>:NOMINMAX>
  $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
)

target_compile_options(keel-vk PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

target_link_libraries(keel-vk PRIVATE
  volk::volk
  GPUOpen::VulkanMemoryAllocator
  SDL3::SDL3
  glm::glm
  EnTT::EnTT
  enet
)
if(KEEL_VK_IMGUI)
  target_link_libraries(keel-vk PRIVATE imgui)
endif()

set_target_properties(keel-vk PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# --- keel-vk-server: headless, no Vulkan/SDL, only the ENet transport ---
add_executable(keel-vk-server src/server/main.cpp src/net/Host.cpp)
target_include_directories(keel-vk-server PRIVATE src)
target_link_libraries(keel-vk-server PRIVATE enet)
set_target_properties(keel-vk-server PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

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
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/shaders")

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
    COMMAND ${GLSLC_COMMAND} --target-env=vulkan1.3 "${CMAKE_SOURCE_DIR}/${shader}" -o "${shader_output}"
    DEPENDS "${CMAKE_SOURCE_DIR}/${shader}"
    COMMENT "Compiling shader ${shader}"
    VERBATIM
  )
  list(APPEND SHADER_SPV_FILES "${shader_output}")
endforeach()

add_custom_target(shaders DEPENDS ${SHADER_SPV_FILES})
add_dependencies(keel-vk shaders)

# --- Copy packages/ next to the executable: keel::Vfs mounts it relative
# to SDL_GetBasePath(), which is the executable's own directory. ---
add_custom_command(TARGET keel-vk POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/packages"
    "$<TARGET_FILE_DIR:keel-vk>/packages"
  COMMENT "Copying packages/ next to the executable"
)

# --- Windows: copy SDL3.dll next to the built executable ---
if(WIN32)
  add_custom_command(TARGET keel-vk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:SDL3::SDL3>"
      "$<TARGET_FILE_DIR:keel-vk>"
    COMMENT "Copying SDL3.dll next to the executable"
  )
endif()
