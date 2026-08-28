# Coding conventions

Inherited from simple-vk without change.

- C++20. RAII throughout: every Vulkan/VMA handle a class owns is created
  in its constructor (or a private helper called from it) and destroyed in
  its destructor.
- 4-space indent, 120-column limit, LLVM base style, left-aligned
  pointers/references. See root `.clang-format`. Not auto-applied to the
  tree; sanity-check new code against it rather than reformatting wholesale.
- Naming: types PascalCase, functions and variables camelCase, member
  variables with a trailing underscore (`device_`).
- Headers declare, `.cpp` files implement. Prefer forward declarations in
  headers over includes where possible (see `keel::Window` forward-declaring
  `SDL_Window`/`SDL_Event`, or `renderer::Renderer` forward-declaring
  `debug::DebugUi`).
- Error handling is exceptions only. `keel::vkCheck()` throws
  `std::runtime_error` on any non-`VK_SUCCESS`. `main()` in each
  executable has exactly one top-level `try`/`catch (const std::exception&)`,
  printing `"Fatal error: " + e.what()` to `stderr` and returning
  `EXIT_FAILURE`.
- Comments explain non-obvious *why*, never *what*: VMA/Vulkan flag
  combinations, layout-transition stage/access masks, queue
  ownership/sync ordering, projection conventions, non-obvious ordering
  requirements. Do not restate a class's name or role, document every
  struct field, or narrate what a change did.
- No em dashes anywhere: code, comments, docs, or the changelog. Use
  periods or commas instead.
- `CHANGELOG.md` follows Keep a Changelog + Semantic Versioning, written
  for a reader who wasn't watching the diff land, not as a commit-message
  dump.
- MIT license.
