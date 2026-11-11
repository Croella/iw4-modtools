#include "fastfile.h"
#include <algorithm>
#include <cstring>
#include <miniz.h>

namespace {
constexpr uint64_t kInlinePtr = ~0ull;
constexpr size_t kMaterialSize = 0x78;
constexpr size_t kTexDefSize = 16;
constexpr size_t kZlibOffset = 21;  // magic(8) + version(4) + prefix(9)

// Signed zones (IWffs100): the retail multiplayer maps. After the file header comes a fixed
// DB_AuthHeader (RSA signature + 244 master-block hashes) and the first DB_MasterBlock (256
// SHA-256 chunk hashes), then the compressed data: one zlib stream whose bytes are interrupted
// every 0x200000 by a fresh 0x2000-byte master block (256 chunks of 0x2000 compressed bytes
// each, per block). Stripping the master blocks yields the continuous zlib stream. The hashes
// authenticate the data for the game; the editor only needs to decompress, so they're skipped.
// Layout verified against every signed zone in the install (mp_*, common_mp, patch_mp).
constexpr size_t kSignedDataOffset = 0x4015;    // file header + DB_AuthHeader + first DB_MasterBlock
constexpr size_t kSignedGroupBytes = 0x200000;  // compressed bytes between master blocks (256 * 0x2000)
constexpr size_t kSignedMasterBlock = 0x2000;   // 256 * 32-byte SHA-256 chunk hashes

// Builds the continuous zlib stream from a signed file by concatenating the compressed groups
// and dropping the interleaved master blocks. Returns false only if the file is too small or
// has no zlib stream where the format requires one.
bool stripSignedStream(const std::vector<uint8_t>& file, std::vector<uint8_t>& compressed, std::string& error) {
    // The data offset is fixed by the auth-header struct; tolerate small shifts by scanning a
    // narrow window for the zlib header rather than trusting the constant blindly.
    size_t dataAt = 0;
    for (size_t o = kSignedDataOffset > 0x80 ? kSignedDataOffset - 0x80 : 0;
         o + 1 < file.size() && o <= kSignedDataOffset + 0x80; o++) {
        uint8_t h = file[o], f = file[o + 1];
        if (h == 0x78 && (f == 0x01 || f == 0x9c || f == 0xda || f == 0x5e)) { dataAt = o; break; }
    }
    if (!dataAt) {
        error = "signed fastfile: no zlib stream near the expected offset";
        return false;
    }
    compressed.clear();
    compressed.reserve(file.size() - dataAt);
    for (size_t pos = dataAt; pos < file.size();) {
        size_t n = std::min(kSignedGroupBytes, file.size() - pos);
        compressed.insert(compressed.end(), file.begin() + pos, file.begin() + pos + n);
        pos += n + kSignedMasterBlock;  // skip the master block before the next group (overshoot at EOF is fine)
    }
    return true;
}

bool inflateStream(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out, std::string& error) {
    mz_stream s{};
    if (mz_inflateInit(&s) != MZ_OK) {
        error = "inflate init failed";
        return false;
    }
    s.next_in = src;
    s.avail_in = static_cast<unsigned int>(srcLen);
    out.resize(std::max<size_t>(srcLen * 2, 1024));
    size_t written = 0;
    int status = MZ_OK;
    while (status == MZ_OK) {
        if (written == out.size()) out.resize(out.size() * 2);
        s.next_out = out.data() + written;
        s.avail_out = static_cast<unsigned int>(std::min<size_t>(out.size() - written, 1u << 30));
        size_t before = s.avail_out;
        status = mz_inflate(&s, MZ_NO_FLUSH);
        written += before - s.avail_out;
        if (status == MZ_BUF_ERROR && s.avail_in > 0) status = MZ_OK;  // output buffer was full
    }
    mz_inflateEnd(&s);
    if (status != MZ_STREAM_END) {
        error = std::string("inflate failed: ") + mz_error(status);
        return false;
    }
    out.resize(written);
    return true;
}

uint64_t read64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

bool validNameChar(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '/' || c == '#' || c == '@' || c == '~' || c == '$' || c == '&' || c == '.' ||
           c == ',';
}

// Returns the string length if p starts a plausible asset name, otherwise 0.
size_t nameLength(const uint8_t* p, const uint8_t* end, size_t maxLen) {
    size_t n = 0;
    while (p + n < end && n <= maxLen) {
        if (p[n] == 0) return n >= 2 ? n : 0;
        if (!validNameChar(p[n])) return 0;
        n++;
    }
    return 0;
}

// Finds the next inline image name: eight 0xFF bytes followed by a NUL-terminated name.
const uint8_t* findImageName(const uint8_t* from, const uint8_t* end, size_t window, size_t& len) {
    const uint8_t* stop = from + window;
    if (stop > end - 9) stop = end - 9;
    for (const uint8_t* p = from; p < stop; p++) {
        if (read64(p) != kInlinePtr || p[8] == 0xff) continue;
        len = nameLength(p + 8, end, 200);
        if (len) return p + 8;
    }
    return nullptr;
}
}  // namespace

FastfileKind classifyFastfile(const uint8_t* data, size_t size) {
    if (size < 29) return FastfileKind::Invalid;
    if (std::memcmp(data, "IWffu100", 8) == 0) return FastfileKind::Unsigned;
    if (std::memcmp(data, "IWff0100", 8) == 0 && std::memcmp(data + 21, "IWffs100", 8) == 0)
        return FastfileKind::Signed;
    return FastfileKind::Invalid;
}

