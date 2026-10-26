#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace BbscAddressed {
struct Packet {
    uint32_t address = 0;
    uint16_t port = 0, sourceVport = 0, destinationVport = 0;
    uint64_t received = 0;
    std::vector<uint8_t> data;
};
// The caller holds its mutex. Closing/rebinding a socket drops its old packets, so
// recycled game socket IDs cannot consume a previous summon's data.
class Inbox {
    struct Binding { uint16_t vport; std::deque<Packet> packets; };
    std::map<int, Binding> sockets;
public:
    void Bind(int id, uint16_t vport) { sockets[id] = {vport, {}}; }
    void Close(int id) { sockets.erase(id); }
    void ClearPackets() { for (auto& [id, binding] : sockets) binding.packets.clear(); }
    uint16_t Vport(int id) const {
        auto it = sockets.find(id); return it == sockets.end() ? 0 : it->second.vport;
    }
    void Push(Packet packet) {
        for (auto& [id, binding] : sockets) {
            if (binding.vport != packet.destinationVport) continue;
            if (binding.packets.size() >= 256) binding.packets.pop_front();
            binding.packets.push_back(packet);
        }
    }
    bool Pop(int id, uint64_t now, bool peek, Packet& out) {
        auto it = sockets.find(id); if (it == sockets.end()) return false;
        auto& q = it->second.packets;
        while (!q.empty() && now - q.front().received > 3000) q.pop_front();
        if (q.empty()) return false;
        out = q.front(); if (!peek) q.pop_front(); return true;
    }
};
}
