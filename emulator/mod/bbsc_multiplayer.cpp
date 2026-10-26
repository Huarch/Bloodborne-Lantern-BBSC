// Opt-in addressed relay: no Lantern binary changes; game socket imports only.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "common/logging/log.h"
#include "common/types.h"
#include "bbsc_multiplayer_queue.h"

namespace BbscMod {
u64 EbootAddress();
bool ReadSlotSafely(u64, u64*);
bool WriteSlotSafely(u64, u64);
void GuardSlot(u64, u64, void**, const char*);
}
namespace {
#pragma pack(push, 1)
struct Address { u8 len = 16, family = 2; u16 port = 0; u32 ip = 0; u16 vport = 0; char zero[6]{}; };
#pragma pack(pop)
static_assert(sizeof(Address) == 16);
using BindFn = s32 PS4_SYSV_ABI (*)(s32, const void*, u32);
using CloseFn = s32 PS4_SYSV_ABI (*)(s32);
using SendFn = s32 PS4_SYSV_ABI (*)(s32, const void*, u64, s32, const void*, u32);
using ReceiveFn = s32 PS4_SYSV_ABI (*)(s32, void*, u64, s32, void*, u32*);
using ActivateFn = s32 PS4_SYSV_ABI (*)(s32, const void*, s32*);
using StatusFn = s32 PS4_SYSV_ABI (*)(s32, s32, u32*, u32*, u16*);
using MatchingStatusFn = s32 PS4_SYSV_ABI (*)(u16, u64, u16, s32*, void*, u16*);
using PollControlFn = s32 PS4_SYSV_ABI (*)(s32, s32, s32, BbscAddressed::PollEvent*);
using PollWaitFn = s32 PS4_SYSV_ABI (*)(s32, BbscAddressed::PollEvent*, s32, s32);
BindFn originalBind = nullptr;
CloseFn originalClose = nullptr;
SendFn originalSend = nullptr;
ReceiveFn originalReceive = nullptr;
ActivateFn originalActivate = nullptr;
StatusFn originalStatus = nullptr;
MatchingStatusFn originalMatchingStatus = nullptr;
PollControlFn originalPollControl = nullptr;
PollWaitFn originalPollWait = nullptr;
CloseFn originalPollDestroy = nullptr;
std::mutex mutex;
BbscAddressed::Inbox inbox;
BbscAddressed::Readiness readiness;
struct Endpoint { u32 ip; u16 port; u64 seen; };
std::map<std::string, Endpoint> endpoints;
std::map<std::pair<u64, u16>, std::string> members;
std::map<std::pair<s32, s32>, std::string> connections;
std::map<std::string, u64> requests;
std::set<std::pair<u32, u16>> loggedSends, loggedReceives;
std::set<s32> loggedDelivery, loggedReadiness;
std::atomic<SOCKET> udp{INVALID_SOCKET};
std::atomic<u64> epoch{0}, acknowledged{0};
sockaddr_in server{};

u64 Now() { return GetTickCount64(); }
bool Ready() { return epoch != 0 && Now() - acknowledged.load() < 10000; }
template<class T> void Put(std::vector<u8>& b, T n) {
    for (int i = sizeof(T) - 1; i >= 0; --i) b.push_back(static_cast<u8>(n >> (i * 8)));
}
template<class T> T Read(const u8* b) {
    T n = 0; for (size_t i = 0; i < sizeof(T); ++i) n = (n << 8) | b[i]; return n;
}
std::string Name(const u8* b) {
    size_t n = 0; while (n < 16 && b[n]) ++n;
    return std::string(reinterpret_cast<const char*>(b), n);
}
std::vector<u8> Frame(u8 kind) {
    std::vector<u8> b{'B','B','U','2',kind}; Put(b, epoch.load()); return b;
}
bool Send(const std::vector<u8>& b) {
    const SOCKET s = udp.load();
    return s != INVALID_SOCKET && ::sendto(s, reinterpret_cast<const char*>(b.data()),
        static_cast<int>(b.size()), 0, reinterpret_cast<const sockaddr*>(&server), sizeof(server)) == b.size();
}
bool IsAlias(u32 ip) { return (ntohl(ip) & 0xffff0000U) == 0xc6120000U; }
bool Lookup(const std::string& name, Endpoint& out) { // mutex held
    const auto it = endpoints.find(name);
    if (!Ready() || it == endpoints.end() || Now() - it->second.seen > 5000) return false;
    out = it->second; return true;
}
bool ResolveName(const std::string& name, Endpoint& out) {
    if (name.empty() || !Ready()) return false;
    std::scoped_lock lock(mutex);
    if (Lookup(name, out)) return true;
    if (Now() - requests[name] >= 300) {
        requests[name] = Now(); auto b = Frame(3);
        const size_t begin = b.size(); b.resize(begin + 16, 0);
        std::memcpy(b.data() + begin, name.data(), std::min<size_t>(16, name.size())); Send(b);
    }
    return false;
}
bool ResolveMember(u64 room, u16 member, Endpoint& out) {
    if (!Ready()) return false;
    std::scoped_lock lock(mutex);
    const auto key = std::make_pair(room, member);
    auto it = members.find(key);
    if (it != members.end() && Lookup(it->second, out)) return true;
    const auto request = std::to_string(room) + ":" + std::to_string(member);
    if (Now() - requests[request] >= 300) {
        requests[request] = Now(); auto b = Frame(5); Put(b, room); Put(b, member); Send(b);
    }
    return false;
}
s32 PS4_SYSV_ABI Bind(s32 s, const void* p, u32 size) {
    const s32 rc = originalBind(s, p, size);
    if (rc == 0 && p && size >= sizeof(Address)) {
        Address a; std::memcpy(&a, p, sizeof(a));
        if (a.family == 2 && a.vport) {
            std::scoped_lock lock(mutex); inbox.Bind(s, ntohs(a.vport));
            LOG_INFO(Mod, "Addressed relay: game socket {} bound to vport {}", s, ntohs(a.vport));
        }
    }
    return rc;
}
s32 PS4_SYSV_ABI Close(s32 s) {
    const s32 rc = originalClose(s);
    if (rc == 0) {
        std::scoped_lock lock(mutex); inbox.Close(s); readiness.CloseSocket(s);
        loggedDelivery.erase(s); loggedReadiness.erase(s);
    }
    return rc;
}
s32 PS4_SYSV_ABI PollControl(s32 eid, s32 op, s32 id, BbscAddressed::PollEvent* event) {
    const s32 rc = originalPollControl(eid, op, id, event);
    if (rc == 0) {
        std::scoped_lock lock(mutex); readiness.Control(eid, op, id, event);
        if (inbox.Vport(id)) LOG_INFO(Mod, "Addressed relay: poll {} op {} socket {} events {:#x}",
                                     eid, op, id, event ? event->events : 0);
    }
    return rc;
}
s32 PS4_SYSV_ABI PollDestroy(s32 eid) {
    const s32 rc = originalPollDestroy(eid);
    if (rc == 0) { std::scoped_lock lock(mutex); readiness.Destroy(eid); }
    return rc;
}
s32 PS4_SYSV_ABI PollWait(s32 eid, BbscAddressed::PollEvent* events, s32 capacity, s32 timeout) {
    bool tracked;
    { std::scoped_lock lock(mutex); tracked = readiness.TracksBound(eid, inbox); }
    if (!tracked || !events || capacity <= 0) return originalPollWait(eid, events, capacity, timeout);
    const auto started = std::chrono::steady_clock::now();
    s32 wait = 0;
    for (;;) {
        // A short native wait preserves native errors/abort and also services sockets
        // outside this mod. The mod queue cannot wake Lantern's internal poll object.
        s32 count = originalPollWait(eid, events, capacity, wait);
        if (count < 0) return count;
        {
            std::scoped_lock lock(mutex);
            if (Ready()) count = readiness.Merge(eid, inbox, Now(), events, count, capacity);
            for (s32 i = 0; i < count; ++i) {
                const auto id = static_cast<s32>(events[i].ident);
                if ((events[i].events & 1) && inbox.HasPending(id, Now()) && loggedReadiness.insert(id).second)
                    LOG_INFO(Mod, "Addressed relay: socket {} reported readable to game", id);
            }
        }
        if (count || timeout == 0) return count;
        if (timeout < 0) { wait = 10000; continue; }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (elapsed >= timeout) return 0;
        wait = static_cast<s32>(std::min<s64>(10000, timeout - elapsed));
    }
}
s32 PS4_SYSV_ABI Sendto(s32 s, const void* buf, u64 len, s32 flags, const void* p, u32 size) {
    Address to;
    if (p && size >= sizeof(to)) std::memcpy(&to, p, sizeof(to));
    if (!p || size < sizeof(to) || to.family != 2 || !IsAlias(to.ip))
        return originalSend(s, buf, len, flags, p, size);
    if (!Ready() || !buf || len == 0 || len > 65000 || !to.vport) return -1;
    u16 sourceVport;
    { std::scoped_lock lock(mutex); sourceVport = inbox.Vport(s); }
    if (!sourceVport) return -1;
    auto b = Frame(7); Put(b, ntohl(to.ip)); Put(b, ntohs(to.port));
    Put(b, sourceVport); Put(b, ntohs(to.vport));
    const auto* bytes = static_cast<const u8*>(buf); b.insert(b.end(), bytes, bytes + len);
    if (!Send(b)) return -1;
    {
        std::scoped_lock lock(mutex);
        if (loggedSends.size() < 128 && loggedSends.insert({ntohl(to.ip), ntohs(to.vport)}).second)
            LOG_INFO(Mod, "Addressed relay: first game send to alias {:#x}, vport {} -> {}, bytes {}",
                     ntohl(to.ip), sourceVport, ntohs(to.vport), len);
    }
    return static_cast<s32>(len);
}
s32 PS4_SYSV_ABI Recvfrom(s32 s, void* buf, u64 len, s32 flags, void* p, u32* size) {
    BbscAddressed::Packet packet;
    bool found = false;
    if (buf && len && Ready()) {
        std::scoped_lock lock(mutex); found = inbox.Pop(s, Now(), (flags & 2) != 0, packet);
    }
    if (found) {
        {
            std::scoped_lock lock(mutex);
            if (loggedDelivery.insert(s).second)
                LOG_INFO(Mod, "Addressed relay: first packet delivered to game socket {}, bytes {}", s, packet.data.size());
        }
        const size_t count = std::min<size_t>(len, packet.data.size());
        std::memcpy(buf, packet.data.data(), count);
        if (p && size) {
            Address from; from.ip = htonl(packet.address); from.port = htons(packet.port);
            from.vport = htons(packet.sourceVport);
            std::memcpy(p, &from, std::min<size_t>(*size, sizeof(from))); *size = sizeof(from);
        }
        return static_cast<s32>(count);
    }
    return originalReceive(s, buf, len, flags, p, size);
}
s32 PS4_SYSV_ABI Activate(s32 ctx, const void* id, s32* conn) {
    const s32 rc = originalActivate(ctx, id, conn);
    if (rc == 0 && id && conn) {
        const auto name = Name(static_cast<const u8*>(id));
        { std::scoped_lock lock(mutex); connections[{ctx, *conn}] = name; }
        Endpoint ignored{}; ResolveName(name, ignored);
    }
    return rc;
}
s32 PS4_SYSV_ABI Status(s32 ctx, s32 conn, u32* status, u32* addr, u16* port) {
    u32 nativeAddr = 0; u16 nativePort = 0;
    const s32 rc = originalStatus(ctx, conn, status, &nativeAddr, &nativePort);
    if (addr) *addr = nativeAddr; if (port) *port = nativePort;
    if (rc != 0 || !status || *status != 2) return rc;
    std::string name;
    { std::scoped_lock lock(mutex); auto it = connections.find({ctx, conn});
      if (it != connections.end()) name = it->second; }
    Endpoint peer{};
    // Some game paths query once after the native ESTABLISHED callback. Resolve the
    // alias before returning ACTIVE rather than handing that path a shared endpoint.
    bool resolved = ResolveName(name, peer);
    const auto deadline = Now() + 1000;
    while (!resolved && !name.empty() && Ready() && Now() < deadline) {
        Sleep(2); resolved = ResolveName(name, peer);
    }
    if (resolved) {
        if (addr) *addr = htonl(peer.ip); if (port) *port = htons(peer.port);
    } else { *status = 1; if (addr) *addr = 0; if (port) *port = 0; }
    return rc;
}
s32 PS4_SYSV_ABI MatchingStatus(u16 ctx, u64 room, u16 member, s32* status, void* addr, u16* port) {
    const s32 rc = originalMatchingStatus(ctx, room, member, status, addr, port);
    if (rc != 0 || !status || *status != 2) return rc;
    Endpoint peer{};
    bool resolved = ResolveMember(room, member, peer);
    const auto deadline = Now() + 1000;
    while (!resolved && Ready() && Now() < deadline) {
        Sleep(2); resolved = ResolveMember(room, member, peer);
    }
    if (resolved) {
        const u32 ip = htonl(peer.ip); if (addr) std::memcpy(addr, &ip, 4);
        if (port) *port = htons(peer.port);
    } else { *status = 1; if (addr) std::memset(addr, 0, 4); if (port) *port = 0; }
    return rc;
}
DWORD WINAPI Worker(void*) {
    WSADATA wsa{}; if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return 0;
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { WSACleanup(); return 0; }
    u_long nonblocking = 1; ioctlsocket(s, FIONBIO, &nonblocking); udp = s;
    std::vector<u8> registration{'B','B','U','2',1}; registration.resize(21, 0);
    const char* name = std::getenv("BB_SHADNET_NPID");
    if (name) std::memcpy(registration.data() + 5, name, std::min<size_t>(16, std::strlen(name)));
    u64 nextRegistration = 0; std::vector<u8> b(65024);
    for (;;) {
        const auto now = Now();
        if (now >= nextRegistration) { Send(registration); nextRegistration = now + 2000; }
        for (int i = 0; i < 256; ++i) {
            sockaddr_in from{}; int size = sizeof(from);
            const int n = ::recvfrom(s, reinterpret_cast<char*>(b.data()), static_cast<int>(b.size()), 0,
                                    reinterpret_cast<sockaddr*>(&from), &size);
            if (n < 0) break;
            if (n < 13 || from.sin_addr.s_addr != server.sin_addr.s_addr || from.sin_port != server.sin_port ||
                std::memcmp(b.data(), "BBU2", 4) != 0) continue;
            const u64 serverEpoch = Read<u64>(b.data() + 5);
            if (b[4] == 2 && n == 19 && serverEpoch != 0) {
                if (epoch.exchange(serverEpoch) != serverEpoch) {
                    std::scoped_lock lock(mutex); endpoints.clear(); members.clear(); requests.clear();
                    inbox.ClearPackets(); loggedSends.clear(); loggedReceives.clear();
                    loggedDelivery.clear(); loggedReadiness.clear();
                    LOG_INFO(Mod, "Addressed relay: server session ready");
                }
                acknowledged = now; continue;
            }
            if (!Ready() || serverEpoch != epoch) continue;
            std::scoped_lock lock(mutex);
            if ((b[4] == 4 && n == 35) || (b[4] == 6 && n == 45)) {
                const int offset = b[4] == 4 ? 13 : 23;
                const auto peer = Name(b.data() + offset);
                const u32 alias = Read<u32>(b.data() + offset + 16);
                const u16 port = Read<u16>(b.data() + offset + 20);
                if (peer.empty() || (alias & 0xffff0000U) != 0xc6120000U || port != 31314) continue;
                if (endpoints.find(peer) == endpoints.end()) LOG_INFO(Mod, "Addressed relay: peer {} mapped", peer);
                endpoints[peer] = {alias, port, now};
                if (b[4] == 6) members[{Read<u64>(b.data()+13), Read<u16>(b.data()+21)}] = peer;
            } else if (b[4] == 8 && n > 23 && n <= 65023) {
                BbscAddressed::Packet packet;
                packet.address = Read<u32>(b.data()+13); packet.port = Read<u16>(b.data()+17);
                packet.sourceVport = Read<u16>(b.data()+19); packet.destinationVport = Read<u16>(b.data()+21);
                packet.received = now; packet.data.assign(b.begin()+23, b.begin()+n);
                if ((packet.address & 0xffff0000U) == 0xc6120000U && packet.port == 31314) {
                    if (loggedReceives.size() < 128 && loggedReceives.insert({packet.address, packet.destinationVport}).second)
                        LOG_INFO(Mod, "Addressed relay: first game receive from alias {:#x}, vport {} -> {}, bytes {}",
                                 packet.address, packet.sourceVport, packet.destinationVport, packet.data.size());
                    inbox.Push(std::move(packet));
                }
            }
        }
        Sleep(1);
    }
}
template<class Fn> bool Hook(u64 offset, Fn wrapper, Fn& original, const char* name) {
    for (int i = 0; i < 60000; ++i) {
        const u64 base = BbscMod::EbootAddress(); u64 value = 0;
        if (BbscMod::ReadSlotSafely(base + offset, &value) && value && (value < base || value >= base + 0x056D4000ULL)) {
            original = reinterpret_cast<Fn>(value);
            if (BbscMod::WriteSlotSafely(base + offset, reinterpret_cast<u64>(wrapper))) {
                BbscMod::GuardSlot(offset, reinterpret_cast<u64>(wrapper), reinterpret_cast<void**>(&original), name);
                return true;
            }
        }
        Sleep(1);
    }
    return false;
}
}
namespace BbscMod {
void StartMultiplayerRelay() {
    const char* address = std::getenv("BBSC_GAME_RELAY");
    const char* identity = std::getenv("BB_SHADNET_NPID");
    if (!address || !identity || !*identity) { LOG_ERROR(Mod, "Addressed relay: configuration missing"); return; }
    const std::string value(address); const auto colon = value.rfind(':');
    if (colon == std::string::npos) return;
    char* end = nullptr; const long port = std::strtol(value.c_str()+colon+1, &end, 10);
    server.sin_family = AF_INET; server.sin_port = htons(static_cast<u16>(port));
    if (!end || *end || port < 1 || port > 65535 || inet_pton(AF_INET, value.substr(0,colon).c_str(), &server.sin_addr) != 1) return;
    // Verified CUSA03173 1.09 import slots. These wrap game calls, never Lantern's internal signaling.
    if (!Hook<PollControlFn>(0x053E4AE8, PollControl, originalPollControl, "MultiplayerPollControl") ||
        !Hook<CloseFn>(0x053E4AF8, PollDestroy, originalPollDestroy, "MultiplayerPollDestroy") ||
        !Hook<PollWaitFn>(0x053E4B48, PollWait, originalPollWait, "MultiplayerPollWait") ||
        !Hook<BindFn>(0x053E4B38, Bind, originalBind, "MultiplayerBind") ||
        !Hook<CloseFn>(0x053E4B08, Close, originalClose, "MultiplayerClose") ||
        !Hook<SendFn>(0x053E4F48, Sendto, originalSend, "MultiplayerSend") ||
        !Hook<ReceiveFn>(0x053E4F58, Recvfrom, originalReceive, "MultiplayerReceive") ||
        !Hook<ActivateFn>(0x053E52B0, Activate, originalActivate, "MultiplayerActivate") ||
        !Hook<StatusFn>(0x053E52D0, Status, originalStatus, "MultiplayerStatus") ||
        !Hook<MatchingStatusFn>(0x053E51D0, MatchingStatus, originalMatchingStatus, "MultiplayerMatchingStatus")) {
        LOG_ERROR(Mod, "Addressed relay: import hooks unavailable"); return;
    }
    if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(thread);
    LOG_INFO(Mod, "Addressed multiplayer relay enabled: {} (BBU2)", value);
}
}
