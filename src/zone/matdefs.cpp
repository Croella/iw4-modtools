#include "zone/matdefs.h"

#include <cctype>
#include <cstring>
#include <fstream>

namespace matdef {
namespace {

constexpr uint64_t kFollow = ~0ull, kInsert = ~1ull;
constexpr size_t kMaterial = 0x78, kTexDef = 16, kImage = 0x28;
constexpr uint32_t kMagic = 0x3246444D;  // "MDF2" (world + model materials)

bool inlinePtr(uint64_t v) { return v == kFollow || v == kInsert; }

struct Reader {
    const std::vector<uint8_t>& d;
    size_t pos;
    bool ok = true;
    uint64_t u64(size_t o) const {
        uint64_t v = 0;
        if (o + 8 <= d.size()) std::memcpy(&v, d.data() + o, 8);
        return v;
    }
    uint16_t u16(size_t o) const {
        uint16_t v = 0;
        if (o + 2 <= d.size()) std::memcpy(&v, d.data() + o, 2);
        return v;
    }
    size_t take(size_t n) {
        size_t o = pos;
        pos += n;
        if (pos > d.size()) ok = false;
        return o;
    }
    std::string xstring(uint64_t ptr) {
        if (!inlinePtr(ptr)) return {};
        size_t s = pos;
        while (pos < d.size() && d[pos]) pos++;
        if (pos >= d.size() || pos - s > 256) {
            ok = false;
            return {};
        }
        std::string r(reinterpret_cast<const char*>(d.data() + s), pos - s);
        pos++;
        return r;
    }
    std::vector<uint8_t> bytes(size_t o, size_t n) const {
        if (o + n > d.size()) return {};
        return {d.begin() + static_cast<std::ptrdiff_t>(o), d.begin() + static_cast<std::ptrdiff_t>(o + n)};
    }
    // Skips an inline technique set (Load_MaterialTechniqueSet order; see zonesim techsetPtr).
    void skipTechset() {
        size_t ts = take(0x198);
        if (!ok) return;
        xstring(u64(ts));
        for (int t = 0; t < 48 && ok; t++) {
            if (u64(ts + 24 + 8 * t) != kFollow) continue;
            size_t tech = take(16);
            uint16_t passes = u16(tech + 10);
            if (passes == 0 || passes > 16) {
                ok = false;
                return;
            }
            size_t pa0 = take(40ull * passes);
            for (uint16_t k = 0; k < passes && ok; k++) {
                size_t pa = pa0 + 40ull * k;
                if (inlinePtr(u64(pa))) {
                    size_t vd = take(0xB0);
                    xstring(u64(vd));
                }
                for (int s = 0; s < 2; s++) {
                    if (!inlinePtr(u64(pa + 8 + 8 * s))) continue;
                    size_t sh = take(0x20);
                    xstring(u64(sh));
                    if (u64(sh + 16)) take(4ull * u16(sh + 24));
                }
                size_t argc = static_cast<size_t>(d[pa + 24]) + d[pa + 25] + d[pa + 26];
                if (u64(pa + 32)) {
                    size_t ao = take(16 * argc);
                    for (size_t a = 0; a < argc && ok; a++) {
                        uint16_t type = u16(ao + 16 * a);
                        if ((type == 1 || type == 7) && u64(ao + 16 * a + 8) == kFollow) take(16);
                    }
                }
            }
            xstring(u64(tech));
        }
    }
};

bool printableName(const std::string& s) {
    if (s.size() < 4 || s.size() > 120) return false;
    for (unsigned char c : s)
        if (c <= 32 || c >= 127) return false;
    return true;
}

// Parses the material whose name string starts at nameAt (its struct is the 0x78 bytes before).
bool parseAt(const std::vector<uint8_t>& d, size_t nameAt, Def& out) {
    if (nameAt < kMaterial) return false;
    Reader r{d, nameAt};
    const size_t m = nameAt - kMaterial;
    if (r.u64(m) != kFollow) return false;
    const uint8_t texCount = d[m + 80], constCount = d[m + 81], sbCount = d[m + 82];
    if (texCount < 1 || texCount > 16 || constCount > 64 || sbCount < 1 || sbCount > 64) return false;
    const uint64_t techset = r.u64(m + 88), table = r.u64(m + 96), consts = r.u64(m + 104), sb = r.u64(m + 112);
    if (table != kFollow) return false;  // texture table must be inline (back-refs can't be read here)
    out = Def{};
    out.name = r.xstring(kFollow);
    if (!r.ok || !printableName(out.name) || (out.name.rfind("wc/", 0) != 0 && out.name.rfind("mc/", 0) != 0)) return false;
    out.header = r.bytes(m, kMaterial);
    if (inlinePtr(techset)) r.skipTechset();
    if (!r.ok) return false;
    const size_t tt = r.take(kTexDef * texCount);
    if (!r.ok) return false;
    for (int i = 0; i < texCount; i++) {
        Texture t;
        t.def = r.bytes(tt + kTexDef * i, kTexDef);
        const uint8_t semantic = t.def[7];
        if (semantic == 11) return false;  // water: needs its own data, not supported
        if (semantic > 0x20) return false;
        uint64_t img = r.u64(tt + kTexDef * i + 8);
        if (inlinePtr(img)) {
            size_t ih = r.take(kImage);
            if (!r.ok) return false;
            t.imageHeader = r.bytes(ih, kImage);
            uint64_t tex = r.u64(ih), name = r.u64(ih + 32);
            t.image = r.xstring(name);
            if (!r.ok) return false;
            if (!t.image.empty() && t.image[0] == ',') t.image.erase(0, 1);
            if (inlinePtr(tex)) {
                size_t ld = r.take(16);
                if (!r.ok) return false;
                t.loadDef = r.bytes(ld, 16);
                uint32_t size;
                std::memcpy(&size, t.loadDef.data() + 12, 4);
                if (size > (64u << 20)) return false;
                r.take(size);
                if (size) t.image.clear();  // pixels live in the zone, not an .iwi: can't stream it
            }
        }
        out.textures.push_back(std::move(t));
    }
    if (consts == kFollow) {
        size_t c = r.take(32ull * constCount);
        out.constants = r.bytes(c, 32ull * constCount);
    }
    if (sb == kFollow) {
        size_t s = r.take(8ull * sbCount);
        out.stateBits = r.bytes(s, 8ull * sbCount);
    }
    return r.ok;
}

size_t knownImages(const Def& d) {
    size_t n = 0;
    for (const Texture& t : d.textures) n += !t.image.empty() && t.imageHeader.size() == kImage;
    return n;
}

void putBlob(std::ofstream& f, const std::vector<uint8_t>& b) {
    uint32_t n = static_cast<uint32_t>(b.size());
    f.write(reinterpret_cast<const char*>(&n), 4);
    f.write(reinterpret_cast<const char*>(b.data()), n);
}
void putStr(std::ofstream& f, const std::string& s) { putBlob(f, std::vector<uint8_t>(s.begin(), s.end())); }
bool getBlob(std::ifstream& f, std::vector<uint8_t>& b) {
    uint32_t n = 0;
    if (!f.read(reinterpret_cast<char*>(&n), 4) || n > (1u << 20)) return false;
    b.resize(n);
    return !n || static_cast<bool>(f.read(reinterpret_cast<char*>(b.data()), n));
}
bool getStr(std::ifstream& f, std::string& s) {
    std::vector<uint8_t> b;
    if (!getBlob(f, b)) return false;
    s.assign(b.begin(), b.end());
    return true;
}

}  // namespace

std::string key(const std::string& name) {
    std::string k;
    for (char c : name) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

void scan(const std::vector<uint8_t>& d, Library& out) {
    // A material's name follows its struct; look for the "wc/" names and check the struct.
    for (size_t o = kMaterial; o + 4 < d.size(); o++) {
        if ((d[o] != 'w' && d[o] != 'm') || d[o + 1] != 'c' || d[o + 2] != '/') continue;
        Def def;
        if (!parseAt(d, o, def)) continue;
        std::string k = key(def.name);
        auto it = out.find(k);
        if (it == out.end() || knownImages(def) > knownImages(it->second)) out[k] = std::move(def);
    }
}

namespace {
std::string constantName(const Def& d, size_t i) {
    const char* p = reinterpret_cast<const char*>(d.constants.data() + 32 * i + 4);
    size_t n = 0;
    while (n < 12 && p[n]) n++;
    return std::string(p, n);
}
}  // namespace

bool hasConstant(const Def& d, uint32_t nameHash) {
    for (size_t i = 0; i + 1 <= d.constants.size() / 32; i++) {
        uint32_t h;
        std::memcpy(&h, d.constants.data() + 32 * i, 4);
        if (h == nameHash) return true;
    }
    return false;
}

bool constantValue(const Def& d, const char* namePrefix, float out[4]) {
    for (size_t i = 0; i < d.constants.size() / 32; i++)
        if (constantName(d, i).rfind(namePrefix, 0) == 0) {
            std::memcpy(out, d.constants.data() + 32 * i + 16, 16);
            return true;
        }
    return false;
}

Shading shadingFor(const Def& d) {
    bool falloff = false, other = false;
    for (size_t i = 0; i < d.constants.size() / 32; i++) {
        std::string n = constantName(d, i);
        if (n.rfind("falloff", 0) == 0) falloff = true;
        else if (n != "colorTint" && n != "envMapParms" && n != "detailScale" && n != "uvAnimParms") other = true;
    }
    bool color = false;
    for (const Texture* t : usedTextures(d)) color |= t->kind() == 'c';
    if (!color || other) return Shading::Unsupported;
    return falloff ? Shading::FalloffAdd : Shading::Lit;
}

std::vector<const Texture*> usedTextures(const Def& d) {
    std::vector<const Texture*> used;
    for (const Texture& t : d.textures) {
        if (t.image.empty() || t.imageHeader.size() != kImage || t.def.size() != kTexDef) continue;
        const char k = t.kind();
        const char last = static_cast<char>(t.def[5]);
        if (last == 'p' && (k == 'c' || k == 'n' || k == 's')) used.push_back(&t);  // colorMap/normalMap/specularMap
    }
    return used;
}

std::string techsetFor(const Def& d) {
    if (shadingFor(d) != Shading::Lit) return {};
    bool c = false, n = false, s = false;
    for (const Texture* t : usedTextures(d)) {
        c |= t->kind() == 'c';
        n |= t->kind() == 'n';
        s |= t->kind() == 's';
    }
    if (!c) return {};
    // World (wc/) and model (mc/) materials use the matching common.ff family.
    const bool model = d.name.rfind("mc/", 0) == 0;
    return std::string(model ? "mc_l_sm_b0c0" : "wc_l_sm_b0c0") + (n ? "n0" : "") + (s ? "s0" : "");
}

bool save(const std::string& path, const Library& lib) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    uint32_t magic = kMagic, count = static_cast<uint32_t>(lib.size());
    f.write(reinterpret_cast<const char*>(&magic), 4);
    f.write(reinterpret_cast<const char*>(&count), 4);
    for (const auto& [k, d] : lib) {
        putStr(f, d.name);
        putBlob(f, d.header);
        putBlob(f, d.constants);
        putBlob(f, d.stateBits);
        uint32_t n = static_cast<uint32_t>(d.textures.size());
        f.write(reinterpret_cast<const char*>(&n), 4);
        for (const Texture& t : d.textures) {
            putBlob(f, t.def);
            putStr(f, t.image);
            putBlob(f, t.imageHeader);
            putBlob(f, t.loadDef);
        }
    }
    return static_cast<bool>(f);
}

bool load(const std::string& path, Library& lib) {
    std::ifstream f(path, std::ios::binary);
    uint32_t magic = 0, count = 0;
    if (!f.read(reinterpret_cast<char*>(&magic), 4) || magic != kMagic || !f.read(reinterpret_cast<char*>(&count), 4))
        return false;
    Library out;
    for (uint32_t i = 0; i < count; i++) {
        Def d;
        uint32_t n = 0;
        if (!getStr(f, d.name) || !getBlob(f, d.header) || !getBlob(f, d.constants) || !getBlob(f, d.stateBits) ||
            !f.read(reinterpret_cast<char*>(&n), 4) || n > 16)
            return false;
        d.textures.resize(n);
        for (Texture& t : d.textures)
            if (!getBlob(f, t.def) || !getStr(f, t.image) || !getBlob(f, t.imageHeader) || !getBlob(f, t.loadDef)) return false;
        out[key(d.name)] = std::move(d);
    }
    lib = std::move(out);
    return true;
}

}  // namespace matdef
