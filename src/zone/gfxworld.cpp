#include "gfxworld.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include "light/lightbake.h"
#include "zone/skysource.h"

namespace gfx {
namespace {

constexpr int kCellXY = 32, kCellZ = 64;
constexpr double kGridOffset = 131072.0;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int gridCoord(double v, int size) { return static_cast<int>(std::floor((v + kGridOffset) / size)); }

// Gradient of the face's texture mapping within the face plane, as a world direction.
Vec3 texGradient(const Face& f, bool v) {
    Vec3 e1 = normalize(f.winding[1] - f.winding[0]);
    Vec3 e2 = normalize(cross(f.normal, e1));
    float u0, v0, u1, v1, u2, v2;
    faceUV(f, f.winding[0], u0, v0);
    faceUV(f, f.winding[0] + e1, u1, v1);
    faceUV(f, f.winding[0] + e2, u2, v2);
    double a = v ? (v1 - v0) : (u1 - u0), b = v ? (v2 - v0) : (u2 - u0);
    Vec3 g = e1 * a + e2 * b;
    return length(g) > 1e-9 ? normalize(g) : e1;
}

Image makeImage(const std::string& name, uint8_t mapType, uint8_t semantic, uint8_t category, uint16_t w, uint16_t h,
                uint8_t levelCount, uint32_t flags, uint32_t format, size_t bytes, uint8_t fill) {
    Image im;
    im.name = name;
    im.mapType = mapType;
    im.semantic = semantic;
    im.category = category;
    im.width = w;
    im.height = h;
    im.levelCount = levelCount;
    im.loadFlags = flags;
    im.format = format;
    im.pixels.assign(bytes, fill);
    return im;
}

// Where the light grid should cover: the drawn geometry (tool/sky/caulk faces are already left out
// of out.vertices), trimmed to its 2nd..98th percentile per axis so a few far-off faces don't
// inflate it. Falls back to the full brush bounds when there's too little drawn geometry.
AABB lightGridRegion(const std::vector<Vertex>& verts, const AABB& world) {
    if (verts.size() < 8) return world;
    AABB r;
    std::vector<float> v(verts.size());
    for (int k = 0; k < 3; k++) {
        for (size_t i = 0; i < verts.size(); i++) v[i] = verts[i].xyz[k];
        size_t lo = v.size() / 50, hi = v.size() - 1 - v.size() / 50;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(lo), v.end());
        r.mins[k] = v[lo];
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(hi), v.end());
        r.maxs[k] = v[hi];
    }
    return r.valid() ? r : world;
}

// Fills g.entries/g.colors from the bake. Entries run row by row (rowAxis y), then column (x),
// then z. Sampling every cell of a big grid is too slow, so cells are sampled on a coarser
// lattice (at most ~40k samples) and each takes its lattice cell's value.
// NOTE: the colour byte scale (128 = 1.0 linear) is unverified in-game.
void bakeLightGrid(const light::Bake& bake, const int lo[3], int cols, int rows, int zs, LightGrid& g) {
    int sx = 1, sy = 1, sz = 1;
    auto count = [&] {
        return static_cast<uint64_t>((cols + sx - 1) / sx) * ((rows + sy - 1) / sy) * ((zs + sz - 1) / sz);
    };
    while (count() > 40000) {
        if (sz < 2 || sx >= 2 * sz)
            sz *= 2;
        else
            sx *= 2, sy *= 2;
    }
    const int cs = (cols + sx - 1) / sx, rs = (rows + sy - 1) / sy, zss = (zs + sz - 1) / sz;
    std::vector<Vec3> pts;
    pts.reserve(static_cast<size_t>(cs) * rs * zss);
    for (int r = 0; r < rs; r++)
        for (int c = 0; c < cs; c++)
            for (int z = 0; z < zss; z++) {
                double cx = (lo[0] + std::min(c * sx + sx / 2, cols - 1) + 0.5) * kCellXY - kGridOffset;
                double cy = (lo[1] + std::min(r * sy + sy / 2, rows - 1) + 0.5) * kCellXY - kGridOffset;
                double cz = (lo[2] + std::min(z * sz + sz / 2, zs - 1) + 0.5) * kCellZ - kGridOffset;
                pts.push_back({cx, cy, cz});
            }
    std::vector<light::GridSample> samples = light::sampleGrid(bake, pts, 24);
    std::map<uint32_t, uint16_t> palette;
    std::vector<uint32_t> sampleEntry(samples.size());
    for (size_t i = 0; i < samples.size(); i++) {
        uint8_t rgb[3];
        for (int k = 0; k < 3; k++)
            rgb[k] = static_cast<uint8_t>(std::clamp(std::lround(samples[i].light[k] * 128.0), 0L, 255L));
        uint32_t key = rgb[0] | (rgb[1] << 8) | (rgb[2] << 16);
        auto it = palette.find(key);
        if (it == palette.end()) {
            if (g.colors.size() >= 65535) {
                it = palette.begin();  // palette full (never in practice): reuse the first colour
            } else {
                it = palette.emplace(key, static_cast<uint16_t>(g.colors.size())).first;
                g.colors.push_back({rgb[0], rgb[1], rgb[2]});
            }
        }
        sampleEntry[i] = it->second | ((samples[i].sun ? 1u : 0u) << 16);
    }
    g.entries.resize(static_cast<size_t>(rows) * cols * zs);
    size_t e = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            for (int z = 0; z < zs; z++)
                g.entries[e++] = sampleEntry[(static_cast<size_t>(r / sy) * cs + c / sx) * zss + z / sz];
    g.colorCount = static_cast<uint32_t>(g.colors.size());
}

