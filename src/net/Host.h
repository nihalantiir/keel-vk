#pragma once

#include <cstdint>
#include <string>

struct _ENetHost;
struct _ENetPeer;

namespace net {

// Thin ENet wrapper: transport only. No replication, no registry dump,
// just connect/heartbeat/disconnect so keel-net has something real to
// build on later.
class Host {
public:
    // Must be called once before any Host is constructed, and shutdown()
    // once after the last one is destroyed; ENet's global state is
    // process-wide, not per-host.
    static bool initialize();
    static void shutdown();

    // listenPort == 0 creates a client-mode host (outgoing connections
    // only); nonzero listens for incoming connections (server mode).
    explicit Host(uint16_t listenPort);
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Client mode only. Connection completes asynchronously; watch for the
    // next service() call's connect log.
    void connect(const std::string& address, uint16_t port);

    // Pumps ENet events for up to timeoutMs. Logs connects/disconnects to
    // stdout, answers Heartbeat with Heartbeat. Call every frame/tick.
    void service(uint32_t timeoutMs);

    // Client mode only: sends a Heartbeat to the connected peer, if any.
    void sendHeartbeat();

    bool isConnected() const { return peer_ != nullptr; }

private:
    void sendHeartbeatTo(_ENetPeer* peer);

    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr; // client mode: the server; server mode: unused
    bool isServer_ = false;
};

} // namespace net
