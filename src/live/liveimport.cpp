#include "live/liveimport.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "live/process.h"
#include "zone/iw4.h"

namespace live {
namespace {

// Image-relative addresses (RVAs) of the asset-pool globals, per game executable. Runtime =
// module base + RVA. Decoded from the pool allocator (the "Exceeded limit of %d '%s' assets"
// function): it indexes [type*8 + DB_XAssetPool] (the pool passed to the per-type alloc fn),
// [type*4 + g_poolSize] (the limit), and [type*8 + g_assetNames] (the type name).
// If the game updates again these move; re-decode that function from the new binary.
// SP and MP share the asset-type enum (gfx_map = 0x15, xmodel = 4) and the struct layouts.
struct GameLayout {
    const char* exe;
    uintptr_t dbXAssetPool;  // per-type pool base pointers, stride 8
    uintptr_t gPoolSize;     // per-type capacity (asset-limit table), stride 4
    uintptr_t assetNames;    // per-type name strings (const char*), stride 8
};
constexpr GameLayout kGames[] = {
    {"iw4mp.exe", 0x407D60, 0x407890, 0x407280},  // 2.0.14 (iw4mp_2014.exe), allocator RVA 0x12cef0
    {"iw4sp.exe", 0x3BFE00, 0x3BF930, 0x3BF360},  // 2.0.14 (iw4_2014_sp.exe), allocator RVA 0xeee30
};
constexpr uint32_t kGfxWorld = 0x15;  // ASSET_TYPE_GFXWORLD

// Attaches to exeName, or (exeName == nullptr) to whichever known game is running, MP first.
const GameLayout* attachGame(ProcessReader& r, const char* exeName, std::string& error) {
    std::string firstErr;
    for (const GameLayout& g : kGames) {
        if (exeName && _stricmp(exeName, g.exe) != 0) continue;
        std::string err;
        if (r.attach(g.exe, err)) return &g;
        if (exeName || err.find("isn't running") == std::string::npos) {
            error = err;  // explicit target, or the game is running but couldn't be opened
            return nullptr;
        }
    }
    error = exeName ? std::string("no pool addresses known for ") + exeName
                    : "neither iw4mp.exe nor iw4sp.exe is running";
    return nullptr;
}

// Guards against a bad pointer turning into a huge allocation.
constexpr uint32_t kMaxVerts = 200000;
constexpr uint32_t kMaxTris = 300000;
constexpr uint32_t kMaxSurfs = 4096;
constexpr uint32_t kMaxModelInsts = 1 << 20;

std::string editorMaterialName(std::string n) {
    if (!n.empty() && n[0] == ',') n.erase(0, 1);  // load-order marker
    if (n.rfind("wc/", 0) == 0) n.erase(0, 3);
    return n;
}

// IW4 packs model texcoords as two 16-bit half-floats in one u32 (low = u, high = v).
float halfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign << 31;
        } else {
            exp = 1;
            while (!(man & 0x400)) { man <<= 1; --exp; }
            man &= 0x3FF;
            bits = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
        }
    } else if (exp == 0x1F) {
        bits = (sign << 31) | (0xFFu << 23) | (man << 13);
    } else {
        bits = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Same unit-vector unpack the GfxWorld importer uses (gfxworld.cpp / mapimport.cpp).
void unpackNormal(uint32_t p, float out[3]) {
    double scale = (static_cast<double>(p >> 24) + 192.0) / 32385.0;
    for (int k = 0; k < 3; k++) out[k] = static_cast<float>((((p >> (8 * k)) & 0xFF) - 127.0) * scale);
}

struct Vtx {
    float pos[3], nrm[3], uv[2];
};
struct LocalSurf {
    std::string material;  // editor name
    std::string image;     // colorMap image, if resolved
    std::vector<Vtx> verts;
    std::vector<uint16_t> idx;  // 0-based into verts
};
struct LocalModel {
    std::string name;
    std::vector<LocalSurf> surfs;
};

class Walker {
public:
    Walker(const ProcessReader& r) : r_(r) {}

    // Reads a material's editor name and colorMap image from its remote address.
    void readMaterial(uint64_t matAddr, std::string& name, std::string& image) {
        if (!matAddr) return;
        name = editorMaterialName(
            r_.readString(r_.readPtr(matAddr + offsetof(iw4::Material, info) + offsetof(iw4::MaterialInfo, name))));
        uint8_t texCount = r_.readOr<uint8_t>(matAddr + offsetof(iw4::Material, textureCount));
        uint64_t table = r_.readPtr(matAddr + offsetof(iw4::Material, textureTable));
        if (!table) return;
        for (uint8_t t = 0; t < texCount; t++) {
            iw4::MaterialTextureDef td{};
            if (!r_.read(table + t * sizeof(iw4::MaterialTextureDef), td)) break;
            if (td.semantic == iw4::TS_COLOR_MAP && td.u.image) {
                std::string img = r_.readString(r_.readPtr(reinterpret_cast<uintptr_t>(td.u.image) +
                                                           offsetof(iw4::GfxImage, name)));
                if (!img.empty()) image = editorMaterialName(img);
                break;
            }
        }
    }

    // Decodes one XModel (its top LOD) into local surfaces, cached by remote address.
    std::shared_ptr<const LocalModel> model(uint64_t modelAddr) {
        auto it = cache_.find(modelAddr);
        if (it != cache_.end()) return it->second;
        auto m = std::make_shared<LocalModel>();
        cache_[modelAddr] = m;  // cache even on partial failure, so we don't retry a bad model
        iw4::XModel xm{};
        if (!r_.read(modelAddr, xm)) return m;
        m->name = r_.readString(reinterpret_cast<uintptr_t>(xm.name));
        if (xm.numLods == 0) return m;
        const iw4::XModelLodInfo& lod = xm.lodInfo[0];  // highest detail
        uint64_t surfsBase = r_.readPtr(reinterpret_cast<uintptr_t>(lod.modelSurfs) +
                                        offsetof(iw4::XModelSurfs, surfs));
        uint64_t mats = reinterpret_cast<uint64_t>(xm.materialHandles);
        uint32_t nsurf = std::min<uint32_t>(lod.numsurfs, kMaxSurfs);
        for (uint32_t j = 0; j < nsurf && surfsBase; j++) {
            iw4::XSurface xs{};
            if (!r_.read(surfsBase + j * sizeof(iw4::XSurface), xs)) break;
            if (xs.vertCount == 0 || xs.vertCount > kMaxVerts || xs.triCount == 0 || xs.triCount > kMaxTris)
                continue;
            std::vector<iw4::GfxPackedVertex> verts;
            std::vector<uint16_t> idx;
            if (!r_.readVec(reinterpret_cast<uintptr_t>(xs.verts0), xs.vertCount, verts)) continue;
            if (!r_.readVec(reinterpret_cast<uintptr_t>(xs.triIndices), 3u * xs.triCount, idx)) continue;
            LocalSurf surf;
            if (mats)
                readMaterial(r_.readPtr(mats + 8ull * (lod.surfIndex + j)), surf.material, surf.image);
            surf.verts.reserve(verts.size());
            for (const iw4::GfxPackedVertex& v : verts) {
                Vtx o;
                o.pos[0] = v.xyz[0];
                o.pos[1] = v.xyz[1];
                o.pos[2] = v.xyz[2];
                unpackNormal(v.normal.packed, o.nrm);
                o.uv[0] = halfToFloat(static_cast<uint16_t>(v.texCoord.packed & 0xFFFF));
                o.uv[1] = halfToFloat(static_cast<uint16_t>(v.texCoord.packed >> 16));
                surf.verts.push_back(o);
            }
            surf.idx = std::move(idx);
            m->surfs.push_back(std::move(surf));
        }
        return m;
    }

private:
    const ProcessReader& r_;
    std::unordered_map<uint64_t, std::shared_ptr<const LocalModel>> cache_;
};

// Appends src's geometry onto dst, offsetting indices and batch starts so the two meshes combine
// into one. Lets a live dump add its models to whatever reference geometry is already loaded.
void appendMesh(RefMesh& dst, const RefMesh& src) {
    uint32_t baseVert = static_cast<uint32_t>(dst.positions.size() / 3);
    uint32_t baseIndex = static_cast<uint32_t>(dst.indices.size());
    dst.positions.insert(dst.positions.end(), src.positions.begin(), src.positions.end());
    dst.normals.insert(dst.normals.end(), src.normals.begin(), src.normals.end());
    dst.uvs.insert(dst.uvs.end(), src.uvs.begin(), src.uvs.end());
    dst.indices.reserve(dst.indices.size() + src.indices.size());
    for (uint32_t ix : src.indices) dst.indices.push_back(baseVert + ix);
    for (const RefMesh::Batch& b : src.batches)
        dst.batches.push_back({b.material, baseIndex + b.firstIndex, b.indexCount});
    dst.bounds.add(src.bounds);
    for (const auto& [name, image] : src.materialImages) dst.materialImages.emplace(name, image);
}

// origin + scale * (axis rows) applied to a local vector; axis[j] is the image of basis vector j.
void transformPos(const iw4::GfxPackedPlacement& p, const float v[3], float out[3]) {
    for (int k = 0; k < 3; k++)
        out[k] = p.origin[k] + p.scale * (p.axis[0][k] * v[0] + p.axis[1][k] * v[1] + p.axis[2][k] * v[2]);
}
void transformNrm(const iw4::GfxPackedPlacement& p, const float v[3], float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = p.axis[0][k] * v[0] + p.axis[1][k] * v[1] + p.axis[2][k] * v[2];
    float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len > 1e-6f)
        for (int k = 0; k < 3; k++) out[k] /= len;
}

