// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC mod: the P2P channel - BBSC's own packets over the game's own player-to-player connection,
// for servers that run no BBSC relay (the public ones: The Hunter's Dream + shadNet).
//
// The players of a session already exchange P2P datagrams (the game's socket, vport 40). The
// emulator's P2P transport (official shadPS4: core/net/p2p_transport.cpp; the shadp2p fork:
// network/p2p_sockets.cpp) carries a source and destination vport in every frame and hands each
// frame to the socket bound to that vport, so a second socket on a vport of our own sees only
// BBSC's frames and the game never sees them.
//
//   - Peers: the game's import slots for sceNetSendto/sceNetRecvfrom are wrapped; every address
//     the game sends to or receives from on a P2P vport is a player of our session (kept while
//     the game talks to it, dropped 20 s after). sceNpSignalingGetConnectionStatus is wrapped too
//     (ACTIVE connections give the peer's address).
//   - Socket: our own DGRAM_P2P socket on ChannelVport, made through the emulator's sceNet
//     functions (the originals from the same slots), non-blocking, read by our own thread.
//   - Frames: the relay's frames unchanged (the "SHAD" 0x40 envelope with sender + session,
//     bbsc_relay.cpp), one datagram each; the receive side is the relay's Dispatch.
//
// Import slots, CUSA03173 01.09 (tools\eboot-imports.py, BBSC offsets):
//   0x053E4B00 sceNetSocket   0x053E4B38 sceNetBind   0x053E4B68 sceNetSetsockopt
//   0x053E4F48 sceNetSendto   0x053E4F58 sceNetRecvfrom
//   0x053E52D0 sceNpSignalingGetConnectionStatus
// Switch SHADPS4_BBSC_P2P_CHANNEL (Mods tab, on). Datagrams can be lost: every BBSC packet was
// made for the emulator's UDP signaling first and is re-sent or repeated by its sender.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "common/logging/log.h"
#include "common/types.h"

namespace BbscMod {
u64 EbootAddress(); // bbsc_mod.cpp
void StartMultiplayerRelay();
bool ReadSlotSafely(u64 address, u64* value);  // bbsc_mod.cpp: no fault if unreadable
bool WriteSlotSafely(u64 address, u64 value);
void GuardSlot(u64 slot_offset, u64 wrapper, void** original, const char* name);
void RelayDispatch(const std::vector<u8>& wrapped); // bbsc_relay.cpp
} // namespace BbscMod

