#pragma once
// The x64 game structs, namespaced so they can't collide with editor types (Material, Brush...).
#include <cstddef>
#include <cstdint>

// The RE'd header marks some 8-byte-aligned structs align(4) (harmless, MSVC ignores it: C4359).
#pragma warning(push)
#pragma warning(disable : 4359 4324)
namespace iw4 {
#include "game/iw4_structs.h"
}
#pragma warning(pop)

static_assert(sizeof(void*) == 8, "the zone code reads/writes x64 game structs; build it as x64");