// A GfxWorld address is good if its name field reads back as a map bsp path.
bool worldNameOk(const ProcessReader& r, uint64_t world, std::string& name) {
    if (!world) return false;
    std::string n = r.readString(r.readPtr(world + offsetof(iw4::GfxWorld, name)), 128);
    if (n.rfind("maps/", 0) != 0) return false;
    name = n;
    return true;
}

// Finds the loaded map's GfxWorld in the asset pool. DB_XAssetPool[type] is either the base of a
// contiguous struct array or an array of pointers depending on the type's allocator; we accept
// whichever interpretation yields a valid map name, so we don't depend on guessing which.
uint64_t findGfxWorld(const ProcessReader& r, const GameLayout& g, std::string& name) {
    uint64_t pool = r.readPtr(r.rva(g.dbXAssetPool + 8 * kGfxWorld));
    if (!pool) return 0;
    uint32_t cap = r.readOr<uint32_t>(r.rva(g.gPoolSize + 4 * kGfxWorld), 1u);
    cap = std::min<uint32_t>(cap ? cap : 1u, 64);
    for (uint32_t i = 0; i < cap; i++) {
        uint64_t inlineWorld = pool + static_cast<uint64_t>(i) * sizeof(iw4::GfxWorld);
        if (worldNameOk(r, inlineWorld, name)) return inlineWorld;
        uint64_t ptrWorld = r.readPtr(pool + static_cast<uint64_t>(i) * 8);  // array-of-pointers layout
        if (worldNameOk(r, ptrWorld, name)) return ptrWorld;
    }
    return 0;
}

}  // namespace