void buildLightGrid(const AABB& world, LightGrid& g, std::string& error, const light::Bake* bake) {
    // rowAxis y, colAxis x, as in stock. One run per row covers every column with every z cell.
    int lo[3], hi[3];
    for (int k = 0; k < 3; k++) {
        int size = k == 2 ? kCellZ : kCellXY;
        lo[k] = gridCoord(world.mins[k] - size, size);
        hi[k] = gridCoord(world.maxs[k] + size, size);
    }
    int cols = hi[0] - lo[0] + 1, rows = hi[1] - lo[1] + 1, zs = hi[2] - lo[2] + 1;
    // This uniform grid fills every cell of its box (stock grids only store cells near geometry),
    // so its size is cols*rows*zs entries. Keep it stock-sized: shrink the z band first, then the
    // XY window, each kept centred on the geometry. Cells outside just aren't covered (the grid is
    // one uniform color anyway). Without this a big imported map needed billions of entries and its
    // VIRTUAL block overflowed 32 bits.
    constexpr uint64_t kMaxEntries = 4u << 20;  // 4M entries = 16 MB
    // The row RLE stores a column's z-cell count as a byte: at most 255 z-cells (~16320 units with
    // 64-unit cells), the same limit the game reports as "map too tall for lightgrid".
    int zCap = 255;
    uint64_t columns = static_cast<uint64_t>(cols) * static_cast<uint64_t>(rows);
    if (columns * static_cast<uint64_t>(std::min(zs, zCap)) > kMaxEntries)
        zCap = static_cast<int>(std::clamp<uint64_t>(kMaxEntries / std::max<uint64_t>(columns, 1), 4, 255));
    if (zs > zCap) {
        int mid = (lo[2] + hi[2]) / 2;
        lo[2] = mid - zCap / 2;
        hi[2] = lo[2] + zCap - 1;
        zs = zCap;
    }
    if (columns * static_cast<uint64_t>(zs) > kMaxEntries) {
        double scale = std::sqrt(static_cast<double>(kMaxEntries) / (static_cast<double>(columns) * zs));
        int newCols = std::max(1, static_cast<int>(cols * scale)), newRows = std::max(1, static_cast<int>(rows * scale));
        lo[0] += (cols - newCols) / 2;
        hi[0] = lo[0] + newCols - 1;
        lo[1] += (rows - newRows) / 2;
        hi[1] = lo[1] + newRows - 1;
        cols = newCols;
        rows = newRows;
    }
    for (int k = 0; k < 3; k++) {
        g.mins[k] = static_cast<uint16_t>(lo[k]);
        g.maxs[k] = static_cast<uint16_t>(hi[k]);
    }
    g.rowAxis = 1;
    g.colAxis = 0;
    uint32_t entry = 0;
    for (int r = 0; r < rows; r++) {
        if (g.rawRowData.size() / 4 > 0xFFFE) {
            error = "light grid row data too large";
            return;
        }
        g.rowDataStart.push_back(static_cast<uint16_t>(g.rawRowData.size() / 4));
        auto put16 = [&](uint16_t v) { g.rawRowData.push_back(v & 0xFF); g.rawRowData.push_back(v >> 8); };
        put16(static_cast<uint16_t>(lo[0]));
        put16(static_cast<uint16_t>(cols));
        put16(static_cast<uint16_t>(lo[2]));
        put16(static_cast<uint16_t>(zs));
        for (int b = 0; b < 4; b++) g.rawRowData.push_back(static_cast<uint8_t>(entry >> (8 * b)));
        for (int left = cols; left > 0; left -= 255) {
            g.rawRowData.push_back(static_cast<uint8_t>(std::min(left, 255)));
            g.rawRowData.push_back(static_cast<uint8_t>(zs));
            g.rawRowData.push_back(0);  // zStart relative to the row's zStart
        }
        while (g.rawRowData.size() % 4) g.rawRowData.push_back(0);
        entry += static_cast<uint32_t>(cols * zs);
    }
    g.entryCount = entry;
    if (bake && bake->scene) bakeLightGrid(*bake, lo, cols, rows, zs, g);
}

