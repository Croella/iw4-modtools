#include "gfxworld.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
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

constexpr size_t kMaxProbes = 64;  // reflection probes per map (64x64 cubes, 128 KB each)

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
    std::vector<light::GridSample> samples = light::sampleGrid(bake, pts);
    std::map<std::array<uint8_t, 168>, uint16_t> palette;
    std::vector<uint32_t> sampleEntry(samples.size());
    for (size_t i = 0; i < samples.size(); i++) {
        std::array<uint8_t, 168> rgb{};
        for (size_t d = 0; d < 56; d++)
            for (int k = 0; k < 3; k++)
                rgb[d * 3 + k] =
                    static_cast<uint8_t>(std::clamp(std::lround(std::sqrt(std::max(0.0, samples[i].dir[d][k])) * 127.5), 0L, 255L));
        auto it = palette.find(rgb);
        if (it == palette.end()) {
            if (g.colors.size() >= 65535) {
                it = palette.begin();  // palette full (never in practice): reuse the first colour
            } else {
                it = palette.emplace(rgb, static_cast<uint16_t>(g.colors.size())).first;
                g.colors.push_back(rgb);
            }
        }
        sampleEntry[i] = it->second | (static_cast<uint32_t>(samples[i].primary & 0xFF) << 16);
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

// A lightmapped face's rect and its tangent frame (as the vertices carry it).
struct LmFrame {
    const light::FaceMap* map;
    Vec3 t, b, n;
};

// Directional encoding of one luxel (stock lit_lmap pixel shaders, normal-mapped variant):
//   light = ( T / |(n,1)| + B * saturate(dot(normalize(d,1), normalize(n,1))) )^2
// with d = (aT * 4.08 - 2.08, aB * 4.0645 - 2.0645) the tangent-space direction the light comes
// from (x along the tangent, y along sign * cross(normal, tangent), implied z 1), n the normal
// map's. A flat surface gets (T + B / |(d,1)|)^2. The share of sqrt(light) put in B is how
// directional the baked light is there (|sum of weighted directions| / total weight, ambient
// counted as undirected).
void encodeDirectional(const float* e, const float* ld, double ambientW, const LmFrame& fr, uint8_t* top, uint8_t* bot) {
    const Vec3 ds{ld[0], ld[1], ld[2]};
    const double len = length(ds), wt = ld[3] + ambientW;
    const double f = (wt > 1e-9 && len > 1e-9) ? std::min(1.0, len / wt) * 0.9 : 0.0;
    if (f < 0.02) return;  // leave it undirected
    Vec3 L = ds * (1.0 / len);
    double ln = dot(L, fr.n);
    if (ln < 0.25) {  // the format can't point below the surface: lift it
        L = normalize(L + fr.n * (0.25 - ln));
        ln = dot(L, fr.n);
    }
    const double dx = dot(L, fr.t) / ln, dy = dot(L, fr.b) / ln;
    const long aT = std::clamp<long>(std::lround((dx + 2.08) / 4.08 * 255), 0, 255);
    const long aB = std::clamp<long>(std::lround((dy + 2.0645) / 4.0645 * 255), 0, 255);
    const double qx = aT / 255.0 * 4.08 - 2.08, qy = aB / 255.0 * 4.0645 - 2.0645;
    const double c0 = 1 / std::sqrt(1 + qx * qx + qy * qy);  // what a flat surface gets of B
    for (int k = 0; k < 3; k++) {
        const double s = std::sqrt(std::max(0.0f, e[k]));
        double T = std::min(s * (1 - f), 1.0), Bv = (s - T) / c0;
        if (Bv > 1) {
            Bv = 1;
            T = std::clamp(s - c0, 0.0, 1.0);
        }
        top[2 - k] = static_cast<uint8_t>(std::lround(std::clamp(T, 0.0, 1.0) * 255));
        bot[2 - k] = static_cast<uint8_t>(std::lround(std::clamp(Bv, 0.0, 1.0) * 255));
    }
    top[3] = static_cast<uint8_t>(aT);
    bot[3] = static_cast<uint8_t>(aB);
}

// Encodes the bake into IW4's lightmap pair (see light/lightbake.h for the shader formula):
//   primary   L8, 2W x 2H: direct-sun visibility
//   secondary A8R8G8B8, W x 2H: top half T, bottom half B. Undirected luxels: light = (T + B)^2
//             with both alphas at the "no direction" value, so T = min(sqrt(E), 1) and B carries
//             the overbright rest (up to 4x). Luxels of the faces in `frames` are re-encoded with
//             a direction (encodeDirectional) when the bake recorded one.
void encodeLightmaps(const light::Bake& b, std::vector<Image>& primaries, std::vector<Image>& secondaries, const std::vector<LmFrame>& frames) {
    const int W = b.width, H = b.height;
    // The whole atlas first (T rows 0..H-1, B rows H..2H-1), then cut into pages.
    Image secondary;
    secondary.pixels.assign(static_cast<size_t>(W) * H * 2 * 4, 0);
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
    const bool directed = b.lightDir.size() == static_cast<size_t>(W) * H * 4;
    const double ambientW = b.env.ambient.x * 0.3 + b.env.ambient.y * 0.59 + b.env.ambient.z * 0.11;
    for (const LmFrame& fr : directed ? frames : std::vector<LmFrame>{}) {
        const light::FaceMap& m = *fr.map;
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                const size_t px = static_cast<size_t>(m.y + y) * W + (m.x + x);
                encodeDirectional(&b.light[px * 3], &b.lightDir[px * 4], ambientW, fr,
                                  &secondary.pixels[px * 4], &secondary.pixels[(px + static_cast<size_t>(W) * H) * 4]);
            }
    }
    // Pages: primary 2W x 2h from the sun mask rows, secondary W x 2h (the page's T rows over its B rows).
    const std::vector<int> pageY = b.pageY.empty() ? std::vector<int>{0} : b.pageY;
    const std::vector<int> pageH = b.pageH.empty() ? std::vector<int>{H} : b.pageH;
    primaries.clear();
    secondaries.clear();
    for (size_t p = 0; p < pageY.size(); p++) {
        const int y0 = pageY[p], h = pageH[p];
        const std::string n = "*lightmap" + std::to_string(p);
        Image pri = makeImage(n + "_primary", 3, 1, 2, static_cast<uint16_t>(W * 2), static_cast<uint16_t>(h * 2), 1, 2, 0x32 /*L8*/,
                              static_cast<size_t>(W) * h * 4, 0);
        std::memcpy(pri.pixels.data(), &b.sunVis[static_cast<size_t>(y0) * 2 * (W * 2)], static_cast<size_t>(W) * h * 4);
        Image sec = makeImage(n + "_secondary", 3, 1, 2, static_cast<uint16_t>(W), static_cast<uint16_t>(h * 2), 1, 2,
                              0x15 /*A8R8G8B8*/, static_cast<size_t>(W) * h * 2 * 4, 0);
        const size_t rowBytes = static_cast<size_t>(W) * 4;
        std::memcpy(sec.pixels.data(), &secondary.pixels[static_cast<size_t>(y0) * rowBytes], h * rowBytes);
        std::memcpy(sec.pixels.data() + h * rowBytes, &secondary.pixels[static_cast<size_t>(H + y0) * rowBytes], h * rowBytes);
        primaries.push_back(std::move(pri));
        secondaries.push_back(std::move(sec));
    }
}

}  // namespace

