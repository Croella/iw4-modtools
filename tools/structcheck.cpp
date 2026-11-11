// Compiling this as x64 is the layout guard: iw4_structs.h's static_asserts fire at build time
// if any map-asset struct ever drifts from the validated x64 layout.
#include "game/iw4_structs.h"
#include <cstdio>
int main() {
    std::printf("iw4 x64 struct layout OK: clipMap_t=0x%zx GfxWorld=0x%zx MapEnts=0x%zx Material=0x%zx\n",
                sizeof(clipMap_t), sizeof(GfxWorld), sizeof(MapEnts), sizeof(Material));
    return 0;
}
