// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../src/core/bbsc_respawn_state.h"
#include "../lantern_cheats.h"
#include <array>
#include <cassert>
#include <iostream>
#include <vector>
#include <fstream>
#include <iterator>
#include <string>

using namespace Core::Bbsc::Respawn;
template<class T> T Read(const std::uint8_t* p, std::size_t offset) {
    T value; std::memcpy(&value, p + offset, sizeof(value)); return value;
}
int main(int argc, char** argv) {
    if (argc == 5) {
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        assert(!bytes.empty());
        const auto found = BbscMod::Lantern::CheatGate(bytes, std::stoull(argv[2], nullptr, 0),
                                                     std::stoull(argv[3], nullptr, 0));
        assert(found && *found == std::stoull(argv[4], nullptr, 0));
        std::cout << "INSTALLED_LANTERN_CHEAT_GATE_RECOGNIZED\n";
        return 0;
    }
    // Actual ordering: guest HP=0 at 20:59:19, host returns at :25, final death step at :28.
    DeathCycle cycle;
    cycle.Observe(3, true);
    cycle.Observe(4, false);
    cycle.Observe(4, false);
    assert(cycle.saw_wipe && cycle.revivals_at_start == 3);
    cycle.Reset();
    cycle.Observe(4, false);
    assert(!cycle.saw_wipe); // a normal boss death still spectates a living partner
    cycle.Observe(5, false);
    assert(cycle.saw_wipe); // revival during the death animation cannot get lost

    assert(ReportDead(true, false, -1, true)); // absent character is not a resurrection
    assert(ReportDead(true, false, 573, true)); // loading character is not ready
    assert(ReportDead(false, true, 573, false)); // spectator keeps its death report
    assert(!ReportDead(true, false, 573, false));
    assert(!CoordinatesUsable(0x15000000, 0x18010000, true)); // stale catalogue entry
    assert(CoordinatesUsable(0x18000000, 0x18010000, true)); // streamed neighbouring block
    assert(!CoordinatesUsable(0x18000000, 0x18010000, false));
    const std::array<EntranceSample, 4> trail = {{
        {1000, {0x15000000, 99}}, {5000, {0x18010000, 10}},
        {6000, {0x18010000, 12}}, {9500, {0x18010000, 20}}}};
    const auto entrance = EntranceBeforeFight(trail, 10000, 0x18010000);
    assert(entrance && entrance->x == 12); // before fog traversal, not the arena's first frame
    assert(!EntranceBeforeFight(trail, 30000, 0x18010000)); // stale/other-map samples cannot be used

    // The observed wrong task: Dream + home lamp 2102961, death type 4/subtype 2.
    std::array<std::uint8_t, 0xa0> descriptor{};
    const std::int32_t dream = 0x15000000, lamp = 2102961, death_type = 4;
    std::memcpy(descriptor.data()+8, &dream, 4);
    std::memcpy(descriptor.data()+0x54, &death_type, 4);
    std::memcpy(descriptor.data()+0x80, &lamp, 4);
    descriptor[0x94] = 0x5a;
    assert(PinDescriptor(descriptor, {0x18010000, -123.8f, -27.01f, 64.3f, 1.5f}));
    assert(Read<std::uint32_t>(descriptor.data(), 8) == 0x18010000);
    assert(Read<int>(descriptor.data(), 0x54) == 2 && Read<int>(descriptor.data(), 0x58) == 1);
    assert(Read<int>(descriptor.data(), 0x80) == -1);
    assert(Read<float>(descriptor.data(), 0x60) == -123.8f && descriptor[0x94] == 0x5a);
    assert(!PinDescriptor(std::span{descriptor}.first(16), {0x18010000}));

    // Native predicate with relocated strings; unrelated BB_ONLINE checks must not match.
    std::vector<std::uint8_t> image(1024);
    const std::array<std::uint8_t, 56> code = {
        0x55,0x48,0x83,0xec,0x20,0x48,0x8d,0x6c,0x24,0x20,0x48,0x8d,0x0d,0,0,0,0,
        0xe8,0,0,0,0,0x48,0x85,0xc0,0x74,0x15,0x80,0x38,0x31,0x75,0x10,0x48,0x8d,0x0d,
        0,0,0,0,0xe8,0,0,0,0,0xb0,0x01,0xeb,0x02,0x31,0xc0,0x48,0x83,0xc4,0x20,0x5d,0xc3};
    auto place = [&](std::size_t at) {
        std::memcpy(image.data()+at, code.data(), code.size());
        const std::int32_t a = 700 - (at+17), b = 750 - (at+39);
        std::memcpy(image.data()+at+13, &a, 4); std::memcpy(image.data()+at+35, &b, 4);
    };
    std::memcpy(image.data()+700, "BB_ONLINE", sizeof("BB_ONLINE"));
    std::memcpy(image.data()+750, "Cheats: off while playing online", sizeof("Cheats: off while playing online"));
    place(32);
    assert(BbscMod::Lantern::CheatGate(image, 0, 600) == 32);
    image[700] = 'X';
    assert(!BbscMod::Lantern::CheatGate(image, 0, 600));
    image[700] = 'B'; place(200);
    assert(!BbscMod::Lantern::CheatGate(image, 0, 600)); // ambiguous code remains untouched
    assert(!BbscMod::Lantern::CheatGate(image, 0, 2048));
    std::cout << "RESPAWN_AND_LANTERN_REGRESSIONS_OK\n";
}