bool inflateFastfile(const std::vector<uint8_t>& file, std::vector<uint8_t>& zone, std::string& error) {
    FastfileKind kind = classifyFastfile(file.data(), file.size());
    if (kind == FastfileKind::Signed) {
        std::vector<uint8_t> compressed;
        if (!stripSignedStream(file, compressed, error)) return false;
        return inflateStream(compressed.data(), compressed.size(), zone, error);
    }
    if (kind != FastfileKind::Unsigned) {
        error = "not a fastfile";
        return false;
    }
    return inflateStream(file.data() + kZlibOffset, file.size() - kZlibOffset, zone, error);
}

void defaultPrefix(uint8_t prefix[9]) { std::memset(prefix, 0, 9); }

bool readFastfile(const std::vector<uint8_t>& file, Fastfile& out, std::string& error) {
    FastfileKind kind = classifyFastfile(file.data(), file.size());
    if (kind != FastfileKind::Unsigned && kind != FastfileKind::Signed) {
        error = "not a fastfile";
        return false;
    }
    uint32_t version;
    std::memcpy(&version, file.data() + 8, 4);
    if (version != kFastfileVersion) {
        error = "unexpected fastfile version " + std::to_string(version);
        return false;
    }
    out.signedZone = kind == FastfileKind::Signed;
    std::memcpy(out.prefix, file.data() + 12, 9);
    if (kind == FastfileKind::Signed) {
        std::vector<uint8_t> compressed;
        if (!stripSignedStream(file, compressed, error)) return false;
        return inflateStream(compressed.data(), compressed.size(), out.zone, error);
    }
    return inflateStream(file.data() + kZlibOffset, file.size() - kZlibOffset, out.zone, error);
}

bool writeFastfile(const Fastfile& ff, std::vector<uint8_t>& out, std::string& error) {
    // The game only inflates the payload, so the deflate level is free; 9 keeps files small.
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(ff.zone.size()));
    std::vector<uint8_t> compressed(bound);
    mz_ulong compressedLen = bound;
    int status = mz_compress2(compressed.data(), &compressedLen, ff.zone.data(),
                              static_cast<mz_ulong>(ff.zone.size()), 9);
    if (status != MZ_OK) {
        error = std::string("deflate failed: ") + mz_error(status);
        return false;
    }
    out.clear();
    out.reserve(kZlibOffset + compressedLen);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>("IWffu100"), reinterpret_cast<const uint8_t*>("IWffu100") + 8);
    uint32_t version = kFastfileVersion;
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&version), reinterpret_cast<const uint8_t*>(&version) + 4);
    out.insert(out.end(), ff.prefix, ff.prefix + 9);
    out.insert(out.end(), compressed.data(), compressed.data() + compressedLen);
    return true;
}

bool readZoneHeader(const std::vector<uint8_t>& zone, ZoneHeader& out) {
    if (zone.size() < 72) return false;
    std::memcpy(&out.stringCount, zone.data() + 40, 4);
    std::memcpy(&out.assetCount, zone.data() + 56, 4);
    // An array pointer is inline (-1) when its count is non-zero and null when it is zero (stock
    // code/patch zones carry no script strings). Checking at 64-bit offsets proves the layout.
    auto arrayPtrOk = [&](size_t off, uint32_t count) {
        uint64_t p = read64(zone.data() + off);
        return count ? p == kInlinePtr : (p == 0 || p == kInlinePtr);
    };
    return arrayPtrOk(48, out.stringCount) && arrayPtrOk(64, out.assetCount);
}

void scanMaterials(const std::vector<uint8_t>& zone, std::map<std::string, std::string>& out) {
    if (zone.size() < kMaterialSize + 64) return;
    const uint8_t* base = zone.data();
    const uint8_t* end = base + zone.size();
    for (size_t o = kMaterialSize; o + 32 < zone.size(); o++) {
        // textureTable is cheapest to reject on, and every material we want has one inline.
        if (read64(base + o - 0x18) != kInlinePtr) continue;
        if (read64(base + o - kMaterialSize) != kInlinePtr) continue;
        uint8_t textureCount = base[o - 0x28], constantCount = base[o - 0x27], stateBits = base[o - 0x26];
        if (textureCount < 1 || textureCount > 16 || constantCount > 64 || stateBits < 1 || stateBits > 64)
            continue;
        size_t len = nameLength(base + o, end, 120);
        if (len < 3) continue;
        size_t tableOff = o + len + 1;
        if (tableOff + kTexDefSize * textureCount >= zone.size()) continue;

        bool valid = true;
        for (int i = 0; i < textureCount && valid; i++) {
            const uint8_t* t = base + tableOff + kTexDefSize * i;
            valid = t[6] <= 0x40 && t[7] <= 0x20;
        }
        if (!valid) continue;

        std::string name(reinterpret_cast<const char*>(base + o), len);
        std::string color;
        const uint8_t* cursor = base + tableOff + kTexDefSize * textureCount;
        for (int i = 0; i < textureCount; i++) {
            const uint8_t* t = base + tableOff + kTexDefSize * i;
            bool isColor = t[4] == 'c' && t[5] == 'p';  // "colorMap" by first and last letter
            if (read64(t + 8) != kInlinePtr) continue;
            size_t imageLen = 0;
            const uint8_t* imageName = findImageName(cursor, end, 0x90, imageLen);
            if (!imageName) break;
            if (isColor) {
                const char* s = reinterpret_cast<const char*>(imageName);
                if (imageLen && s[0] == ',') { s++; imageLen--; }  // load-order marker, not part of the file name
                color.assign(s, imageLen);
                break;
            }
            cursor = imageName + imageLen + 1;
        }
        auto it = out.find(name);
        if (it == out.end()) out.emplace(std::move(name), std::move(color));
        else if (it->second.empty()) it->second = std::move(color);
        o = tableOff;  // the name and table can't contain another material
    }
}
