// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace Core {
// Reserve trampoline storage before the host reserves its rendering address space.
// This allocates memory only; game code remains untouched until hook installation.
bool PrepareGuestHookArea();
// Call after native initialization: the host may finalize its code pages as RX.
// Shared game patch installers require writable executable pages; leave data alone.
bool PrepareGuestCodeWrites(std::uintptr_t image, std::size_t size);
}
