cmake_minimum_required(VERSION 3.24)
project(keel-vk-consumer-smoke LANGUAGES CXX)

# CI-only fixture: proves the keel target is discoverable and linkable
# from an external project via add_subdirectory, the same way a real
# fork would consume this repo - see the wiki's Extending page ("Using
# Keel from another repo"). CI only configures this (no GPU on hosted
# runners to actually run anything); that alone proves the target
# exists, is correctly named, and its headers resolve.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." keel-vk-build)

add_executable(keel_vk_consumer_smoke main.cpp)
target_link_libraries(keel_vk_consumer_smoke PRIVATE keel)
keel_copy_runtime_assets(keel_vk_consumer_smoke)
