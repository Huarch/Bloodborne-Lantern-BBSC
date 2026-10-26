// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "shadnet.pb.h"

namespace {

constexpr std::size_t HeaderSize = 15;
constexpr std::uint8_t PacketRequest = 0;
constexpr std::uint8_t PacketReply = 1;
constexpr std::uint8_t PacketNotification = 2;
constexpr std::uint8_t PacketServerInfo = 3;
constexpr std::uint16_t CommandLogin = 0;
constexpr std::uint16_t CommandContextStart = 100;
constexpr std::uint16_t CommandJoinRoom = 102;
constexpr std::uint16_t CommandSearchRoom = 104;
constexpr std::uint16_t CommandRequestSignalingInfos = 105;
constexpr std::uint16_t NotificationRoomEvent = 10;
constexpr std::uint16_t SignalingVport = 0xffff;
constexpr std::uint16_t GuestUdpPort = 36581;

struct Packet {
    std::uint8_t type = 0;
    std::uint16_t command = 0;
    std::uint64_t id = 0;
    std::vector<std::uint8_t> payload;
};

template <typename T>
void AppendLittle(std::vector<std::uint8_t>& output, T value) {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
    }
}

template <typename T>
T ReadLittle(const std::uint8_t* input) {
    T value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(input[index]) << (index * 8);
    }
    return value;
}

