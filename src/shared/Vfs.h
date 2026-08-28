#pragma once

#include <string>
#include <vector>

namespace keel {

// Mounts every subdirectory of <SDL_GetBasePath()>packages/ that contains a
// package.json as a package root, in discovery order, and resolves
// relative paths against them (first match wins). No override/priority
// system yet; there is exactly one package (packages/base/) to test
// against.
class Vfs {
public:
    Vfs();

    // Throws if relativePath isn't found in any mounted package.
    std::string resolve(const std::string& relativePath) const;

private:
    struct Package {
        std::string name;
        std::string rootPath;
    };

    std::vector<Package> packages_;
};

} // namespace keel
