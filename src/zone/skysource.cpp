#include "zone/skysource.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "fastfile.h"

namespace sky {
namespace {

constexpr uint64_t kFollow = ~0ull, kInsert = ~1ull;
constexpr uint32_t kCacheMagic = 0x31594B53;  // "SKY1"

uint64_t rd64(const std::vector<uint8_t>& d, size_t o) {
    uint64_t v = 0;
    if (o + 8 <= d.size()) std::memcpy(&v, d.data() + o, 8);
    return v;
}
uint16_t rd16(const std::vector<uint8_t>& d, size_t o) {
    uint16_t v = 0;
    if (o + 2 <= d.size()) std::memcpy(&v, d.data() + o, 2);
    return v;
}
size_t find(const std::vector<uint8_t>& d, const std::string& s, size_t from = 0) {
    if (s.size() > d.size()) return std::string::npos;
    for (size_t i = from; i + s.size() <= d.size(); i++)
        if (d[i] == static_cast<uint8_t>(s[0]) && std::memcmp(d.data() + i, s.data(), s.size()) == 0) return i;
    return std::string::npos;
}
std::vector<uint8_t> slice(const std::vector<uint8_t>& d, size_t o, size_t n) {
    if (o + n > d.size()) return {};
    return {d.begin() + static_cast<std::ptrdiff_t>(o), d.begin() + static_cast<std::ptrdiff_t>(o + n)};
}

// Serialised cache: magic, then each blob as u32 length + bytes, in a fixed order.
std::vector<std::vector<uint8_t>*> blobs(Source& s) {
    std::vector<std::vector<uint8_t>*> b = {&s.technique, &s.pass,     &s.vs.header, &s.vs.code,  &s.ps.header,
                                            &s.ps.code,   &s.args,     &s.techset,   &s.material, &s.textureDef,
                                            &s.stateBits, &s.image,    &s.imageLoadDef};
    return b;
}

}  // namespace

bool extract(const std::vector<uint8_t>& d, Source& out, std::string& error) {
    out = Source{};
    // Technique set: 0x198 struct (name = -1) immediately followed by its name.
    size_t nameAt = find(d, std::string("wc_sky", 7));
    if (nameAt == std::string::npos || nameAt < 0x198 || rd64(d, nameAt - 0x198) != kFollow) {
        error = "no wc_sky technique set in the zone";
        return false;
    }
    const size_t ts = nameAt - 0x198;
    out.techset = slice(d, ts, 0x198);
    size_t pos = nameAt + 7;
    auto xstring = [&](uint64_t ptr) {
        if (ptr != kFollow && ptr != kInsert) return;
        while (pos < d.size() && d[pos]) pos++;
        pos++;
    };
    // Techniques load in slot order; only slot 4 is kept, but the earlier inline ones must be
    // walked to find where it starts.
    for (int t = 0; t < 48; t++) {
        uint64_t f = rd64(d, ts + 24 + 8 * t);
        if (f != kFollow) continue;
        size_t tech = pos;
        pos += 16;
        uint16_t passes = rd16(d, tech + 10);
        size_t passAt = pos;
        pos += 40ull * passes;
        std::vector<uint8_t> args;
        std::vector<std::vector<uint8_t>> literals;
        ShaderBlob vs, ps;
        for (uint16_t k = 0; k < passes; k++) {
            size_t pa = passAt + 40ull * k;
            uint64_t decl = rd64(d, pa);
            if (decl == kFollow || decl == kInsert) {  // inline decl: 0xB0 + name
                size_t vd = pos;
                pos += 0xB0;
                xstring(rd64(d, vd));
            }
            for (int which = 0; which < 2; which++) {
                uint64_t sp = rd64(d, pa + 8 + 8 * which);
                if (sp != kFollow && sp != kInsert) continue;
                size_t sh = pos;
                pos += 0x20;
                xstring(rd64(d, sh));
                uint16_t dwords = rd16(d, sh + 24);
                ShaderBlob blob{slice(d, sh, 0x20), {}};
                if (rd64(d, sh + 16)) {
                    blob.code = slice(d, pos, 4ull * dwords);
                    pos += 4ull * dwords;
                }
                (which ? ps : vs) = std::move(blob);
            }
            size_t argc = static_cast<size_t>(d[pa + 24]) + d[pa + 25] + d[pa + 26];
            if (rd64(d, pa + 32)) {
                size_t ao = pos;
                pos += 16 * argc;
                args = slice(d, ao, 16 * argc);
                literals.assign(argc, {});
                for (size_t a = 0; a < argc; a++) {
                    uint16_t type = rd16(d, ao + 16 * a);
                    if ((type == 1 || type == 7) && rd64(d, ao + 16 * a + 8) == kFollow) {
                        literals[a] = slice(d, pos, 16);
                        pos += 16;
                    }
                }
            }
        }
        xstring(rd64(d, tech));
        if (t == 4 && passes == 1) {
            out.technique = slice(d, tech, 16);
            out.pass = slice(d, passAt, 40);
            out.vs = std::move(vs);
            out.ps = std::move(ps);
            out.args = std::move(args);
            out.literals = std::move(literals);
        }
        if (pos > d.size()) break;
    }
    if (out.technique.empty() || out.vs.code.empty() || out.ps.code.empty()) {
        error = "wc_sky's sky technique isn't self-contained in this zone";
        return false;
    }
    // Sky material: struct (0x78) right before its name; then (techset is a back-reference) the
    // texture table and the state bits table inline.
    size_t matName = find(d, "wc/sky_");
    while (matName != std::string::npos && (matName < 0x78 || rd64(d, matName - 0x78) != kFollow))
        matName = find(d, "wc/sky_", matName + 1);
    if (matName == std::string::npos) {
        error = "no wc/sky_* material in the zone";
        return false;
    }
    const size_t m = matName - 0x78;
    out.material = slice(d, m, 0x78);
    pos = matName;
    while (pos < d.size() && d[pos]) pos++;
    pos++;
    const uint8_t texCount = d[m + 80], stateCount = d[m + 82];
    if (rd64(d, m + 88) == kFollow) {
        error = "stock sky material carries its technique set inline (unexpected layout)";
        return false;
    }
    if (texCount != 1 || rd64(d, m + 96) != kFollow || rd64(d, m + 104) != 0) {
        error = "stock sky material has an unexpected texture/constant layout";
        return false;
    }
    out.textureDef = slice(d, pos, 16);
    pos += 16;
    uint64_t img = rd64(out.textureDef, 8);
    if (img == kFollow || img == kInsert) {
        error = "stock sky material carries its image inline (unexpected layout)";
        return false;
    }
    if (rd64(d, m + 112) == kFollow) out.stateBits = slice(d, pos, 8ull * stateCount);
    // Sky cube image header: 0x28 struct before a "sky_*_ft" name, then its 16-byte load def.
    size_t imgName = find(d, "sky_");
    while (imgName != std::string::npos) {
        size_t e = imgName;
        while (e < d.size() && d[e]) e++;
        std::string n(reinterpret_cast<const char*>(d.data() + imgName), e - imgName);
        if (imgName >= 0x28 && n.size() > 3 && n.compare(n.size() - 3, 3, "_ft") == 0 && d[imgName - 0x28 + 8] == 5) {
            out.image = slice(d, imgName - 0x28, 0x28);
            out.imageLoadDef = slice(d, e + 1, 16);
            break;
        }
        imgName = find(d, "sky_", imgName + 1);
    }
    if (out.image.empty() || out.imageLoadDef.size() != 16) {
        error = "no sky cube image header in the zone";
        return false;
    }
    return true;
}

bool load(const std::string& gameDir, const std::string& cacheDir, Source& out, std::string& error) {
    namespace fs = std::filesystem;
    const fs::path cache = cacheDir.empty() ? fs::path() : fs::path(cacheDir) / "sky_source.bin";
    if (!cache.empty()) {
        std::ifstream f(cache, std::ios::binary);
        uint32_t magic = 0;
        if (f.read(reinterpret_cast<char*>(&magic), 4) && magic == kCacheMagic) {
            Source s;
            bool ok = true;
            for (std::vector<uint8_t>* b : blobs(s)) {
                uint32_t n = 0;
                if (!f.read(reinterpret_cast<char*>(&n), 4) || n > (16u << 20)) { ok = false; break; }
                b->resize(n);
                if (n && !f.read(reinterpret_cast<char*>(b->data()), n)) { ok = false; break; }
            }
            uint32_t lits = 0;
            if (ok && f.read(reinterpret_cast<char*>(&lits), 4) && lits < 256) {
                s.literals.resize(lits);
                for (auto& l : s.literals) {
                    uint32_t n = 0;
                    if (!f.read(reinterpret_cast<char*>(&n), 4) || n > 16) { ok = false; break; }
                    l.resize(n);
                    if (n) f.read(reinterpret_cast<char*>(l.data()), n);
                }
                if (ok && s.valid()) {
                    out = std::move(s);
                    return true;
                }
            }
        }
    }
    const fs::path ff = fs::path(gameDir) / "zone" / "english" / "trainer.ff";
    std::ifstream in(ff, std::ios::binary);
    if (!in) {
        error = "can't open " + ff.string() + " (needed once to copy the stock sky shader)";
        return false;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Fastfile zf;
    if (!readFastfile(file, zf, error)) return false;
    if (!extract(zf.zone, out, error)) return false;
    if (!cache.empty()) {
        std::error_code ec;
        fs::create_directories(cache.parent_path(), ec);
        std::ofstream f(cache, std::ios::binary | std::ios::trunc);
        uint32_t magic = kCacheMagic;
        f.write(reinterpret_cast<const char*>(&magic), 4);
        Source copy = out;
        for (std::vector<uint8_t>* b : blobs(copy)) {
            uint32_t n = static_cast<uint32_t>(b->size());
            f.write(reinterpret_cast<const char*>(&n), 4);
            f.write(reinterpret_cast<const char*>(b->data()), n);
        }
        uint32_t lits = static_cast<uint32_t>(out.literals.size());
        f.write(reinterpret_cast<const char*>(&lits), 4);
        for (const auto& l : out.literals) {
            uint32_t n = static_cast<uint32_t>(l.size());
            f.write(reinterpret_cast<const char*>(&n), 4);
            f.write(reinterpret_cast<const char*>(l.data()), n);
        }
    }
    return true;
}

std::string imageForMaterial(const std::string& material, const std::string& fallback) {
    std::string n;
    for (char c : material) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.rfind("wc/", 0) == 0) n = n.substr(3);
    if (n.rfind("sky_", 0) != 0 || n.size() <= 4) return fallback;
    if (n.size() > 3 && n.compare(n.size() - 3, 3, "_ft") == 0) return n;
    return n + "_ft";
}

}  // namespace sky
