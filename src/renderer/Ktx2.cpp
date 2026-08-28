#include "Ktx2.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace renderer {

namespace {

constexpr std::array<uint8_t, 12> kKtx2Identifier = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                       0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

template <typename T>
T readValue(const std::vector<uint8_t>& bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

} // namespace

Ktx2Image loadKtx2(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Ktx2: failed to open " + path);
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    constexpr size_t kHeaderStart = kKtx2Identifier.size();
    constexpr size_t kHeaderSize = 9 * sizeof(uint32_t);
    constexpr size_t kIndexSize = 4 * sizeof(uint32_t) + 2 * sizeof(uint64_t);
    constexpr size_t kLevelEntrySize = 3 * sizeof(uint64_t);

    if (bytes.size() < kHeaderStart + kHeaderSize + kIndexSize + kLevelEntrySize) {
        throw std::runtime_error("Ktx2: " + path + " is too small to be a valid KTX2 file");
    }
    if (!std::equal(kKtx2Identifier.begin(), kKtx2Identifier.end(), bytes.begin())) {
        throw std::runtime_error("Ktx2: " + path + " has no KTX2 identifier");
    }

    size_t offset = kHeaderStart;
    const auto readU32 = [&]() {
        const uint32_t value = readValue<uint32_t>(bytes, offset);
        offset += sizeof(uint32_t);
        return value;
    };

    const uint32_t vkFormat = readU32();
    readU32(); // typeSize: unused, always 1 for a block-compressed format
    const uint32_t width = readU32();
    const uint32_t height = readU32();
    const uint32_t pixelDepth = readU32();
    const uint32_t layerCount = readU32();
    const uint32_t faceCount = readU32();
    const uint32_t levelCount = readU32();
    const uint32_t supercompressionScheme = readU32();

    // pixelDepth 0, layerCount 0, faceCount 1: a plain non-array 2D
    // texture, no cubemap. levelCount 1: no mip chain (nothing streams
    // mips yet). supercompressionScheme 0: block data is stored as-is,
    // no Basis Universal / zstd transcode step this reader doesn't have.
    if (pixelDepth != 0 || layerCount != 0 || faceCount != 1 || levelCount != 1 || supercompressionScheme != 0) {
        throw std::runtime_error("Ktx2: " + path +
                                  " uses a layout this reader doesn't support (mips/layers/faces/supercompression)");
    }

    offset += kIndexSize; // dfd/kvd/sgd offsets and lengths: unused, this reader trusts vkFormat directly

    const uint64_t levelOffset = readValue<uint64_t>(bytes, offset);
    const uint64_t levelLength = readValue<uint64_t>(bytes, offset + sizeof(uint64_t));

    if (bytes.size() < levelOffset + levelLength) {
        throw std::runtime_error("Ktx2: " + path + " level 0 data extends past the end of the file");
    }

    Ktx2Image image;
    image.width = width;
    image.height = height;
    image.format = static_cast<VkFormat>(vkFormat);
    image.data.assign(bytes.begin() + static_cast<ptrdiff_t>(levelOffset),
                       bytes.begin() + static_cast<ptrdiff_t>(levelOffset + levelLength));
    return image;
}

} // namespace renderer