const Vec3 kKdopAxes[9] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, -1, 0}, {1, 0, 1}, {1, 0, -1}, {0, 1, 1}, {0, 1, -1}};

// ComPrimaryLight values + the light region: the k-DOP around the cone (apex + spherical cap
// out to the radius), sampled densely and padded a little.
PrimaryLight makePrimaryLight(const light::PointLight& L) {
    PrimaryLight p{};
    for (int k = 0; k < 3; k++) {
        p.origin[k] = static_cast<float>(L.origin[k]);
        p.dir[k] = static_cast<float>(-L.spotDir[k]);
        p.color[k] = static_cast<float>(L.color[k]);
    }
    p.radius = static_cast<float>(L.radius);
    p.cosOuter = static_cast<float>(L.cosOuter);
    p.cosInner = static_cast<float>(L.cosInner);
    const Vec3 s = normalize(L.spotDir);
    const Vec3 u = normalize(std::fabs(s.z) < 0.9 ? cross(s, Vec3{0, 0, 1}) : cross(s, Vec3{1, 0, 0})), v = cross(s, u);
    const double theta = std::acos(std::clamp(L.cosOuter, -1.0, 1.0));
    std::vector<Vec3> pts{{0, 0, 0}};
    for (int a = 0; a <= 16; a++)
        for (int b = 0; b < 48; b++) {
            const double al = theta * a / 16, ph = 2 * 3.14159265358979 * b / 48;
            pts.push_back((s * std::cos(al) + (u * std::cos(ph) + v * std::sin(ph)) * std::sin(al)) * L.radius);
        }
    for (int k = 0; k < 9; k++) {
        double lo = 1e300, hi = -1e300;
        for (const Vec3& q : pts) {
            const double d = dot(q, kKdopAxes[k]);
            lo = std::min(lo, d), hi = std::max(hi, d);
        }
        lo -= 8, hi += 8;
        p.kdopMid[k] = static_cast<float>((lo + hi) * 0.5);
        p.kdopHalf[k] = static_cast<float>((hi - lo) * 0.5);
    }
    return p;
}