namespace {

constexpr u64 EbootSize = 0x056D4000ULL;
constexpr u64 SlotSocket = 0x053E4B00;
constexpr u64 SlotSocketClose = 0x053E4B08;
constexpr u64 SlotBind = 0x053E4B38;
constexpr u64 SlotSetsockopt = 0x053E4B68;
constexpr u64 SlotSendto = 0x053E4F48;
constexpr u64 SlotRecvfrom = 0x053E4F58;
constexpr u64 SlotConnectionStatus = 0x053E52D0;

constexpr u16 ChannelVport = 3173; // below 32768 (official shadPS4 reserves the rest); game uses 40
constexpr s32 OrbisAfInet = 2;
constexpr s32 OrbisSockDgramP2P = 6;
constexpr s32 OrbisSolSocket = 0xFFFF;
constexpr s32 OrbisSoNbio = 0x1200;
constexpr s32 OrbisMsgDontwait = 0x80;
constexpr u32 SignalingConnActive = 2;
constexpr s64 PeerKeepMs = 20'000;
constexpr size_t MaxDatagram = 2048;
constexpr size_t MaxGameDatagram = 65'000;
constexpr s64 RelayFallbackMs = 1'200;

#pragma pack(push, 1)
struct OrbisSockaddrIn {
    u8 sin_len;
    u8 sin_family;
    u16 sin_port;  // network order (P2P datagrams: the peer's UDP port)
    u32 sin_addr;  // network order
    u16 sin_vport; // network order
    char sin_zero[6];
};
#pragma pack(pop)
static_assert(sizeof(OrbisSockaddrIn) == 16);

using SocketFn = s32 PS4_SYSV_ABI (*)(const char*, s32, s32, s32);
using SocketCloseFn = s32 PS4_SYSV_ABI (*)(s32);
using BindFn = s32 PS4_SYSV_ABI (*)(s32, const void*, u32);
using SetsockoptFn = s32 PS4_SYSV_ABI (*)(s32, s32, s32, const void*, u32);
using SendtoFn = s32 PS4_SYSV_ABI (*)(s32, const void*, u64, s32, const void*, u32);
using RecvfromFn = s32 PS4_SYSV_ABI (*)(s32, void*, u64, s32, void*, u32*);
using ConnectionStatusFn = s32 PS4_SYSV_ABI (*)(s32, s32, u32*, u32*, u16*);

SendtoFn original_sendto = nullptr;
RecvfromFn original_recvfrom = nullptr;
ConnectionStatusFn original_connection_status = nullptr;
SocketFn original_socket = nullptr;
SocketCloseFn original_socket_close = nullptr;

struct Peer {
    s64 seen_ms = 0;
    s64 first_send_ms = 0;
    bool direct_heard = false;
    bool relay_heard = false;
    bool logged = false;
};

struct RelayedGamePacket {
    OrbisSockaddrIn from{};
    std::vector<u8> data;
};

struct PendingMirror {
    u64 peer_key = 0;
    s64 due_ms = 0;
    std::vector<u8> packet;
};

std::mutex peers_mutex;
std::map<u64, Peer> peers; // key: addr << 16 | port (both network order)
std::atomic<s32> channel_socket{-1};
std::atomic<bool> enabled{false};
std::atomic<u32> sent_count{0};
std::atomic<u32> received_count{0};
std::atomic<bool> game_relay_enabled{false};
std::atomic<SOCKET> game_relay_socket{INVALID_SOCKET};
sockaddr_in game_relay_server{};
std::mutex game_sockets_mutex;
std::set<s32> game_p2p_sockets;
std::mutex game_relay_queue_mutex;
std::deque<RelayedGamePacket> game_relay_queue;
std::mutex pending_mirrors_mutex;
std::deque<PendingMirror> pending_mirrors;

s64 NowMs() {
    return static_cast<s64>(GetTickCount64());
}

std::string AddressText(u32 addr, u16 port) {
    const auto* b = reinterpret_cast<const u8*>(&addr);
    return fmt::format("{}.{}.{}.x:{}", b[0], b[1], b[2], static_cast<u16>((port >> 8) | (port << 8)));
}

void NotePeer(u32 addr, u16 port, const char* how) {
    if (addr == 0 || port == 0) {
        return;
    }
    const u64 key = (static_cast<u64>(addr) << 16) | port;
    std::scoped_lock lock{peers_mutex};
    Peer& peer = peers[key];
    const bool fresh = !peer.logged || NowMs() - peer.seen_ms > PeerKeepMs;
    peer.seen_ms = NowMs();
    if (fresh) {
        peer.logged = true;
        LOG_INFO(Mod, "P2P channel: player at {} ({})", AddressText(addr, port), how);
    }
}

void NoteGameAddress(const void* addr, u32 len, const char* how) {
    if (!enabled || addr == nullptr || len < sizeof(OrbisSockaddrIn)) {
        return;
    }
    OrbisSockaddrIn in{};
    std::memcpy(&in, addr, sizeof(in));
    // A P2P address has a vport; plain UDP/TCP sockets of the game leave it 0.
    if (in.sin_family != OrbisAfInet || in.sin_vport == 0 ||
        in.sin_vport == static_cast<u16>((ChannelVport >> 8) | (ChannelVport << 8))) {
        return;
    }
    NotePeer(in.sin_addr, in.sin_port, how);
}

u64 PeerKey(u32 addr, u16 port) {
    return (static_cast<u64>(addr) << 16) | port;
}

bool IsP2PSocket(s32 s) {
    std::scoped_lock lock{game_sockets_mutex};
    return game_p2p_sockets.contains(s);
}

s32 PS4_SYSV_ABI Socket(const char* name, s32 domain, s32 type, s32 protocol) {
    const s32 s = original_socket(name, domain, type, protocol);
    if (s >= 0 && domain == OrbisAfInet && type == OrbisSockDgramP2P) {
        std::scoped_lock lock{game_sockets_mutex};
        game_p2p_sockets.insert(s);
    }
    return s;
}

s32 PS4_SYSV_ABI SocketClose(s32 s) {
    {
        std::scoped_lock lock{game_sockets_mutex};
        game_p2p_sockets.erase(s);
    }
    return original_socket_close(s);
}

bool PeerNeedsRelayNow(const OrbisSockaddrIn& to, bool* direct) {
    std::scoped_lock lock{peers_mutex};
    Peer& peer = peers[PeerKey(to.sin_addr, to.sin_port)];
    const s64 now = NowMs();
    peer.seen_ms = now;
    if (peer.first_send_ms == 0)
        peer.first_send_ms = now;
    *direct = peer.direct_heard;
    return peer.relay_heard;
}

void MirrorGameDatagram(const void* buf, u64 len, const void* addr, u32 addrlen) {
    const SOCKET relay = game_relay_socket.load();
    if (!game_relay_enabled || relay == INVALID_SOCKET || !buf || len == 0 ||
        len > MaxGameDatagram || !addr || addrlen < sizeof(OrbisSockaddrIn))
        return;
    OrbisSockaddrIn to{};
    std::memcpy(&to, addr, sizeof(to));
    if (to.sin_family != OrbisAfInet || to.sin_vport == 0)
        return;
    std::vector<u8> packet(13 + static_cast<size_t>(len));
    std::memcpy(packet.data(), "BBU1\x03", 5);
    std::memcpy(packet.data() + 5, &to.sin_addr, 4);
    std::memcpy(packet.data() + 9, &to.sin_port, 2);
    std::memcpy(packet.data() + 11, &to.sin_vport, 2);
    std::memcpy(packet.data() + 13, buf, static_cast<size_t>(len));
    bool direct = false;
    if (PeerNeedsRelayNow(to, &direct)) {
        ::sendto(relay, reinterpret_cast<const char*>(packet.data()),
                 static_cast<int>(packet.size()), 0,
                 reinterpret_cast<const sockaddr*>(&game_relay_server), sizeof(game_relay_server));
    } else if (!direct) {
        std::scoped_lock lock{pending_mirrors_mutex};
        if (pending_mirrors.size() >= 2048)
            pending_mirrors.pop_front();
        pending_mirrors.push_back(
            PendingMirror{PeerKey(to.sin_addr, to.sin_port), NowMs() + RelayFallbackMs,
                          std::move(packet)});
    }
}

bool PopRelayedGamePacket(s32 s, void* buf, u64 len, void* addr, u32* addrlen, s32* result) {
    if (!game_relay_enabled || !IsP2PSocket(s) || !buf || len == 0)
        return false;
    RelayedGamePacket packet;
    {
        std::scoped_lock lock{game_relay_queue_mutex};
        if (game_relay_queue.empty())
            return false;
        packet = std::move(game_relay_queue.front());
        game_relay_queue.pop_front();
    }
    const size_t copied = std::min<size_t>(packet.data.size(), static_cast<size_t>(len));
    std::memcpy(buf, packet.data.data(), copied);
    if (addr && addrlen && *addrlen >= sizeof(packet.from)) {
        std::memcpy(addr, &packet.from, sizeof(packet.from));
        *addrlen = sizeof(packet.from);
    }
    *result = static_cast<s32>(copied);
    return true;
}

s32 PS4_SYSV_ABI Sendto(s32 s, const void* buf, u64 len, s32 flags, const void* addr, u32 addrlen) {
    const s32 r = original_sendto(s, buf, len, flags, addr, addrlen);
    if (s != channel_socket && addr && addrlen >= sizeof(OrbisSockaddrIn)) {
        OrbisSockaddrIn to{};
        std::memcpy(&to, addr, sizeof(to));
        if (to.sin_family == OrbisAfInet && to.sin_vport != 0) {
            std::scoped_lock lock{game_sockets_mutex};
            game_p2p_sockets.insert(s); // also covers sockets created before our wrappers installed
        }
        NoteGameAddress(addr, addrlen, "game sends to it");
        MirrorGameDatagram(buf, len, addr, addrlen);
    }
    return r;
}

s32 PS4_SYSV_ABI Recvfrom(s32 s, void* buf, u64 len, s32 flags, void* addr, u32* addrlen) {
    s32 relayed = 0;
    if (s != channel_socket && PopRelayedGamePacket(s, buf, len, addr, addrlen, &relayed))
        return relayed;
    const s32 r = original_recvfrom(s, buf, len, flags, addr, addrlen);
    if (r > 0 && addrlen != nullptr && s != channel_socket) {
        NoteGameAddress(addr, *addrlen, "game hears from it");
        if (addr && *addrlen >= sizeof(OrbisSockaddrIn)) {
            OrbisSockaddrIn from{};
            std::memcpy(&from, addr, sizeof(from));
            std::scoped_lock lock{peers_mutex};
            peers[PeerKey(from.sin_addr, from.sin_port)].direct_heard = true;
        }
    }
    return r;
}

s32 PS4_SYSV_ABI ConnectionStatus(s32 ctx, s32 conn, u32* status, u32* addr, u16* port) {
    // The game may pass no address pointers: ask with our own, hand back what it asked for.
    u32 own_addr = 0;
    u16 own_port = 0;
    const s32 r = original_connection_status(ctx, conn, status, addr ? addr : &own_addr,
                                             port ? port : &own_port);
    if (r == 0 && status != nullptr && *status == SignalingConnActive) {
        NotePeer(addr ? *addr : own_addr, port ? *port : own_port, "signaling connection active");
    }
    return r;
}

template <typename Fn>
bool Swap(u64 slot_offset, Fn wrapper, Fn& original, const char* name) {
    const u64 slot = BbscMod::EbootAddress() + slot_offset;
    for (int i = 0; i < 60'000; ++i) {
        u64 value = 0;
        if (BbscMod::ReadSlotSafely(slot, &value) && value != 0 &&
            (value < BbscMod::EbootAddress() || value >= BbscMod::EbootAddress() + EbootSize)) {
            original = reinterpret_cast<Fn>(value);
            if (!BbscMod::WriteSlotSafely(slot, reinterpret_cast<u64>(wrapper))) {
                Sleep(1);
                continue; // page not writable right now: try again
            }
            BbscMod::GuardSlot(slot_offset, reinterpret_cast<u64>(wrapper),
                               reinterpret_cast<void**>(&original), name);
            return true;
        }
        Sleep(1);
    }
    return false;
}

template <typename Fn>
Fn ReadSlot(u64 slot_offset) {
    u64 value = 0;
    if (!BbscMod::ReadSlotSafely(BbscMod::EbootAddress() + slot_offset, &value) || value == 0 ||
        (value >= BbscMod::EbootAddress() && value < BbscMod::EbootAddress() + EbootSize)) {
        return nullptr;
    }
    return reinterpret_cast<Fn>(value);
}

// Made on the first peer: the emulator starts its P2P transport when the game needs it.
// Made on the first peer: the emulator starts its P2P transport when the game needs it. The slots
// are read here, not at start-up (review 2026-10-11): the official shadPS4 links the imports again
// after BBSC starts, and these slots are not wrapped, so they always hold the emulator's current
// functions.
bool OpenSocket() {
    const auto open = ReadSlot<SocketFn>(SlotSocket);
    const auto bind = ReadSlot<BindFn>(SlotBind);
    const auto setsockopt = ReadSlot<SetsockoptFn>(SlotSetsockopt);
    const auto close = ReadSlot<SocketCloseFn>(SlotSocketClose);
    if (open == nullptr || bind == nullptr || setsockopt == nullptr) {
        LOG_WARNING(Mod, "P2P channel: the game's socket imports are not filled - retrying");
        return false;
    }
    const s32 s = open("BBSC channel", OrbisAfInet, OrbisSockDgramP2P, 0);
    if (s < 0) {
        LOG_WARNING(Mod, "P2P channel: sceNetSocket failed ({:#x}) - retrying", static_cast<u32>(s));
        return false;
    }
    OrbisSockaddrIn bind_to{};
    bind_to.sin_len = sizeof(bind_to);
    bind_to.sin_family = OrbisAfInet;
    bind_to.sin_vport = static_cast<u16>((ChannelVport >> 8) | (ChannelVport << 8));
    if (const s32 r = bind(s, &bind_to, sizeof(bind_to)); r < 0) {
        LOG_WARNING(Mod, "P2P channel: bind to vport {} failed ({:#x}) - retrying", ChannelVport,
                    static_cast<u32>(r));
        if (close != nullptr) {
            close(s);
        }
        return false;
    }
    const s32 on = 1;
    setsockopt(s, OrbisSolSocket, OrbisSoNbio, &on, sizeof(on));
    channel_socket = s;
    LOG_INFO(Mod, "P2P channel: socket {} on vport {} - BBSC packets go to the players of the "
                  "session directly",
             s, ChannelVport);
    return true;
}

DWORD WINAPI ChannelThread(void*) {
    std::vector<u8> buffer(MaxDatagram);
    s64 next_open_ms = 0;
    int failures = 0;
    s64 next_report_ms = NowMs() + 60'000;
    for (;;) {
        const s32 s = channel_socket;
        if (s < 0) {
            bool any = false;
            {
                std::scoped_lock lock{peers_mutex};
                any = !peers.empty();
            }
            if (any && NowMs() >= next_open_ms && failures < 10 && !OpenSocket()) {
                next_open_ms = NowMs() + 5'000;
                if (++failures == 10) {
                    LOG_ERROR(Mod, "P2P channel: no socket after 10 tries - channel off for this run");
                }
            }
            Sleep(100);
            continue;
        }
        bool got_any = false;
        for (int i = 0; i < 64; ++i) {
            OrbisSockaddrIn from{};
            u32 from_len = sizeof(from);
            const s32 r = original_recvfrom(s, buffer.data(), buffer.size(), OrbisMsgDontwait,
                                            &from, &from_len);
            if (r <= 0) {
                break;
            }
            got_any = true;
            ++received_count;
            NotePeer(from.sin_addr, from.sin_port, "BBSC packet from it");
            BbscMod::RelayDispatch(std::vector<u8>(buffer.begin(), buffer.begin() + r));
        }
        if (NowMs() >= next_report_ms) {
            next_report_ms = NowMs() + 60'000;
            const u32 sent = sent_count.exchange(0);
            const u32 received = received_count.exchange(0);
            if (sent != 0 || received != 0) {
                LOG_INFO(Mod, "P2P channel: last minute {} packet(s) sent, {} received", sent,
                         received);
            }
        }
        Sleep(got_any ? 1 : 5);
    }
}

DWORD WINAPI GameRelayThread(void*) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR(Mod, "Game relay: WSAStartup failed");
        return 0;
    }
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        LOG_ERROR(Mod, "Game relay: UDP socket failed ({})", WSAGetLastError());
        WSACleanup();
        return 0;
    }
    u_long nonblocking = 1;
    ioctlsocket(socket, FIONBIO, &nonblocking);
    game_relay_socket = socket;

    std::string npid = std::getenv("BB_SHADNET_NPID") ? std::getenv("BB_SHADNET_NPID") : "";
    if (npid.size() > 16)
        npid.resize(16);
    std::array<u8, 21> registration{};
    std::memcpy(registration.data(), "BBU1\x01", 5);
    std::memcpy(registration.data() + 5, npid.data(), npid.size());

    s64 next_registration = 0;
    s64 next_report = NowMs() + 60'000;
    u32 forwarded = 0;
    std::vector<u8> raw(MaxGameDatagram + 13);
    while (game_relay_enabled) {
        const s64 now = NowMs();
        if (now >= next_registration) {
            next_registration = now + 5'000;
            ::sendto(socket, reinterpret_cast<const char*>(registration.data()),
                     static_cast<int>(registration.size()), 0,
                     reinterpret_cast<const sockaddr*>(&game_relay_server),
                     sizeof(game_relay_server));
        }
        std::vector<PendingMirror> due;
        {
            std::scoped_lock lock{pending_mirrors_mutex};
            while (!pending_mirrors.empty() && pending_mirrors.front().due_ms <= now) {
                due.push_back(std::move(pending_mirrors.front()));
                pending_mirrors.pop_front();
            }
        }
        for (const PendingMirror& pending : due) {
            bool direct = false;
            {
                std::scoped_lock lock{peers_mutex};
                direct = peers[pending.peer_key].direct_heard;
            }
            if (!direct) {
                ::sendto(socket, reinterpret_cast<const char*>(pending.packet.data()),
                         static_cast<int>(pending.packet.size()), 0,
                         reinterpret_cast<const sockaddr*>(&game_relay_server),
                         sizeof(game_relay_server));
            }
        }
        for (int i = 0; i < 64; ++i) {
            sockaddr_in from{};
            int from_len = sizeof(from);
            const int received = ::recvfrom(socket, reinterpret_cast<char*>(raw.data()),
                                            static_cast<int>(raw.size()), 0,
                                            reinterpret_cast<sockaddr*>(&from), &from_len);
            if (received < 0) {
                if (WSAGetLastError() != WSAEWOULDBLOCK)
                    LOG_WARNING(Mod, "Game relay: receive failed ({})", WSAGetLastError());
                break;
            }
            if (from.sin_addr.s_addr != game_relay_server.sin_addr.s_addr ||
                from.sin_port != game_relay_server.sin_port || received < 13 ||
                std::memcmp(raw.data(), "BBU1\x04", 5) != 0)
                continue;
            RelayedGamePacket packet;
            packet.from.sin_len = sizeof(packet.from);
            packet.from.sin_family = OrbisAfInet;
            std::memcpy(&packet.from.sin_addr, raw.data() + 5, 4);
            std::memcpy(&packet.from.sin_port, raw.data() + 9, 2);
            std::memcpy(&packet.from.sin_vport, raw.data() + 11, 2);
            packet.data.assign(raw.begin() + 13, raw.begin() + received);
            {
                std::scoped_lock lock{peers_mutex};
                Peer& peer = peers[PeerKey(packet.from.sin_addr, packet.from.sin_port)];
                peer.relay_heard = true;
                peer.seen_ms = now;
            }
            {
                std::scoped_lock lock{game_relay_queue_mutex};
                if (game_relay_queue.size() >= 1024)
                    game_relay_queue.pop_front();
                game_relay_queue.push_back(std::move(packet));
            }
            ++forwarded;
        }
        if (now >= next_report) {
            next_report = now + 60'000;
            if (forwarded != 0) {
                LOG_INFO(Mod, "Game relay: {} datagram(s) received in the last minute", forwarded);
                forwarded = 0;
            }
        }
        Sleep(2);
    }
    game_relay_socket = INVALID_SOCKET;
    closesocket(socket);
    WSACleanup();
    return 0;
}

