#include "Config.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace client {

namespace {

std::string trim(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

void applyField(Config& config, const std::string& key, const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (key == "window_width") {
        config.windowWidth = std::atoi(value.c_str());
    } else if (key == "window_height") {
        config.windowHeight = std::atoi(value.c_str());
    } else if (key == "present_mode") {
        config.presentModePreference = value;
    } else if (key == "package_root") {
        config.packageRootOverride = value;
    } else if (key == "connect_address") {
        config.wantsNet = true;
        config.connectAddress = value;
    } else if (key == "connect_port") {
        config.connectPort = static_cast<uint16_t>(std::atoi(value.c_str()));
    }
}

// key = value per line, "#" comments, blank lines skipped. No sections,
// no arrays, no nesting - a home-grown microparser for a handful of flat
// fields, not a TOML library.
void applyToml(const std::string& content, Config& config) {
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        applyField(config, trim(line.substr(0, eq)), stripQuotes(trim(line.substr(eq + 1))));
    }
}

// Deliberately not a general JSON parser, same shape as keel::Vfs's
// package.json reader: a small string-search extractor for this flat,
// known set of fields.
std::string extractJsonValue(const std::string& content, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = content.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    pos = content.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return {};
    }
    ++pos;
    while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos]))) {
        ++pos;
    }
    if (pos >= content.size()) {
        return {};
    }
    if (content[pos] == '"') {
        const size_t end = content.find('"', pos + 1);
        if (end == std::string::npos) {
            return {};
        }
        return content.substr(pos + 1, end - pos - 1);
    }
    size_t end = pos;
    while (end < content.size() &&
           (std::isdigit(static_cast<unsigned char>(content[end])) || content[end] == '-')) {
        ++end;
    }
    return content.substr(pos, end - pos);
}

void applyJson(const std::string& content, Config& config) {
    static const char* kKeys[] = {"window_width",   "window_height", "present_mode",
                                   "package_root",   "connect_address", "connect_port"};
    for (const char* key : kKeys) {
        applyField(config, key, extractJsonValue(content, key));
    }
}

// "--connect" alone means the default address/port; "--connect=host" or
// "--connect=host:port" override either part.
void applyConnectFlag(const std::string& arg, Config& config) {
    config.wantsNet = true;
    const size_t eq = arg.find('=');
    if (eq == std::string::npos) {
        return;
    }
    const std::string value = arg.substr(eq + 1);
    const size_t colon = value.find(':');
    if (colon != std::string::npos) {
        config.connectAddress = value.substr(0, colon);
        config.connectPort = static_cast<uint16_t>(std::atoi(value.substr(colon + 1).c_str()));
    } else {
        config.connectAddress = value;
    }
}

void applyCli(int argc, char** argv, Config& config) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--connect", 0) == 0) {
            applyConnectFlag(arg, config);
        } else if (arg.rfind("--present-mode=", 0) == 0) {
            config.presentModePreference = arg.substr(15);
        } else if (arg.rfind("--packages=", 0) == 0) {
            config.packageRootOverride = arg.substr(11);
        }
    }
}

} // namespace

Config Config::load(int argc, char** argv) {
    Config config;

    const char* basePathCStr = SDL_GetBasePath();
    if (basePathCStr) {
        const std::string basePath = basePathCStr;
        const std::string tomlContent = readFile(basePath + "keel.toml");
        if (!tomlContent.empty()) {
            applyToml(tomlContent, config);
        } else {
            const std::string jsonContent = readFile(basePath + "keel.json");
            if (!jsonContent.empty()) {
                applyJson(jsonContent, config);
            }
        }
    }

    applyCli(argc, argv, config);
    return config;
}

VkPresentModeKHR toVkPresentMode(const std::string& preference) {
    if (preference == "fifo") {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    if (preference == "immediate") {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_MAILBOX_KHR; // default, and unrecognized values alike
}

} // namespace client
