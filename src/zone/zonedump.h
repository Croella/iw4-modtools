#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "zonesim.h"

// Human-readable reports and file dumps for IW4 (x64) fastfiles, shared by the editor's
// File > Dump Fast File, `iw4r_zone dump`, and the compile/verify reports.
namespace zone {

// Asset type names in this build's XAssetType numbering. The world types (5, 9, 10, 14-21, 36)
// are confirmed against the loader switch (0x122e30); the rest follow the IW4 PC list.
const char* assetTypeName(uint32_t type);

// Multi-line summary of a simulated load: block sizes/usage, asset counts, world statistics.
std::string describeSimulatedZone(const SimulatedZone& sim);

struct DumpResult {
    std::string folder;              // where everything was written
    std::vector<std::string> files;  // paths written, relative to folder
    std::string summary;             // short text for a status line / dialog
    bool simulated = false;          // the loader simulator walked the whole zone
};

// Dumps <ffPath> into <outRoot>/<stem>/:
//   <stem>.zone        the inflated zone image (unsigned IWffu100 files only)
//   report.txt         file header, XFile/blockSize, asset list, simulation result
//   files/<name>       every rawfile (decompressed), when the simulator can walk the zone
//   entities.txt       the map entity string, when present
// Signed (IWffs100) zones can't be inflated; the report still records the header.
bool dumpFastfile(const std::string& ffPath, const std::string& outRoot, DumpResult& out, std::string& error);

}  // namespace zone
