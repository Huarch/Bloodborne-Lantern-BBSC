#include "../bbsc_multiplayer_queue.h"
#include <cstdlib>
#include <iostream>
using namespace BbscAddressed;
static void Check(bool condition, const char* label) {
    if (!condition) { std::cerr << label << '\n'; std::exit(1); }
}
int main() {
    Inbox inbox; Readiness ready;
    inbox.Bind(2, 40); inbox.Bind(3, 30);
    PollEvent interest{1, 0, 999, 0x123456789ULL};
    ready.Control(8, 1, 2, &interest);
    ready.Control(8, 1, 3, &interest);
    Check(ready.TracksBound(8, inbox), "poll tracks game sockets");
    PollEvent events[3]{};
    Check(ready.Merge(8, inbox, 100, events, 0, 3) == 0, "empty queue does not wake game");
    Packet packet; packet.destinationVport = 40; packet.sourceVport = 40;
    packet.received = 100; packet.data.resize(15, 7); inbox.Push(packet);
    // The real regression: no native packet exists, but the game must call recvfrom.
    int count = ready.Merge(8, inbox, 101, events, 0, 3);
    Check(count == 1 && events[0].ident == 2 && events[0].events == 1 &&
          events[0].data == interest.data, "first relay packet makes correct socket readable");
    Packet received;
    Check(inbox.Pop(2, 101, true, received), "readiness leaves packet queued");
    Check(ready.Merge(8, inbox, 101, events, 0, 3) == 1, "peek keeps readiness");
    Check(inbox.Pop(2, 101, false, received) && received.data.size() == 15, "game receives first packet");
    Check(ready.Merge(8, inbox, 101, events, 0, 3) == 0, "consumed data clears readiness");
    inbox.Push(packet);
    events[0] = {2, 0, 2, interest.data};
    Check(ready.Merge(8, inbox, 101, events, 1, 1) == 1 && events[0].events == 3,
          "native writable and relay readable merge even when capacity is full");
    events[0] = {8, 0, 99, 0x888}; events[1] = {0xfeed, 0, 0, 0};
    Check(ready.Merge(8, inbox, 101, events, 1, 1) == 1 && events[1].events == 0xfeed,
          "unrelated native event retained without buffer overrun");
    Check(ready.Merge(9, inbox, 101, events, 0, 3) == 0, "other poll cannot see packet");
    PollEvent writeOnly{2, 0, 0, 0x999}; ready.Control(8, 2, 2, &writeOnly);
    Check(ready.Merge(8, inbox, 101, events, 0, 3) == 0, "modified write-only interest suppresses read");
    ready.Control(8, 2, 2, &interest);
    Check(ready.Merge(8, inbox, 4000, events, 0, 3) == 0, "expired data cannot wake game");
    packet.received = 4000; inbox.Push(packet);
    ready.Control(8, 3, 2, nullptr);
    Check(ready.Merge(8, inbox, 4001, events, 0, 3) == 0, "deleted interest stays silent");
    ready.Control(8, 1, 2, &interest);
    ready.CloseSocket(2); inbox.Close(2); inbox.Bind(2, 40); inbox.Push(packet);
    Check(ready.Merge(8, inbox, 4001, events, 0, 3) == 0, "recycled socket has no stale subscription");
    ready.Control(8, 1, 2, &interest); ready.Destroy(8);
    Check(!ready.TracksBound(8, inbox) && ready.Merge(8, inbox, 4001, events, 0, 3) == 0,
          "destroyed poll has no stale subscriptions");
    std::cout << "MULTIPLAYER_GAME_READINESS_REGRESSION_OK\n";
}