// Encodes the bake into IW4's lightmap pair (see light/lightbake.h for the shader formula):
//   primary   L8, 2W x 2H: direct-sun visibility
//   secondary A8R8G8B8, W x 2H: top half T, bottom half B; light = (T + B)^2 with both alphas
//             at the "no direction" value, so T = min(sqrt(E), 1) and B carries the overbright
//             rest (up to 4x).
void encodeLightmaps(const light::Bake& b, Image& primary, Image& secondary) {
    const int W = b.width, H = b.height;
    primary = makeImage("*lightmap0_primary", 3, 1, 2, static_cast<uint16_t>(W * 2), static_cast<uint16_t>(H * 2), 1, 2,
                        0x32 /*L8*/, static_cast<size_t>(W) * H * 4, 0);
    primary.pixels = b.sunVis;
    secondary = makeImage("*lightmap0_secondary", 3, 1, 2, static_cast<uint16_t>(W), static_cast<uint16_t>(H * 2), 1, 2,
                          0x15 /*A8R8G8B8*/, static_cast<size_t>(W) * H * 2 * 4, 0);
    // d = a * 4.08 - 2.08 (x) / a * 4.0645 - 2.0645 (y): ~zero at byte 130.
    constexpr uint8_t kNoDir = 130;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const float* e = &b.light[(static_cast<size_t>(y) * W + x) * 3];
            uint8_t* top = &secondary.pixels[(static_cast<size_t>(y) * W + x) * 4];
            uint8_t* bot = &secondary.pixels[(static_cast<size_t>(y + H) * W + x) * 4];
            for (int k = 0; k < 3; k++) {
                double s = std::sqrt(std::max(0.0f, e[k]));
                double t = std::min(s, 1.0), bb = std::clamp(s - 1.0, 0.0, 1.0);
                // A8R8G8B8 is B, G, R, A in memory.
                top[2 - k] = static_cast<uint8_t>(std::lround(t * 255));
                bot[2 - k] = static_cast<uint8_t>(std::lround(bb * 255));
            }
            top[3] = bot[3] = kNoDir;
        }
}

}  // namespace

uint32_t packUnitVec(const Vec3& v) {
    auto enc = [](double c) {
        long q = std::lround(c * 127.0 + 127.0);
        return static_cast<uint32_t>(std::clamp<long>(q, 0, 254));
    };
    return enc(v.x) | (enc(v.y) << 8) | (enc(v.z) << 16) | (63u << 24);
}

Vec3 unpackUnitVec(uint32_t p) {
    double scale = (static_cast<double>(p >> 24) + 192.0) / 32385.0;
    return {((p & 0xFF) - 127.0) * scale, (((p >> 8) & 0xFF) - 127.0) * scale, (((p >> 16) & 0xFF) - 127.0) * scale};
}

bool isToolMaterial(const std::string& raw) {
    std::string n = lower(raw);
    if (n.rfind("wc/", 0) == 0) n = n.substr(3);
    for (const char* t : {"caulk", "clip", "nodraw", "trigger", "hint", "skip", "portal", "origin", "volume"})
        if (n.find(t) != std::string::npos) return true;
    return n.rfind("sky", 0) == 0;
}

