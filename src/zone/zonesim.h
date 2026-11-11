#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// An offline port of the x64 iw4mp zone loader (DB_LoadXFile and the Load_* functions, reversed
// from iw4mp_2013.exe; addresses in zonewriter.h / zonesim.cpp) for the asset types iw4-radiant
// emits. It loads an inflated zone image into real memory exactly as the game would: blocks are
// allocated from XFile.blockSize, every Load_Stream is bounds-checked against its block, and
// every back-reference is resolved and checked. The loaded structs (iw4:: types, see iw4.h)
// hold real pointers, so tests can inspect a clipMap_t as the game sees it.
//
// Fails on: block overrun (blockSize too small), reading past the file, a back-reference out of
// range, unconsumed file bytes, or an asset feature this port doesn't implement.
namespace zone {

struct LoadedAsset {
    uint32_t type;
    const void* header;      // the asset struct after DB_Add* copied it out of TEMP
    bool reference = false;  // ','-named: DB_AddXAsset resolves it to an existing/default asset
};

class SimulatedZone {
public:
    std::vector<LoadedAsset> assets;  // in DB_Add order (nested assets such as MapEnts included)
    std::vector<std::string> scriptStrings;
    uint32_t blockSize[8] = {};
    uint32_t blockUsed[8] = {};  // high-water mark actually reached by the loader

    // First (non-reference) asset of a type, cast by the caller to the iw4:: struct.
    const void* find(uint32_t type) const;
    size_t count(uint32_t type, bool references) const;

    struct Owned;  // implementation-owned memory (blocks + DB_Add pool)
    std::shared_ptr<Owned> memory;
};

bool simulateLoad(const std::vector<uint8_t>& zoneImage, SimulatedZone& out, std::string& error);

// Partial loads, for zones whose other asset types this port can't walk (stock map zones): each
// asset is loaded from its struct's file offset as if it were the next Load_XAsset (header -1).
// Back-references to data outside what was loaded resolve to zero memory (null asset pointers,
// empty strings) instead of failing. The absolute VIRTUAL position is unknown mid-zone, so an
// anchor forces it where a known array is allocated (GfxWorld: dpvsPlanes.planes, clipMap:
// brushsides); everything allocated after the anchor then has its exact in-game position, so
// later back-references (e.g. surface -> material insert slot) resolve correctly.
struct PartialAsset {
    uint32_t type = 0;        // 14/15 clipMap, 19 MapEnts, 21 GfxWorld
    size_t fileOffset = 0;    // where the asset struct starts in the inflated zone
    bool hasAnchor = false;
    uint32_t anchor = 0;      // VIRTUAL offset to force at the anchor array
};
struct PartialResult {
    size_t endOffset = 0;            // file position after the asset (or where it stopped)
    std::string stopped;             // non-empty: stopped early here (unsupported tail data)
    std::vector<uint64_t> rawRefs;   // clipMap: raw brush.sides values, for computing its anchor
    size_t mapEntsOffset = 0;        // clipMap: file offset where its inline MapEnts struct was read
};
bool simulatePartial(const std::vector<uint8_t>& zoneImage, const std::vector<PartialAsset>& assets,
                     SimulatedZone& out, std::vector<PartialResult>& results, std::string& error);

}  // namespace zone
