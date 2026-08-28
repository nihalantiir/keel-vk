#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace renderer {

// One mip, one layer, one face - this template's scope. A real content
// pipeline's cooked assets would carry a full mip chain; nothing here
// streams mips yet, so there's nothing to gain from reading more than
// level 0.
struct Ktx2Image {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<uint8_t> data; // level 0, exactly as stored on disk
};

// Deliberately not a general KTX2 reader: no supercompression (Basis
// Universal / zstd), no mip chain, no array layers, no cubemaps, no key/
// value metadata parsing (the DFD/KVD/SGD regions are skipped over, not
// read). Throws on anything outside that shape - this reads what this
// project's own cook step produces (see tools/bake_ktx2_fixture.py), not
// arbitrary third-party KTX2 files.
Ktx2Image loadKtx2(const std::string& path);

} // namespace renderer