// Each primary light's shadow casters: the drawn, lit surfaces whose bounds touch its region box.
void fillShadowCasters(GfxWorldData& gw) {
    for (PrimaryLight& p : gw.primaries) {
        p.shadowSurfs.clear();
        for (size_t i = 0; i < gw.surfaces.size() && i < 65535; i++) {
            const Surface& s = gw.surfaces[i];
            if (s.sky || s.unlit) continue;
            bool touch = true;
            for (int k = 0; k < 3; k++) {
                const float lo = p.origin[k] + p.kdopMid[k] - p.kdopHalf[k], hi = p.origin[k] + p.kdopMid[k] + p.kdopHalf[k];
                if (s.maxs[k] < lo || s.mins[k] > hi) touch = false;
            }
            if (touch) p.shadowSurfs.push_back(static_cast<uint16_t>(i));
        }
    }
}

void sortSurfaces(GfxWorldData& gw, const std::vector<uint8_t>& sortKeys) {
    auto key = [&](const Surface& s) { return s.material < sortKeys.size() ? sortKeys[s.material] : uint8_t{1}; };
    std::vector<uint32_t> order(gw.surfaces.size());
    for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t a, uint32_t b) { return key(gw.surfaces[a]) < key(gw.surfaces[b]); });
    std::vector<Surface> sorted;
    std::vector<uint32_t> newIndex(order.size());
    sorted.reserve(order.size());
    for (uint32_t i = 0; i < order.size(); i++) {
        newIndex[order[i]] = i;
        sorted.push_back(gw.surfaces[order[i]]);
    }
    gw.surfaces = std::move(sorted);
    for (uint32_t& s : gw.skySurfaces) s = newIndex[s];
    gw.opaqueSurfaceCount = 0;
    while (gw.opaqueSurfaceCount < gw.surfaces.size() && key(gw.surfaces[gw.opaqueSurfaceCount]) <= kLastOpaqueSortKey)
        gw.opaqueSurfaceCount++;
    fillShadowCasters(gw);
}

uint32_t packUnitVec(const Vec3& v) {
    auto enc = [](double c) {
        long q = std::lround(c * 127.0 + 127.0);
        return static_cast<uint32_t>(std::clamp<long>(q, 0, 254));
    };
    return enc(v.x) | (enc(v.y) << 8) | (enc(v.z) << 16) | (63u << 24);
}

