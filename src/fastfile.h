#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Fastfile support for the zones in the x64 install. These are version 276 but use 64-bit
// pointers on disk, so 32-bit layouts from older references don't apply. Both kinds decompress
// to the same zone payload:
//   'IWffu100' at offset 0: unsigned, one zlib stream from offset 21 (singleplayer / Spec Ops).
//   'IWff0100' + 'IWffs100' at 21: signed, the retail multiplayer maps. A DB_AuthHeader and
//     DB_MasterBlock precede the data, and 0x2000-byte master blocks (SHA-256 chunk hashes) are
//     interleaved into the zlib stream every 0x200000 bytes; stripping them recovers the stream.
enum class FastfileKind { Unsigned, Signed, Invalid };

FastfileKind classifyFastfile(const uint8_t* data, size_t size);
bool inflateFastfile(const std::vector<uint8_t>& file, std::vector<uint8_t>& zone, std::string& error);

// An unsigned fastfile is "IWffu100" + u32 version (276) + 9 opaque prefix bytes + one zlib
// stream. The 9 bytes are a flag (01 in stock zones, 00 in the client's own custom zone) plus
// a build stamp and a per-file value; the client's loader doesn't require meaningful values
// there. Reader and writer keep them verbatim so a rebuild is faithful, and a from-scratch
// build can use defaultPrefix().
constexpr uint32_t kFastfileVersion = 276;

struct Fastfile {
    uint8_t prefix[9] = {0};    // on-disk bytes 12..20, between the version and the zlib stream
    bool signedZone = false;    // true for IWffs100 (retail MP); writeFastfile only emits unsigned
    std::vector<uint8_t> zone;  // the inflated zone payload
};

// Matches the prefix the client writes for its own zone: flag 0, zeroed stamp and tail.
void defaultPrefix(uint8_t prefix[9]);

bool readFastfile(const std::vector<uint8_t>& file, Fastfile& out, std::string& error);
bool writeFastfile(const Fastfile& ff, std::vector<uint8_t>& out, std::string& error);

struct ZoneHeader {
    uint32_t stringCount = 0;
    uint32_t assetCount = 0;
};
bool readZoneHeader(const std::vector<uint8_t>& zone, ZoneHeader& out);

// Maps material asset name (including any "wc/" prefix) to the colorMap image name, or to an
// empty string when the image is a back-reference we can't resolve without a full zone walk.
//
// This scans for the on-disk Material pattern rather than loading the zone in order, because
// walking it properly needs a reader for every asset type. x64 Material layout (0x78 bytes):
//   0x00 name ptr, 0x50 textureCount, 0x51 constantCount, 0x52 stateBitsCount,
//   0x58 techniqueSet, 0x60 textureTable, 0x68 constantTable, 0x70 stateBitsTable
// followed by the name, then 16-byte MaterialTextureDef entries.
void scanMaterials(const std::vector<uint8_t>& zone, std::map<std::string, std::string>& out);
