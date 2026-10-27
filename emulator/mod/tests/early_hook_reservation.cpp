// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include "../bbsc_hooks.h"
#include "core/cpu_patches.h"

namespace {
constexpr u64 Base = 0x800000000ULL;
constexpr u64 Reach = 0x70000000ULL;
constexpr u64 ImageSize = 0x056D4000ULL;
}
namespace BbscMod {
void LogPlain(const char* text) { std::cout << text << '\n'; }
u64 EbootAddress() { return Base; }
}
void PS4_SYSV_ABI Observe(u64, const Core::GuestRegisterSnapshot*) {}

int main() {
    auto* image = static_cast<u8*>(VirtualAlloc(reinterpret_cast<void*>(Base), ImageSize,
        MEM_RESERVE, PAGE_NOACCESS));
    assert(image == reinterpret_cast<void*>(Base));
    assert(VirtualAlloc(image, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    const u8 prologue[] = {0x55,0x48,0x89,0xe5,0x41,0x57};
    std::memcpy(image, prologue, sizeof(prologue));
    assert(Core::PrepareGuestHookArea());
    assert(std::memcmp(image, prologue, sizeof(prologue)) == 0); // reservation installs nothing

    // Model the host's later VRAM reservation occupying every remaining nearby address.
    std::vector<void*> reservations;
    const u64 low = Base + ImageSize - Reach;
    const u64 high = Base + Reach;
    for (u64 at = low & ~0xffffULL; at < high;) {
        MEMORY_BASIC_INFORMATION info{};
        assert(VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)));
        const auto end = std::min<u64>(reinterpret_cast<u64>(info.BaseAddress)+info.RegionSize, high);
        if (info.State == MEM_FREE) {
            const u64 start = (at+0xffff)&~0xffffULL;
            if (start < end) {
                void* got = VirtualAlloc(reinterpret_cast<void*>(start), end-start, MEM_RESERVE, PAGE_NOACCESS);
                assert(got); reservations.push_back(got);
            }
        }
        assert(end > at); at = end;
    }
    assert(Core::PrepareGuestHookArea()); // reuse, no second allocation in the now-full window

    // Reproduce the startup fault: native initialization changes initially writable code
    // to RX after trampoline reservation, so the shared seamless installer cannot memcpy it.
    auto* bell_page = image + 0x0157f000;
    assert(VirtualAlloc(bell_page, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    assert(VirtualAlloc(image + 0x1000, 0x1000, MEM_COMMIT, PAGE_READONLY));
    DWORD old = 0;
    assert(VirtualProtect(image, 0x1000, PAGE_EXECUTE_READ, &old));
    assert(VirtualProtect(bell_page, 0x1000, PAGE_EXECUTE_READ, &old));
    MEMORY_BASIC_INFORMATION before{};
    assert(VirtualQuery(image + 0x0157f8cc, &before, sizeof(before)));
    assert(before.Protect == PAGE_EXECUTE_READ);
    assert(Core::PrepareGuestCodeWrites(Base, ImageSize)); // runs after native completion
    MEMORY_BASIC_INFORMATION data{};
    assert(VirtualQuery(image + 0x1000, &data, sizeof(data)));
    assert(data.Protect == PAGE_READONLY); // unrelated data retains its protection
    MEMORY_BASIC_INFORMATION after{};
    assert(VirtualQuery(image + 0x0157f8cc, &after, sizeof(after)));
    assert(after.Protect == PAGE_EXECUTE_READWRITE);
    const u8 patch[] = {0x0f, 0x8f, 0x90, 0x00, 0x00, 0x00};
    std::memcpy(image + 0x0157f8c8, patch, sizeof(patch)); // exact faulting patch range
    assert(std::memcmp(image + 0x0157f8c8, patch, sizeof(patch)) == 0);
    assert(!Core::PrepareGuestCodeWrites(0, ImageSize));
    assert(!Core::PrepareGuestCodeWrites(Base, 0));
    assert(VirtualProtect(bell_page, 0x1000, PAGE_EXECUTE_READ | PAGE_GUARD, &old));
    assert(!Core::PrepareGuestCodeWrites(Base, ImageSize)); // fail closed on guarded code
    assert(VirtualProtect(bell_page, 0x1000, PAGE_EXECUTE_READWRITE, &old));
    std::cout << "LATE_CODE_PERMISSIONS_REGRESSION_OK\n";

    assert(Core::InstallGuestCodeHook(image, prologue, 0, Observe, nullptr));
    assert(image[0] == 0xe9);
    s32 jump = 0; std::memcpy(&jump, image+1, 4);
    MEMORY_BASIC_INFORMATION target{};
    assert(VirtualQuery(image+5+jump, &target, sizeof(target)));
    assert(target.State == MEM_COMMIT && (target.Protect & PAGE_EXECUTE_READWRITE));
    for (void* p : reservations) VirtualFree(p, 0, MEM_RELEASE);
    VirtualFree(image, 0, MEM_RELEASE);
    std::cout << "EARLY_HOOK_RESERVATION_REGRESSION_OK\n";
}