Vec3 texGradientOf(const Face& f) { return texGradient(f, false); }
float binormalSignOf(const Face& f) {
    return dot(cross(f.normal, texGradient(f, false)), texGradient(f, true)) < 0 ? -1.0f : 1.0f;
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
                       bool includeSky, const std::set<std::string>* unlitMaterials) {
    out = GfxWorldData{};
    if (doc.ents.empty()) {
        error = "map has no worldspawn";
        return false;
    }
    // Faces to draw, grouped by material (std::map keeps the material order stable). Sky faces
    // all share the embedded sky material.
    std::map<std::string, std::vector<std::pair<Brush, size_t>>> byMaterial;
    std::vector<LmFrame> lmFrames;  // lightmapped faces' tangent frames (directional lightmap)
    // Reflection probes: reflection_probe entities; none: one at the first spawn point (48 up).
    std::vector<Vec3> probes;
    for (size_t i = 1; i < doc.ents.size() && probes.size() < kMaxProbes; i++) {
        Vec3 o;
        if (lower(doc.ents[i].classname()) == "reflection_probe" && doc.ents[i].origin(o)) probes.push_back(o);
    }
    if (probes.empty())
        for (size_t i = 1; i < doc.ents.size(); i++) {
            const std::string cn = lower(doc.ents[i].classname());
            Vec3 o;
            if ((cn.rfind("info_player", 0) == 0 || (cn.rfind("mp_", 0) == 0 && cn.find("spawn") != std::string::npos)) &&
                doc.ents[i].origin(o)) {
                probes.push_back(o + Vec3{0, 0, 48});
                break;
            }
        }
    AABB world;
    std::string skySource;  // first sky face's material, picks the sky image
    for (const Brush& src : doc.ents[0].brushes) {
        Brush b = src;
        if (!b.rebuild()) continue;
        world.add(b.bounds);
        // Two-sided brushes also emit every face mirrored (seen from inside the brush).
        Brush back;
        if (b.twoSided()) {
            back = b;
            for (Face& f : back.faces) f = mirroredFace(f);
        }
        for (size_t i = 0; i < b.faces.size(); i++) {
            const Face& f = b.faces[i];
            if (f.winding.size() < 3) continue;
            if (light::isSkyMaterial(f.material)) {
                if (!includeSky) continue;
                if (skySource.empty() || lower(skySource) == "sky") skySource = f.material;
                byMaterial[kSkyMaterial].push_back({b, i});
                if (!back.faces.empty()) byMaterial[kSkyMaterial].push_back({back, i});
                continue;
            }
            if (isToolMaterial(f.material)) continue;
            byMaterial[f.material].push_back({b, i});
            if (!back.faces.empty()) byMaterial[f.material].push_back({back, i});
        }
    }
    if (includeSky && byMaterial.count(kSkyMaterial)) {
        // worldspawn "sky" picks the sky by name (e.g. "sky_mp_boneyard"); else the sky faces' material.
        const std::string* key = doc.ents[0].find("sky");
        out.skyImage = sky::imageForMaterial(key && !key->empty() ? *key : skySource);
    }
    if (!world.valid()) world = AABB{{-64, -64, -64}, {64, 64, 64}};
    if (probes.empty()) probes.push_back(world.center());
    auto probeOf = [&](const Face& f) -> uint8_t {
        Vec3 c;
        for (const Vec3& p : f.winding) c += p;
        c = c * (1.0 / std::max<size_t>(1, f.winding.size()));
        size_t best = 0;
        for (size_t i = 1; i < probes.size(); i++)
            if (length(probes[i] - c) < length(probes[best] - c)) best = i;
        return static_cast<uint8_t>(best);
    };

    for (auto& [material, faces] : byMaterial) {
        uint32_t matIndex = static_cast<uint32_t>(out.materials.size());
        out.materials.push_back(material);
        const bool skyGroup = material == kSkyMaterial;
        const bool unlitGroup = unlitMaterials && light::inMaterialSet(*unlitMaterials, material);
        if (skyGroup) out.skyMaterial = static_cast<int>(matIndex);
        // Engine primary light of a face: its primary spotlight, else the sun (lightmapped faces).
        auto primaryOf = [&](const Face& f) -> uint8_t {
            if (!bake || skyGroup || unlitGroup) return 0;
            const light::FaceMap* m = bake->find(f);
            if (m && m->primary >= 0) return static_cast<uint8_t>(2 + m->primary);
            return bake->env.hasSun ? 1 : 0;
        };
        auto pageOf = [&](const Face& f) -> uint8_t {
            const light::FaceMap* m = (bake && !skyGroup && !unlitGroup) ? bake->find(f) : nullptr;
            return m ? static_cast<uint8_t>(m->page) : 0;
        };
        auto groupOf = [&](const Face& f) { return (primaryOf(f) * 256 + probeOf(f)) * 256 + pageOf(f); };
        std::stable_sort(faces.begin(), faces.end(), [&](const auto& a, const auto& b) {
            return groupOf(a.first.faces[a.second]) < groupOf(b.first.faces[b.second]);
        });
        Surface* s = nullptr;
        for (auto& [brush, fi] : faces) {
            const Face& f = brush.faces[fi];
            size_t n = f.winding.size();
            const uint8_t prim = primaryOf(f), probe = probeOf(f), page = pageOf(f);
            if (!s || s->vertexCount + n > 65535 || s->primaryLight != prim || s->probe != probe || s->lightmap != page) {
                out.surfaces.push_back(Surface{});
                s = &out.surfaces.back();
                s->firstVertex = static_cast<uint32_t>(out.vertices.size());
                s->baseIndex = static_cast<uint32_t>(out.indices.size());
                s->material = matIndex;
                s->sky = skyGroup;
                s->unlit = unlitGroup;
                if (skyGroup) out.skySurfaces.push_back(static_cast<uint32_t>(out.surfaces.size() - 1));
                for (int k = 0; k < 3; k++) {
                    s->mins[k] = 1e30f;
                    s->maxs[k] = -1e30f;
                }
            }
            Vec3 t = texGradient(f, false), bt = texGradient(f, true);
            float sign = dot(cross(f.normal, t), bt) < 0 ? -1.0f : 1.0f;
            const light::FaceMap* lm = (bake && !skyGroup && !unlitGroup) ? bake->find(f) : nullptr;
            if (lm) lmFrames.push_back({lm, t, cross(f.normal, t) * sign, f.normal});
            s->primaryLight = prim;
            s->probe = probe;
            s->lightmap = page;
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
                    bake->lmapCoordPage(*lm, p, v.lmapCoord[0], v.lmapCoord[1]);
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
        encodeLightmaps(*bake, out.lightmapPrimaries, out.lightmapSecondaries, lmFrames);
    } else {
        out.lightmapPrimaries = {makeImage("*lightmap0_primary", 3, 1, 2, 4, 2, 1, 2, 0x32 /*L8*/, 4 * 2, 0xFF)};
        out.lightmapSecondaries = {makeImage("*lightmap0_secondary", 3, 1, 2, 2, 2, 1, 2, 0x15 /*A8R8G8B8*/, 2 * 2 * 4, 0x80)};
    }
    for (size_t i = 0; i < probes.size(); i++) {
        const std::string pn = "*reflection_probe" + std::to_string(i);
        if (bake && bake->scene) {
            Image im = makeImage(pn, 5, 1, 1, 64, 64, 0, 0x10000, 0x15, 6 * 5461 * 4, 0);
            im.pixels = light::bakeReflectionProbe(*bake, probes[i], 64);
            out.reflectionProbes.push_back(std::move(im));
        } else {
            out.reflectionProbes.push_back(makeImage(pn, 5, 1, 1, 4, 4, 0, 0x10000, 0x15, 6 * (16 + 4 + 1) * 4, 0x80));
        }
        out.probeOrigins.push_back({static_cast<float>(probes[i].x), static_cast<float>(probes[i].y), static_cast<float>(probes[i].z)});
    }

    buildLightGrid(lightGridRegion(out.vertices, world), out.lightGrid, error, bake);
    if (bake)
        for (const light::PointLight& L : bake->primaries) out.primaries.push_back(makePrimaryLight(L));
    fillShadowCasters(out);
    return error.empty();
}

}  // namespace gfx