// Load_XModel reads a 408-byte struct; the walker relies on these offsets.
static_assert(sizeof(iw4::XModel) == 408, "XModel size differs from the loader's 408 bytes");
static_assert(offsetof(iw4::XModel, materialHandles) == 88, "XModel.materialHandles moved");
static_assert(offsetof(iw4::XModel, lodInfo) == 96 && sizeof(iw4::XModelLodInfo) == 56, "XModel.lodInfo moved");

std::string modelKey(std::string name) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

bool loadLiveModels(const std::vector<std::string>& names, LiveModelLibrary& lib, LiveModelStats& stats,
                    std::string& error, const char* exeName) {
    constexpr uint32_t kXModel = 4;  // ASSET_TYPE_XMODEL
    stats = LiveModelStats{};
    std::map<std::string, bool> wanted;  // key -> found
    for (const std::string& n : names)
        if (!n.empty()) wanted.emplace(modelKey(n), false);
    stats.requested = wanted.size();
    if (wanted.empty()) return true;

    ProcessReader r;
    const GameLayout* g = attachGame(r, exeName, error);
    if (!g) return false;
    stats.game = g->exe;
    const uint64_t pool = r.readPtr(r.rva(g->dbXAssetPool + 8 * kXModel));
    const uint32_t cap = r.readOr<uint32_t>(r.rva(g->gPoolSize + 4 * kXModel));
    if (!pool || cap == 0 || cap > 65536) {
        error = "couldn't find the game's XModel pool";
        return false;
    }
    // The pool is an array of 408-byte entries, possibly behind an 8-byte free-list head (iw4sp
    // puts one there; entries then start at pool+8). A used entry starts with its name pointer; a
    // free one starts with a free-list link into the pool itself, which we skip.
    constexpr uint64_t kStride = sizeof(iw4::XModel);
    std::vector<uint8_t> entries;
    const uint64_t poolBytes = kStride * cap + 8;
    if (!r.readVec(pool, static_cast<size_t>(poolBytes), entries)) {
        error = "couldn't read the game's XModel pool";
        return false;
    }
    auto printable = [](const std::string& s) {
        if (s.empty()) return false;
        for (unsigned char c : s)
            if (c < 0x20 || c >= 0x7f) return false;
        return true;
    };
    auto qwordAt = [&](uint64_t off) {
        uint64_t p;
        std::memcpy(&p, entries.data() + off, 8);
        return p;
    };
    auto inPool = [&](uint64_t p) { return p >= pool && p < pool + poolBytes; };
    // Pick the layout whose entries read back as names: inline structs at offset 0 or 8, else the
    // pointer-array layout (the pool holds XModel* slots), as for GfxWorld.
    uint64_t head = 0;
    bool pointerLayout = false;
    {
        int best = 0;
        for (uint64_t h : {0ull, 8ull}) {
            int hits = 0;
            for (uint32_t i = 0; i < cap && i < 128; i++) {
                uint64_t p = qwordAt(h + kStride * i);
                if (p && !inPool(p) && printable(r.readString(p, 128))) hits++;
            }
            if (hits > best) best = hits, head = h;
        }
        pointerLayout = best == 0;
    }
    auto modelAt = [&](uint32_t i) -> uint64_t {
        return pointerLayout ? qwordAt(8ull * i) : pool + head + kStride * i;
    };
    Walker walker(r);
    for (uint32_t i = 0; i < cap; i++) {
        uint64_t modelAddr = modelAt(i);
        if (!modelAddr) continue;
        uint64_t namePtr = pointerLayout ? r.readPtr(modelAddr) : qwordAt(head + kStride * i);
        if (!namePtr || inPool(namePtr)) continue;  // free-list link
        std::string name = r.readString(namePtr, 128);
        if (!printable(name) || name[0] == ',') continue;
        auto it = wanted.find(modelKey(name));
        if (it == wanted.end() || it->second) continue;
        std::shared_ptr<const LocalModel> m = walker.model(modelAddr);
        if (m->surfs.empty()) continue;
        auto out = std::make_shared<LiveModel>();
        out->name = name;
        for (const LocalSurf& s : m->surfs) {
            LiveModelSurf ls;
            ls.material = s.material;
            ls.image = s.image;
            ls.idx = s.idx;
            for (const Vtx& v : s.verts) {
                ls.pos.insert(ls.pos.end(), v.pos, v.pos + 3);
                ls.nrm.insert(ls.nrm.end(), v.nrm, v.nrm + 3);
                ls.uv.insert(ls.uv.end(), v.uv, v.uv + 2);
                out->bounds.add(Vec3{v.pos[0], v.pos[1], v.pos[2]});
            }
            stats.surfaces++;
            stats.triangles += ls.idx.size() / 3;
            out->surfs.push_back(std::move(ls));
        }
        lib[it->first] = std::move(out);
        it->second = true;
        stats.found++;
    }
    for (const auto& [key, found] : wanted)
        if (!found) stats.missing.push_back(key);
    return true;
}