bool ConfigureGameRelay() {
    const char* raw = std::getenv("BBSC_GAME_RELAY");
    const char* id = std::getenv("BB_SHADNET_NPID");
    if (!raw || !raw[0] || !id || !id[0])
        return false;
    const std::string value(raw);
    const size_t colon = value.rfind(':');
    if (colon == std::string::npos)
        return false;
    const std::string host = value.substr(0, colon);
    char* end = nullptr;
    const long port = std::strtol(value.c_str() + colon + 1, &end, 10);
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(static_cast<u_short>(port));
    if (!end || *end != '\0' || port < 1 || port > 65535 ||
        inet_pton(AF_INET, host.c_str(), &server.sin_addr) != 1) {
        LOG_ERROR(Mod, "Game relay: invalid BBSC_GAME_RELAY '{}' (IPv4:port required)", value);
        return false;
    }
    game_relay_server = server;
    game_relay_enabled = true;
    if (HANDLE thread = CreateThread(nullptr, 0, GameRelayThread, nullptr, 0, nullptr))
        CloseHandle(thread);
    LOG_INFO(Mod, "Game relay: direct P2P remains primary; fallback {} after {} ms", value,
             RelayFallbackMs);
    return true;
}

} // namespace

namespace BbscMod {

bool P2PChannelEnabled() {
    return enabled;
}

// One relay frame (envelope included, no length prefix) to every live peer. Returns the peer count.
u32 P2PChannelSend(const void* data, u32 size) {
    const s32 s = channel_socket;
    if (!enabled || s < 0 || size > MaxDatagram) {
        return 0;
    }
    std::vector<u64> live;
    {
        std::scoped_lock lock{peers_mutex};
        const s64 now = NowMs();
        for (auto it = peers.begin(); it != peers.end();) {
            if (now - it->second.seen_ms > PeerKeepMs) {
                it = peers.erase(it);
            } else {
                live.push_back(it->first);
                ++it;
            }
        }
    }
    u32 count = 0;
    for (const u64 key : live) {
        OrbisSockaddrIn to{};
        to.sin_len = sizeof(to);
        to.sin_family = OrbisAfInet;
        to.sin_addr = static_cast<u32>(key >> 16);
        to.sin_port = static_cast<u16>(key);
        to.sin_vport = static_cast<u16>((ChannelVport >> 8) | (ChannelVport << 8));
        if (original_sendto(s, data, size, OrbisMsgDontwait, &to, sizeof(to)) > 0) {
            ++count;
        }
    }
    sent_count += count;
    return count;
}

u32 P2PChannelPeerCount() {
    std::scoped_lock lock{peers_mutex};
    const s64 now = NowMs();
    u32 count = 0;
    for (const auto& [key, peer] : peers) {
        count += now - peer.seen_ms <= PeerKeepMs ? 1 : 0;
    }
    return count;
}

void StartP2PChannel() {
    const char* multiplayer = std::getenv("BBSC_MULTIPLAYER_RELAY");
    if (multiplayer && std::strcmp(multiplayer, "1") == 0) {
        StartMultiplayerRelay();
        return;
    }
    const char* channel_env = std::getenv("SHADPS4_BBSC_P2P_CHANNEL");
    const bool want_channel = channel_env == nullptr ||
                              (channel_env[0] != '0' && channel_env[0] != 'f' &&
                               channel_env[0] != 'F');
    const bool want_game_relay = std::getenv("BBSC_GAME_RELAY") != nullptr &&
                                 std::getenv("BBSC_GAME_RELAY")[0] != '\0';
    if (!want_channel && !want_game_relay) {
        LOG_INFO(Mod, "P2P channel: off (switch)");
        return;
    }

    if (want_game_relay &&
        (!Swap<SocketFn>(SlotSocket, &Socket, original_socket, "Socket") ||
         !Swap<SocketCloseFn>(SlotSocketClose, &SocketClose, original_socket_close,
                              "SocketClose"))) {
        LOG_ERROR(Mod, "Game relay: socket import slots were never filled - relay off");
        return;
    }
    if (!Swap<SendtoFn>(SlotSendto, &Sendto, original_sendto, "Sendto") ||
        !Swap<RecvfromFn>(SlotRecvfrom, &Recvfrom, original_recvfrom, "Recvfrom")) {
        LOG_ERROR(Mod, "P2P hooks: an import slot was never filled - channel and relay off");
        return;
    }

    if (want_game_relay)
        ConfigureGameRelay();
    if (want_channel) {
        Swap<ConnectionStatusFn>(SlotConnectionStatus, &ConnectionStatus,
                                 original_connection_status, "ConnectionStatus");
        enabled = true;
        if (HANDLE thread = CreateThread(nullptr, 0, ChannelThread, nullptr, 0, nullptr))
            CloseHandle(thread);
        LOG_INFO(Mod, "P2P channel: ready (vport {}), waiting for the players of a session",
                 ChannelVport);
    } else {
        LOG_INFO(Mod, "P2P channel: off; game-datagram relay hooks remain active");
    }
}

} // namespace BbscMod
