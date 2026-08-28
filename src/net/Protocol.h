#pragma once

#include <cstdint>

namespace net {

// Bumped whenever MessageHeader or any MessageType payload layout changes.
// A peer with a mismatched version is rejected, not negotiated with.
constexpr uint8_t kProtocolVersion = 1;

enum class MessageType : uint8_t {
    Heartbeat = 0,
};

#pragma pack(push, 1)
struct MessageHeader {
    uint8_t version = kProtocolVersion;
    MessageType type = MessageType::Heartbeat;
};
#pragma pack(pop)

} // namespace net