bool probeProcess(std::string& report, std::string& error, const char* exeName) {
    ProcessReader r;
    const GameLayout* g = attachGame(r, exeName, error);
    if (!g) return false;
    char line[512];
    auto add = [&](const char* fmt, auto... a) {
        std::snprintf(line, sizeof(line), fmt, a...);
        report += line;
    };
    add("game: %s\n", g->exe);
    add("module base: 0x%llX  size: 0x%zX\n", static_cast<unsigned long long>(r.base()), r.moduleSize());
    add("DB_XAssetPool @ base+0x%llX, g_poolSize @ base+0x%llX, names @ base+0x%llX\n",
        (unsigned long long)g->dbXAssetPool, (unsigned long long)g->gPoolSize, (unsigned long long)g->assetNames);
    // Asset-type names are static strings; reading them back correctly proves the addressing.
    add("asset type names (type: name  pool  count):\n");
    for (uint32_t t = 0; t <= 0x2A; t++) {
        std::string name = r.readString(r.readPtr(r.rva(g->assetNames + 8 * t)), 64);
        uint64_t pool = r.readPtr(r.rva(g->dbXAssetPool + 8 * t));
        uint32_t count = r.readOr<uint32_t>(r.rva(g->gPoolSize + 4 * t));
        if (name.empty() && !pool && !count) continue;
        add("  0x%02X: %-18s pool=0x%llX count=%u\n", t, name.empty() ? "<none>" : name.c_str(),
            (unsigned long long)pool, count);
    }
    std::string mapName;
    uint64_t world = findGfxWorld(r, *g, mapName);
    if (world)
        add("GfxWorld found at 0x%llX: %s\n", (unsigned long long)world, mapName.c_str());
    else
        add("GfxWorld: not found (no map loaded, or the pool layout/address is off)\n");
    return true;
}