bool buildGfxWorldData(const Document& doc, GfxWorldData& out, std::string& error, const light::Bake* bake,
                       bool includeSky) {
    out = GfxWorldData{};
    if (doc.ents.empty()) {
        error = "map has no worldspawn";
        return false;
    }
    // Faces to draw, grouped by material (std::map keeps the material order stable). Sky faces
    // all share the embedded sky material.
    std::map<std::string, std::vector<std::pair<Brush, size_t>>> byMaterial;
    AABB world;
    std::string skySource;  // first sky face's material, picks the sky image
    for (const Brush& src : doc.ents[0].brushes) {
        Brush b = src;
        if (!b.rebuild()) continue;
        world.add(b.bounds);
        for (size_t i = 0; i < b.faces.size(); i++) {
            const Face& f = b.faces[i];
            if (f.winding.size() < 3) continue;
            if (light::isSkyMaterial(f.material)) {
                if (!includeSky) continue;
                if (skySource.empty() || lower(skySource) == "sky") skySource = f.material;
                byMaterial[kSkyMaterial].push_back({b, i});
                continue;
            }
            if (isToolMaterial(f.material)) continue;
            byMaterial[f.material].push_back({b, i});
        }
    }
    if (includeSky && byMaterial.count(kSkyMaterial)) {
        // worldspawn "sky" picks the sky by name (e.g. "sky_mp_boneyard"); else the sky faces' material.
        const std::string* key = doc.ents[0].find("sky");
        out.skyImage = sky::imageForMaterial(key && !key->empty() ? *key : skySource);
    }
    if (!world.valid()) world = AABB{{-64, -64, -64}, {64, 64, 64}};

    for (auto& [material, faces] : byMaterial) {
        uint32_t matIndex = static_cast<uint32_t>(out.materials.size());
        out.materials.push_back(material);
        const bool skyGroup = material == kSkyMaterial;
        if (skyGroup) out.skyMaterial = static_cast<int>(matIndex);
        Surface* s = nullptr;
        for (auto& [brush, fi] : faces) {
            const Face& f = brush.faces[fi];
            size_t n = f.winding.size();
            if (!s || s->vertexCount + n > 65535) {
                out.surfaces.push_back(Surface{});
                s = &out.surfaces.back();
                s->firstVertex = static_cast<uint32_t>(out.vertices.size());
                s->baseIndex = static_cast<uint32_t>(out.indices.size());
                s->material = matIndex;
                s->sky = skyGroup;
                if (skyGroup) out.skySurfaces.push_back(static_cast<uint32_t>(out.surfaces.size() - 1));
                for (int k = 0; k < 3; k++) {
                    s->mins[k] = 1e30f;
                    s->maxs[k] = -1e30f;
                }
            }
            Vec3 t = texGradient(f, false), bt = texGradient(f, true);
            float sign = dot(cross(f.normal, t), bt) < 0 ? -1.0f : 1.0f;
            const light::FaceMap* lm = (bake && !skyGroup) ? bake->find(f) : nullptr;
            if (bake && bake->env.hasSun && !skyGroup) s->primaryLight = 1;
            uint16_t base = s->vertexCount;
            for (const Vec3& p : f.winding) {
                Vertex v{};
                v.xyz[0] = static_cast<float>(p.x);
                v.xyz[1] = static_cast<float>(p.y);
                v.xyz[2] = static_cast<float>(p.z);
                v.binormalSign = sign;
                v.color = 0xFFFFFFFFu;
                faceUV(f, p, v.texCoord[0], v.texCoord[1]);
                if (lm)
                    bake->lmapCoord(*lm, p, v.lmapCoord[0], v.lmapCoord[1]);
                else
                    v.lmapCoord[0] = v.lmapCoord[1] = 0.5f;  // uniform lightmap (or an unbaked face)
                v.normal = packUnitVec(f.normal);
                v.tangent = packUnitVec(t);
                out.vertices.push_back(v);
                for (int k = 0; k < 3; k++) {
                    s->mins[k] = std::min(s->mins[k], v.xyz[k]);
                    s->maxs[k] = std::max(s->maxs[k], v.xyz[k]);
                }
            }
            // Fan over the winding, which is clockwise seen from outside.
            for (size_t k = 1; k + 1 < n; k++) {
                out.indices.push_back(base);
                out.indices.push_back(static_cast<uint16_t>(base + k));
                out.indices.push_back(static_cast<uint16_t>(base + k + 1));
                s->triCount++;
            }
            s->vertexCount = static_cast<uint16_t>(s->vertexCount + n);
        }
    }
    if (out.surfaces.size() > 65535) {
        error = "too many world surfaces (>65535)";
        return false;
    }
    for (int k = 0; k < 3; k++) {
        out.mins[k] = static_cast<float>(world.mins[k]);
        out.maxs[k] = static_cast<float>(world.maxs[k]);
    }

    // Lightmaps: baked, or (no bake) a tiny uniform pair whose pixel layout cannot matter.
    // Formats and flags copied from stock. The reflection probe stays a tiny uniform cube.
    if (bake) {
        encodeLightmaps(*bake, out.lightmapPrimary, out.lightmapSecondary);
    } else {
        out.lightmapPrimary = makeImage("*lightmap0_primary", 3, 1, 2, 4, 2, 1, 2, 0x32 /*L8*/, 4 * 2, 0xFF);
        out.lightmapSecondary = makeImage("*lightmap0_secondary", 3, 1, 2, 2, 2, 1, 2, 0x15 /*A8R8G8B8*/, 2 * 2 * 4, 0x80);
    }
    out.reflectionProbe = makeImage("*reflection_probe0", 5, 1, 1, 4, 4, 0, 0x10000, 0x15, 6 * (16 + 4 + 1) * 4, 0x80);

    buildLightGrid(lightGridRegion(out.vertices, world), out.lightGrid, error, bake);
    return error.empty();
}

}  // namespace gfx
