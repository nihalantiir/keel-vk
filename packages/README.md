# packages/

Content packs, mounted by `keel::Vfs` (`src/shared/Vfs.h`) from
`<SDL_GetBasePath()>packages/` at startup. Copied next to the built
executable by CMake (see the `packages/` `POST_BUILD` step). Any
subdirectory containing a `package.json` is mounted as a package root; the
first mounted package that has a given relative path wins.

`packages/base/` is the one package that ships with this template (the
cube's checker texture lives there). See
[Extending](https://github.com/nihalantiir/keel-vk/wiki/Extending) for
where the pack format is expected to grow and what stays out of
`src/keel-vk/`.

Textures here are raw RGBA8 (see `textures/checker.rgba8`): no
image-loading library, no stb_image, because the format is known ahead of
time and a straight binary read is enough for a debug checker pattern.
That's a placeholder, not a policy - a real cook step (KTX2/BC7, mipmaps,
compression) belongs here later, once something actually needs it.