bool inspectWorld(std::string& report, std::string& error, const char* exeName) {
    ProcessReader r;
    const GameLayout* g = attachGame(r, exeName, error);
    if (!g) return false;
    char line[512];
    auto add = [&](const char* fmt, auto... a) {
        std::snprintf(line, sizeof(line), fmt, a...);
        report += line;
    };
    std::string mapName;
    uint64_t wa = findGfxWorld(r, *g, mapName);
    if (!wa) {
        error = "no map loaded";
        return false;
    }
    iw4::GfxWorld w{};
    r.read(wa, w);
    add("%s: %u surfaces, skyCount %d, primaryLightCount %u, lastSun %u, lightmaps %d\n", mapName.c_str(),
        w.surfaceCount, w.skyCount, w.primaryLightCount, w.lastSunPrimaryLightIndex, w.draw.lightmapCount);
    auto imageInfo = [&](uint64_t ia) {
        if (!ia) return std::string("null");
        iw4::GfxImage im{};
        r.read(ia, im);
        char b[256];
        std::snprintf(b, sizeof b, "%s %ux%u map%d sem%d cat%d tex=%p", r.readString(reinterpret_cast<uintptr_t>(im.name)).c_str(),
                      im.width, im.height, im.mapType, static_cast<int>(im.semantic), im.category, im.texture.basemap);
        return std::string(b);
    };
    if (w.draw.lightmapCount > 0 && w.draw.lightmaps) {
        iw4::GfxLightmapArray la{};
        r.read(reinterpret_cast<uintptr_t>(w.draw.lightmaps), la);
        add("  lightmap primary:   %s\n", imageInfo(reinterpret_cast<uintptr_t>(la.primary)).c_str());
        add("  lightmap secondary: %s\n", imageInfo(reinterpret_cast<uintptr_t>(la.secondary)).c_str());
    }
    add("  lightmap override primary %p secondary %p\n", w.draw.lightmapOverridePrimary, w.draw.lightmapOverrideSecondary);
    add("  lightGrid: entries %u colors %u\n", w.lightGrid.entryCount, w.lightGrid.colorCount);
    for (uint32_t i = 0; i < w.surfaceCount && i < 24; i++) {
        iw4::GfxSurface s{};
        r.read(reinterpret_cast<uintptr_t>(w.dpvs.surfaces) + i * sizeof(iw4::GfxSurface), s);
        iw4::Material m{};
        r.read(reinterpret_cast<uintptr_t>(s.material), m);
        std::string mname = r.readString(reinterpret_cast<uintptr_t>(m.info.name));
        std::string tsname = m.techniqueSet ? r.readString(r.readPtr(reinterpret_cast<uintptr_t>(m.techniqueSet))) : "null";
        std::string techs;
        if (m.techniqueSet)
            for (int t = 0; t < 48; t++) {
                uint64_t tp = r.readPtr(reinterpret_cast<uintptr_t>(m.techniqueSet) + 24 + 8 * t);
                if (tp) techs += std::to_string(t) + ":" + r.readString(r.readPtr(tp)) + " ";
            }
        add("  surf %u: tris %u verts %u  lmap %u probe %u primary %u flags %u  material %s sortKey %u techset %s\n", i,
            s.tris.triCount, s.tris.vertexCount, s.laf.fields.lightmapIndex, s.laf.fields.reflectionProbeIndex,
            s.laf.fields.primaryLightIndex, s.laf.fields.flags, mname.c_str(), m.info.sortKey, tsname.c_str());
        add("      techniques: %s\n", techs.c_str());
        if (m.textureCount && m.textureTable) {
            iw4::MaterialTextureDef td{};
            r.read(reinterpret_cast<uintptr_t>(m.textureTable), td);
            add("      tex0 sem %d: %s\n", static_cast<int>(td.semantic), imageInfo(reinterpret_cast<uintptr_t>(td.u.image)).c_str());
        }
        iw4::GfxWorldVertex v{};
        r.read(reinterpret_cast<uintptr_t>(w.draw.vd.vertices) + s.tris.firstVertex * sizeof(iw4::GfxWorldVertex), v);
        add("      v0 lmap %.4f %.4f  normal %08x  color %08x\n", v.lmapCoord[0], v.lmapCoord[1], v.normal.packed, v.color.packed);
    }
    uint64_t cwa = r.readPtr(r.rva(g->dbXAssetPool + 8 * 0x10));
    iw4::ComWorld cw{};
    if (cwa && r.read(cwa, cw)) {
        add("ComWorld %s: %u primary lights\n", r.readString(reinterpret_cast<uintptr_t>(cw.name)).c_str(), cw.primaryLightCount);
        for (uint32_t i = 0; i < cw.primaryLightCount && i < 4; i++) {
            iw4::ComPrimaryLight pl{};
            r.read(reinterpret_cast<uintptr_t>(cw.primaryLights) + i * sizeof(pl), pl);
            add("  light %u: type %d color %.3f %.3f %.3f dir %.3f %.3f %.3f\n", i, pl.type, pl.color[0], pl.color[1], pl.color[2],
                pl.dir[0], pl.dir[1], pl.dir[2]);
        }
    }
    return true;
}

