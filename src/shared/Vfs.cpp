#include "Vfs.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace keel {

namespace {

namespace fs = std::filesystem;

// Deliberately not a general JSON parser: package.json is a small, flat,
// known set of string/number fields, and this stub doesn't need a real
// parser dependency for that. Returns an empty string if the key isn't
// found as a quoted string value.
std::string extractJsonString(const std::string& content, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = content.find(needle);
    if (keyPos == std::string::npos) {
        return {};
    }
    const size_t firstQuote = content.find('"', keyPos + needle.size());
    if (firstQuote == std::string::npos) {
        return {};
    }
    const size_t secondQuote = content.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos) {
        return {};
    }
    return content.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

} // namespace

Vfs::Vfs(const std::string& packagesRoot) {
    fs::path packagesDir;
    if (!packagesRoot.empty()) {
        packagesDir = fs::path(packagesRoot);
    } else {
        const char* basePathCStr = SDL_GetBasePath();
        if (!basePathCStr) {
            throw std::runtime_error("Failed to resolve SDL base path for package discovery");
        }
        packagesDir = fs::path(basePathCStr) / "packages";
    }

    if (!fs::exists(packagesDir)) {
        throw std::runtime_error("packages/ not found at: " + fs::absolute(packagesDir).string());
    }

    for (const auto& entry : fs::directory_iterator(packagesDir)) {
        if (!entry.is_directory()) {
            continue;
        }
        const fs::path manifestPath = entry.path() / "package.json";
        if (!fs::exists(manifestPath)) {
            continue;
        }

        std::ifstream file(manifestPath, std::ios::binary);
        std::ostringstream contentStream;
        contentStream << file.rdbuf();
        const std::string content = contentStream.str();

        Package package;
        package.name = extractJsonString(content, "name");
        package.rootPath = entry.path().string();
        packages_.push_back(package);

        std::cout << "[vfs] mounted package '" << package.name << "' from " << package.rootPath << std::endl;
    }
}

std::string Vfs::resolve(const std::string& relativePath) const {
    for (const Package& package : packages_) {
        const fs::path candidate = fs::path(package.rootPath) / relativePath;
        if (fs::exists(candidate)) {
            return candidate.string();
        }
    }
    throw std::runtime_error("Not found in any mounted package: " + relativePath);
}

} // namespace keel
