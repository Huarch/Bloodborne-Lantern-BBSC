// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <optional>

namespace Core::Bbsc::Respawn {
struct DeathCycle {
    bool active = false;
    bool saw_wipe = false;
    std::uint32_t revivals_at_start = 0;
    void Observe(std::uint32_t revivals, bool all_partners_dead) {
        if (!active) {
            active = true;
            revivals_at_start = revivals;
        }
        saw_wipe |= all_partners_dead || revivals != revivals_at_start;
    }
    void Reset() { *this = {}; }
};

// A catalogue entry for another stage is not proof of streamed geometry/collision.
inline bool CoordinatesUsable(std::uint32_t here, std::uint32_t there, bool resident) {
    return here && there && (here == there || ((here >> 24) == (there >> 24) && resident));
}
inline bool ReportDead(bool previous, bool spectating, int hp, bool loading) {
    if (spectating || hp == 0) return true;
    if (loading || hp < 0) return previous;
    return false;
}

struct Destination {
    std::uint32_t map = 0;
    float x = 0, y = 0, z = 0, heading = 0;
};
struct EntranceSample { std::int64_t ms; Destination to; };
inline std::optional<Destination> EntranceBeforeFight(std::span<const EntranceSample> samples,
                                                     std::int64_t now, std::uint32_t map) {
    // The fog traversal animation precedes the boss gauge. Retain the last position before
    // that animation, rather than the first gauge frame, which is already inside the arena.
    std::optional<Destination> best;
    for (const auto& sample : samples) {
        const auto age = now - sample.ms;
        if (sample.to.map == map && age >= 4'000 && age <= 12'000) best = sample.to;
    }
    return best;
}

// CUSA03173 1.09 StageFlow descriptor: absolute summoned placement (type 2/subtype 1).
// A retained death flag otherwise selects type 4/subtype 2 and the guest's own last lamp.
inline bool PinDescriptor(std::span<std::uint8_t> bytes, const Destination& to) {
    if (bytes.size() < 0x88 || to.map == 0) return false;
    auto put = [&](std::size_t offset, const auto& value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    const std::int32_t type = 2, subtype = 1, no_lamp = -1;
    const float position[4] = {to.x, to.y, to.z, 1};
    const float rotation[4] = {0, to.heading, 0, 0};
    put(8, to.map); put(0x54, type); put(0x58, subtype);
    put(0x60, position); put(0x70, rotation);
    put(0x80, no_lamp);
    return true;
}
} // namespace Core::Bbsc::Respawn
