// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Core {
// Reserve trampoline storage before the host reserves its rendering address space.
// This allocates memory only; game code remains untouched until hook installation.
bool PrepareGuestHookArea();
}
