#include "Host.h"

#include "Protocol.h"

#include <enet/enet.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace net {

bool Host::initialize() {
    return enet_initialize() == 0;
}

void Host::shutdown() {
    enet_deinitialize();
}

Host::Host(uint16_t listenPort) : isServer_(listenPort != 0) {
    ENetAddress address{};
    ENetAddress* addressPtr = nullptr;
    if (isServer_) {
        address.host = ENET_HOST_ANY;
        address.port = listenPort;
        addressPtr = &address;
    }

    // peerCount=32, channelLimit=1: this is a transport, not a matchmaking
    // service; a real peer cap belongs to whatever system uses this later.
    host_ = enet_host_create(addressPtr, 32, 1, 0, 0);
    if (!host_) {
        throw std::runtime_error("Failed to create ENet host");
    }
}

Host::~Host() {
    if (host_) {
        enet_host_destroy(host_);
    }
}

void Host::connect(const std::string& address, uint16_t port) {
    ENetAddress enetAddress{};
    enet_address_set_host(&enetAddress, address.c_str());
    enetAddress.port = port;

    peer_ = enet_host_connect(host_, &enetAddress, 1, 0);
    if (!peer_) {
        throw std::runtime_error("Failed to initiate ENet connection");
    }
}

void Host::sendHeartbeatTo(ENetPeer* peer) {
    MessageHeader header{};
    ENetPacket* packet = enet_packet_create(&header, sizeof(header), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, packet);
}

void Host::sendHeartbeat() {
    if (peer_) {
        sendHeartbeatTo(peer_);
    }
}

void Host::service(uint32_t timeoutMs) {
    ENetEvent event;
    int result = enet_host_service(host_, &event, timeoutMs);
    while (result > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                std::printf("[net] peer connected\n");
                break;

            case ENET_EVENT_TYPE_DISCONNECT:
                std::printf("[net] peer disconnected\n");
                if (event.peer == peer_) {
                    peer_ = nullptr;
                }
                break;

            case ENET_EVENT_TYPE_RECEIVE: {
                if (event.packet->dataLength >= sizeof(MessageHeader)) {
                    MessageHeader header{};
                    std::memcpy(&header, event.packet->data, sizeof(header));
                    if (header.version == kProtocolVersion && header.type == MessageType::Heartbeat) {
                        std::printf("[net] heartbeat\n");
                        // Server echoes back so the client sees a round
                        // trip; a client has nothing else to answer to.
                        if (isServer_) {
                            sendHeartbeatTo(event.peer);
                        }
                    }
                }
                enet_packet_destroy(event.packet);
                break;
            }

            default:
                break;
        }
        result = enet_host_service(host_, &event, 0); // drain remaining queued events without blocking again
    }
}

} // namespace net
