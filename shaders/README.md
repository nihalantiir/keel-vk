# shaders/

GLSL sources, compiled to SPIR-V by CMake at build time (see the shader
compilation block in the top-level `CMakeLists.txt`). Output `.spv` files are
written to the build tree (`build/<preset>/bin/shaders/`) next to the
executable. Nothing under this directory is a build artifact, and
nothing here needs to be copied or installed manually.

Naming convention: `<name>.<stage>`, e.g. `cube.vert` / `cube.frag`,
compiled to `<name>.<stage>.spv`. `keel::ShaderModule` loads compiled
modules by that relative path (e.g. `"shaders/cube.vert.spv"`).

Add a new shader by dropping the source file here and adding it to the
`SHADER_SOURCES` list in `CMakeLists.txt`.
