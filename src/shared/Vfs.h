#pragma once

#include <string>
#include <vector>

namespace keel {

// Mounts every subdirectory of packagesRoot (default <SDL_GetBasePath()>packages/)
// that contains a package.json as a package root, in discovery order, and
// resolves relative paths against them (first match wins). No override/
// priority system between packages yet; there is exactly one
// (packages/base/) to test against. A missing packages/ directory is a
// hard error, not a silently empty Vfs - see the constructor.
class Vfs {
public:
    // packagesRoot empty (the default) means <SDL_GetBasePath()>packages/;
    // non-empty overrides the root entirely (see client::Config).
    // Throws if the resolved root doesn't exist: an empty Vfs from a typo
    // or a missing packages/ directory should fail loudly at startup, not
    // silently serve nothing to every resolve() call later.
    explicit Vfs(const std::string& packagesRoot = "");

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