bool importFromProcess(Document& doc, LiveStats& stats, std::string& error, const char* exeName) {
    stats = LiveStats{};
    ProcessReader r;
    const GameLayout* g = attachGame(r, exeName, error);
    if (!g) return false;
    stats.game = g->exe;

    uint64_t worldAddr = findGfxWorld(r, *g, stats.mapName);
    if (!worldAddr) {
        error = "no map is loaded in the game (couldn't find a GfxWorld). Load a map first, then dump.";
        return false;
    }
    uint64_t dpvs = worldAddr + offsetof(iw4::GfxWorld, dpvs);
    uint32_t smodelCount = r.readOr<uint32_t>(dpvs + offsetof(iw4::GfxWorldDpvsStatic, smodelCount));
    uint64_t drawInsts = r.readPtr(dpvs + offsetof(iw4::GfxWorldDpvsStatic, smodelDrawInsts));
    if (!drawInsts || smodelCount == 0) {
        error = "the loaded map has no static models (" + stats.mapName + ")";
        return false;
    }
    smodelCount = std::min<uint32_t>(smodelCount, kMaxModelInsts);

    auto mesh = std::make_shared<RefMesh>();
    std::map<std::string, std::vector<uint32_t>> byMaterial;
    Walker walker(r);
    std::unordered_map<uint64_t, bool> seenModel;

    for (uint32_t i = 0; i < smodelCount; i++) {
        iw4::GfxStaticModelDrawInst inst{};
        if (!r.read(drawInsts + static_cast<uint64_t>(i) * sizeof(iw4::GfxStaticModelDrawInst), inst)) continue;
        uint64_t modelAddr = reinterpret_cast<uint64_t>(inst.model);
        if (!modelAddr) {
            stats.modelsSkipped++;
            continue;
        }
        std::shared_ptr<const LocalModel> m = walker.model(modelAddr);
        if (m->surfs.empty()) {
            stats.modelsSkipped++;
            continue;
        }
        if (seenModel.emplace(modelAddr, true).second) stats.uniqueModels++;
        stats.models++;
        for (const LocalSurf& s : m->surfs) {
            if (!s.material.empty() && !s.image.empty() && !mesh->materialImages.count(s.material))
                mesh->materialImages[s.material] = s.image;
            std::vector<uint32_t>& out = byMaterial[s.material];
            uint32_t base = static_cast<uint32_t>(mesh->positions.size() / 3);
            for (const Vtx& v : s.verts) {
                float wp[3], wn[3];
                transformPos(inst.placement, v.pos, wp);
                transformNrm(inst.placement, v.nrm, wn);
                for (int k = 0; k < 3; k++) mesh->positions.push_back(wp[k]);
                for (int k = 0; k < 3; k++) mesh->normals.push_back(wn[k]);
                mesh->uvs.push_back(v.uv[0]);
                mesh->uvs.push_back(v.uv[1]);
                mesh->bounds.add(Vec3{wp[0], wp[1], wp[2]});
            }
            for (uint16_t ix : s.idx) out.push_back(base + ix);
            stats.triangles += s.idx.size() / 3;
            stats.surfaces++;
        }
    }

    if (mesh->positions.empty()) {
        error = "found " + std::to_string(smodelCount) + " model instances but couldn't read any geometry";
        return false;
    }
    for (auto& [name, idx] : byMaterial) {
        mesh->batches.push_back({name, static_cast<uint32_t>(mesh->indices.size()), static_cast<uint32_t>(idx.size())});
        mesh->indices.insert(mesh->indices.end(), idx.begin(), idx.end());
    }
    stats.materials = byMaterial.size();
    stats.images = mesh->materialImages.size();

    // Merge into the current document rather than replacing it: the dumped models join whatever is
    // already open (e.g. a map imported from a .ff) as reference geometry. A previous live dump is
    // stripped first, so repeat dumps replace instead of stacking; non-dump geometry is kept.
    auto combined = std::make_shared<RefMesh>();
    if (doc.refMesh) {
        *combined = *doc.refMesh;
        if (combined->hasLiveDump) {
            combined->positions.resize(combined->liveVertStart * 3);
            combined->normals.resize(combined->liveVertStart * 3);
            combined->uvs.resize(combined->liveVertStart * 2);
            combined->indices.resize(combined->liveIndexStart);
            combined->batches.resize(combined->liveBatchStart);
        }
    }
    combined->liveVertStart = combined->positions.size() / 3;
    combined->liveIndexStart = combined->indices.size();
    combined->liveBatchStart = combined->batches.size();
    combined->hasLiveDump = true;
    appendMesh(*combined, *mesh);
    // Stripping a prior dump can leave the bounds too large, so recompute them from the final set.
    combined->bounds = AABB{};
    for (size_t i = 0; i + 2 < combined->positions.size(); i += 3)
        combined->bounds.add(Vec3{combined->positions[i], combined->positions[i + 1], combined->positions[i + 2]});

    if (doc.ents.empty()) {
        Entity world;
        world.set("classname", "worldspawn");
        doc.ents.push_back(std::move(world));
    }
    doc.refMesh = combined;
    doc.dirty = true;
    doc.touch();
    error.clear();
    return true;
}

}  // namespace live
