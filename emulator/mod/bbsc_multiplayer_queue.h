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
    bool HasPending(int id, uint64_t now) {
        auto it = sockets.find(id); if (it == sockets.end()) return false;
        auto& q = it->second.packets;
        while (!q.empty() && now - q.front().received > 3000) q.pop_front();
        return !q.empty();
    }
    bool Pop(int id, uint64_t now, bool peek, Packet& out) {
        auto it = sockets.find(id); if (it == sockets.end()) return false;
        auto& q = it->second.packets;
        while (!q.empty() && now - q.front().received > 3000) q.pop_front();
        if (q.empty()) return false;
        out = q.front(); if (!peek) q.pop_front(); return true;
    }
};

// Guest ABI layout. Keep the emulator's event data and descriptor identity intact.
struct PollEvent {
    uint32_t events = 0, pad = 0;
    uint64_t ident = 0, data = 0;
};
static_assert(sizeof(PollEvent) == 24);

// Caller holds the same mutex as Inbox. Native sockets continue using the native poller.
class Readiness {
    std::map<int, std::map<int, PollEvent>> polls;
public:
    void Control(int eid, int op, int id, const PollEvent* event) {
        if (op == 3) {
            auto it = polls.find(eid);
            if (it != polls.end()) {
                it->second.erase(id);
                if (it->second.empty()) polls.erase(it);
            }
        } else if ((op == 1 || op == 2) && event) {
            polls[eid][id] = *event;
        }
    }
    void CloseSocket(int id) {
        for (auto& [eid, entries] : polls) entries.erase(id);
    }
    void Destroy(int eid) { polls.erase(eid); }
    bool TracksBound(int eid, const Inbox& inbox) const {
        auto it = polls.find(eid); if (it == polls.end()) return false;
        for (const auto& [id, event] : it->second)
            if ((event.events & 1) && inbox.Vport(id)) return true;
        return false;
    }
    int Merge(int eid, Inbox& inbox, uint64_t now, PollEvent* events, int count, int capacity) {
        auto it = polls.find(eid); if (it == polls.end()) return count;
        for (const auto& [id, event] : it->second) {
            if (!(event.events & 1) || !inbox.HasPending(id, now)) continue;
            int index = 0;
            while (index < count && events[index].ident != static_cast<uint64_t>(id)) ++index;
            if (index < count) {
                events[index].events |= 1;
            } else if (count < capacity) {
                events[count++] = {1, 0, static_cast<uint64_t>(id), event.data};
            }
        }
        return count;
    }
};
}