bool SendAll(int socket, const std::vector<std::uint8_t>& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result =
            send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool ReceiveAll(int socket, void* output, std::size_t size, int timeoutMs) {
    auto* bytes = static_cast<std::uint8_t*>(output);
    std::size_t received = 0;
    while (received < size) {
        pollfd descriptor{socket, POLLIN, 0};
        if (poll(&descriptor, 1, timeoutMs) <= 0) {
            return false;
        }
        const ssize_t result = recv(socket, bytes + received, size - received, 0);
        if (result <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(result);
    }
    return true;
}

std::optional<Packet> ReceivePacket(int socket, int timeoutMs) {
    std::uint8_t header[HeaderSize]{};
    if (!ReceiveAll(socket, header, sizeof(header), timeoutMs)) {
        return std::nullopt;
    }
    const std::uint32_t totalSize = ReadLittle<std::uint32_t>(header + 3);
    if (totalSize < HeaderSize || totalSize > 8 * 1024 * 1024) {
        return std::nullopt;
    }
    Packet packet;
    packet.type = header[0];
    packet.command = ReadLittle<std::uint16_t>(header + 1);
    packet.id = ReadLittle<std::uint64_t>(header + 7);
    packet.payload.resize(totalSize - HeaderSize);
    if (!packet.payload.empty() &&
        !ReceiveAll(socket, packet.payload.data(), packet.payload.size(), timeoutMs)) {
        return std::nullopt;
    }
    return packet;
}

template <typename Message>
std::vector<std::uint8_t> EncodeProto(const Message& message) {
    std::string encoded;
    if (!message.SerializeToString(&encoded)) {
        return {};
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(4 + encoded.size());
    AppendLittle<std::uint32_t>(payload, static_cast<std::uint32_t>(encoded.size()));
    payload.insert(payload.end(), encoded.begin(), encoded.end());
    return payload;
}

std::vector<std::uint8_t> BuildRequest(std::uint16_t command, std::uint64_t id,
                                       const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> packet;
    packet.reserve(HeaderSize + payload.size());
    packet.push_back(PacketRequest);
    AppendLittle<std::uint16_t>(packet, command);
    AppendLittle<std::uint32_t>(packet,
                                static_cast<std::uint32_t>(HeaderSize + payload.size()));
    AppendLittle<std::uint64_t>(packet, id);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

template <typename Message>
bool ParseReplyProto(const Packet& packet, Message& message) {
    if (packet.payload.size() < 5 || packet.payload[0] != 0) {
        return false;
    }
    const std::uint32_t size = ReadLittle<std::uint32_t>(packet.payload.data() + 1);
    return packet.payload.size() >= 5 + size &&
           message.ParseFromArray(packet.payload.data() + 5, static_cast<int>(size));
}

void PrintNotification(const Packet& packet) {
    if (packet.command != NotificationRoomEvent || packet.payload.size() < 4) {
        return;
    }
    const std::uint32_t size = ReadLittle<std::uint32_t>(packet.payload.data());
    if (packet.payload.size() < 4 + size) {
        return;
    }
    shadnet::NotifyRoomEvent event;
    if (!event.ParseFromArray(packet.payload.data() + 4, static_cast<int>(size))) {
        return;
    }
    std::printf("[guest] room event=0x%x room=%llu member=%u npid=%s\n", event.event(),
                static_cast<unsigned long long>(event.room_id()), event.member().member_id(),
                event.member().npid().c_str());
}

std::optional<Packet> Request(int socket, std::uint16_t command, std::uint64_t id,
                              const std::vector<std::uint8_t>& payload, int timeoutMs) {
    if (!SendAll(socket, BuildRequest(command, id, payload))) {
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto packet = ReceivePacket(socket, 500);
        if (!packet) {
            continue;
        }
        if (packet->type == PacketNotification) {
            PrintNotification(*packet);
            continue;
        }
        if (packet->type == PacketReply && packet->id == id) {
            return packet;
        }
    }
    return std::nullopt;
}

void WriteLittle(std::uint8_t* output, std::uint64_t value, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

void SendStunPing(int socket, const sockaddr_in& server, const std::string& npid) {
    std::uint8_t packet[4 + 21]{};
    std::memset(packet, 0xff, 4);
    packet[4] = 1;
    std::memcpy(packet + 5, npid.data(), std::min<std::size_t>(16, npid.size()));
    sendto(socket, packet, sizeof(packet), 0, reinterpret_cast<const sockaddr*>(&server),
           sizeof(server));
}

void SendHandshakeResponse(int socket, const sockaddr_in& destination,
                           const std::uint8_t* incoming, std::uint8_t responseKind,
                           const std::string& npid) {
    std::uint8_t packet[4 + 0x32]{};
    std::memset(packet, 0xff, 4);
    std::memcpy(packet + 4, incoming, 0x32);
    packet[4 + 5] = responseKind;
    const std::uint16_t fromMember = ReadLittle<std::uint16_t>(incoming + 14);
    const std::uint16_t toMember = ReadLittle<std::uint16_t>(incoming + 16);
    WriteLittle(packet + 4 + 14, toMember, 2);
    WriteLittle(packet + 4 + 16, fromMember, 2);
    std::memset(packet + 4 + 18, 0, 16);
    std::memcpy(packet + 4 + 18, npid.data(), std::min<std::size_t>(16, npid.size()));
    const std::uint16_t mappedPort = htons(GuestUdpPort);
    std::memcpy(packet + 4 + 38, &mappedPort, sizeof(mappedPort));
    sendto(socket, packet, sizeof(packet), 0, reinterpret_cast<const sockaddr*>(&destination),
           sizeof(destination));
}

void UdpWorker(std::atomic<bool>& stop, const std::string& serverIp, const std::string& npid) {
    const int socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket < 0) {
        std::perror("guest UDP socket");
        return;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(GuestUdpPort);
    if (bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        std::perror("guest UDP bind");
        close(socket);
        return;
    }
    sockaddr_in stun{};
    stun.sin_family = AF_INET;
    stun.sin_port = htons(31314);
    inet_pton(AF_INET, serverIp.c_str(), &stun.sin_addr);

    auto nextPing = std::chrono::steady_clock::time_point{};
    while (!stop.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (nextPing.time_since_epoch().count() == 0 || now >= nextPing) {
            SendStunPing(socket, stun, npid);
            nextPing = now + std::chrono::seconds(2);
        }
        pollfd descriptor{socket, POLLIN, 0};
        if (poll(&descriptor, 1, 100) <= 0) {
            continue;
        }
        std::uint8_t packet[2048]{};
        sockaddr_in from{};
        socklen_t fromSize = sizeof(from);
        const ssize_t size = recvfrom(socket, packet, sizeof(packet), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromSize);
        if (size != 4 + 0x32 || std::memcmp(packet, "\xff\xff\xff\xff", 4) != 0 ||
            std::memcmp(packet + 4, "SHAD", 4) != 0 || packet[4 + 4] != 0x21) {
            continue;
        }
        const std::uint8_t kind = packet[4 + 5];
        if (kind == 1) {
            std::printf("[guest] received Offer; sending Accept\n");
            SendHandshakeResponse(socket, from, packet + 4, 2, npid);
        } else if (kind == 3) {
            std::printf("[guest] received Check; sending CheckAck\n");
            SendHandshakeResponse(socket, from, packet + 4, 4, npid);
        }
    }
    close(socket);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <server-ip> <npid>\n", argv[0]);
        return 2;
    }
    const char* password = std::getenv("SHADNET_GUEST_PASSWORD");
    if (!password || !*password) {
        std::fprintf(stderr, "SHADNET_GUEST_PASSWORD is required\n");
        return 2;
    }
    const std::string serverIp = argv[1];
    const std::string npid = argv[2];

    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(31313);
    if (socket < 0 || inet_pton(AF_INET, serverIp.c_str(), &server.sin_addr) != 1 ||
        connect(socket, reinterpret_cast<const sockaddr*>(&server), sizeof(server)) != 0) {
        std::perror("guest TCP connect");
        return 1;
    }
    const auto serverInfo = ReceivePacket(socket, 5000);
    if (!serverInfo || serverInfo->type != PacketServerInfo) {
        std::fprintf(stderr, "guest did not receive ServerInfo\n");
        return 1;
    }

    std::uint64_t packetId = 1;
    shadnet::LoginRequest login;
    login.set_npid(npid);
    login.set_password(password);
    login.set_title_id("CUSA03173");
    login.set_title_name("Bloodborne");
    auto reply = Request(socket, CommandLogin, packetId++, EncodeProto(login), 5000);
    shadnet::LoginReply loginReply;
    if (!reply || !ParseReplyProto(*reply, loginReply)) {
        std::fprintf(stderr, "guest login failed\n");
        return 1;
    }
    std::printf("[guest] login OK user=%llu\n",
                static_cast<unsigned long long>(loginReply.user_id()));

    shadnet::ContextStartRequest context;
    context.set_ctx_id(2);
    reply = Request(socket, CommandContextStart, packetId++, EncodeProto(context), 5000);
    if (!reply || reply->payload.empty() || reply->payload[0] != 0) {
        std::fprintf(stderr, "guest ContextStart failed\n");
        return 1;
    }
    std::printf("[guest] context 2 started\n");

    std::atomic<bool> stopUdp{false};
    std::thread udpThread(UdpWorker, std::ref(stopUdp), serverIp, npid);

    std::uint64_t roomId = 0;
    const auto searchDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < searchDeadline && roomId == 0) {
        shadnet::SearchRoomRequest search;
        search.set_world_id(1);
        search.set_range_filter_start(1);
        search.set_range_filter_max(20);
        reply = Request(socket, CommandSearchRoom, packetId++, EncodeProto(search), 3000);
        shadnet::SearchRoomReply searchReply;
        if (reply && ParseReplyProto(*reply, searchReply) && searchReply.rooms_size() > 0) {
            roomId = searchReply.rooms(0).room_id();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (roomId == 0) {
        std::fprintf(stderr, "guest timed out waiting for a Bloodborne room\n");
        stopUdp = true;
        udpThread.join();
        return 1;
    }
    std::printf("[guest] found room=%llu\n", static_cast<unsigned long long>(roomId));

    shadnet::JoinRoomRequest join;
    join.set_room_id(roomId);
    join.set_req_id(1);
    reply = Request(socket, CommandJoinRoom, packetId++, EncodeProto(join), 5000);
    shadnet::JoinRoomReply joinReply;
    if (!reply || !ParseReplyProto(*reply, joinReply)) {
        std::fprintf(stderr, "guest JoinRoom failed\n");
        stopUdp = true;
        udpThread.join();
        return 1;
    }
    std::printf("[guest] joined room=%llu member=%u members=%u\n",
                static_cast<unsigned long long>(joinReply.room_id()), joinReply.member_id(),
                joinReply.cur_members());

    shadnet::RequestSignalingInfosRequest signaling;
    signaling.set_target_npid("wozzardman");
    reply = Request(socket, CommandRequestSignalingInfos, packetId++, EncodeProto(signaling), 5000);
    shadnet::RequestSignalingInfosReply signalingReply;
    if (reply && ParseReplyProto(*reply, signalingReply)) {
        std::printf("[guest] host endpoint=%s:%u member=%u\n",
                    signalingReply.target_ip().c_str(), signalingReply.target_port(),
                    signalingReply.target_member_id());
    }

    const auto holdDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < holdDeadline) {
        const auto packet = ReceivePacket(socket, 250);
        if (packet && packet->type == PacketNotification) {
            PrintNotification(*packet);
        }
    }

    stopUdp = true;
    udpThread.join();
    close(socket);
    std::printf("[guest] probe complete\n");
    return 0;
}