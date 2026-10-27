// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace BbscMod::Lantern {
// Locate only the small native SharedWorld initializer, including both referenced strings.
// Relative addresses may change when Lantern regenerates its probe. Unknown/ambiguous code
// is left untouched. No file is replaced and no address is hard-coded.
inline std::optional<std::size_t> CheatGate(std::span<const std::uint8_t> image,
                                           std::size_t begin, std::size_t end) {
    if (end > image.size() || begin > end || end - begin < 56) return {};
    std::optional<std::size_t> found;
    const auto refers = [&](std::size_t at, std::string_view text) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, image.data() + at + 3, 4);
        const auto target = static_cast<std::int64_t>(at + 7) + displacement;
        return target >= 0 && static_cast<std::uint64_t>(target) < image.size() &&
               text.size() + 1 <= image.size() - static_cast<std::size_t>(target) &&
               std::memcmp(image.data() + target, text.data(), text.size()) == 0 &&
               image[target + text.size()] == 0;
    };
    constexpr std::uint8_t prologue[] = {0x55,0x48,0x83,0xec,0x20,0x48,0x8d,0x6c,0x24,0x20,0x48,0x8d,0x0d};
    constexpr std::uint8_t check[] = {0x48,0x85,0xc0,0x74,0x15,0x80,0x38,0x31,0x75,0x10,0x48,0x8d,0x0d};
    constexpr std::uint8_t tail[] = {0xb0,0x01,0xeb,0x02,0x31,0xc0,0x48,0x83,0xc4,0x20,0x5d,0xc3};
    for (auto at = begin; at + 56 <= end; ++at) {
        if (std::memcmp(image.data()+at, prologue, sizeof(prologue)) ||
            image[at+17] != 0xe8 || image[at+39] != 0xe8 ||
            std::memcmp(image.data()+at+22, check, sizeof(check)) ||
            std::memcmp(image.data()+at+44, tail, sizeof(tail)) ||
            !refers(at+10, "BB_ONLINE") ||
            !refers(at+32, "Cheats: off while playing online")) continue;
        if (found) return {}; // several matches: refuse to guess
        found = at;
    }
    return found;
}
} // namespace BbscMod::Lantern
