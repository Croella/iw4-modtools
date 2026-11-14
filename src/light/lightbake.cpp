#include "light/lightbake.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "zone/gfxworld.h"

namespace light {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRayOffset = 0.5;  // start rays this far off the surface (world units)

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool readVec(const Entity& e, const char* key, Vec3& out) {
    const std::string* v = e.find(key);
    if (!v) return false;
    double a = 0, b = 0, c = 0;
    if (std::sscanf(v->c_str(), "%lf %lf %lf", &a, &b, &c) != 3) return false;
    out = {a, b, c};
    return true;
}
bool readNum(const Entity& e, const char* key, double& out) {
    const std::string* v = e.find(key);
    if (!v) return false;
    return std::sscanf(v->c_str(), "%lf", &out) == 1;
}
Vec3 mul(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

// ---------------------------------------------------------------------------------------------
// Ray tracing: float triangles in a median-split BVH.
struct F3 {
    float x, y, z;
};
F3 f3(const Vec3& v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }

struct Tri {
    F3 a, e1, e2;
    int face;  // index into Scene::maps (-1: blocks light but isn't lightmapped, e.g. caulk)
    bool sky;
};
struct Node {
    float mn[3], mx[3];
    int start, count;  // leaf: tris [start, start+count); inner: count 0, children start, start+1
};
struct Hit {
    float t;
    int tri;
};

}  // namespace

class Scene {
public:
    std::vector<Tri> tris;
    std::vector<Node> nodes;
    // Lightmapped faces, for bounce lookups: map + the direct light (sun + point lights) per luxel.
    std::vector<FaceMap> maps;
    std::vector<float> direct;
    int width = 0;
    double luxelSize = 16;

    void build() {
        std::vector<int> idx(tris.size());
        for (size_t i = 0; i < idx.size(); i++) idx[i] = static_cast<int>(i);
        nodes.clear();
        nodes.reserve(tris.size() * 2 + 1);
        nodes.push_back({});
        buildNode(0, idx, 0, static_cast<int>(idx.size()));
        std::vector<Tri> sorted(tris.size());
        for (size_t i = 0; i < idx.size(); i++) sorted[i] = tris[idx[i]];
        tris = std::move(sorted);
    }

    // Closest hit within (tmin, tmax); false if the ray escapes.
    bool trace(const Vec3& o, const Vec3& d, float tmax, Hit& hit) const { return walk(o, d, tmax, false, hit); }
    // True if anything blocks the segment.
    bool occluded(const Vec3& o, const Vec3& d, float tmax) const {
        Hit h;
        return walk(o, d, tmax, true, h);
    }

    // Direct light at a point on lit face `f` (luxel nearest to p).
    Vec3 directAt(int f, const Vec3& p) const {
        if (f < 0 || f >= static_cast<int>(maps.size()) || direct.empty()) return {};
        const FaceMap& m = maps[f];
        int x = static_cast<int>((p[m.axisU] - m.minU) / luxelSize);
        int y = static_cast<int>((p[m.axisV] - m.minV) / luxelSize);
        x = std::clamp(x, 0, m.w - 1) + m.x;
        y = std::clamp(y, 0, m.h - 1) + m.y;
        const float* c = &direct[(static_cast<size_t>(y) * width + x) * 3];
        return {c[0], c[1], c[2]};
    }

private:
    void bounds(const std::vector<int>& idx, int s, int e, float mn[3], float mx[3]) const {
        for (int k = 0; k < 3; k++) mn[k] = 1e30f, mx[k] = -1e30f;
        for (int i = s; i < e; i++) {
            const Tri& t = tris[idx[i]];
            const float* a = &t.a.x;
            for (int k = 0; k < 3; k++) {
                float p0 = a[k], p1 = a[k] + (&t.e1.x)[k], p2 = a[k] + (&t.e2.x)[k];
                mn[k] = std::min({mn[k], p0, p1, p2});
                mx[k] = std::max({mx[k], p0, p1, p2});
            }
        }
    }
    void buildNode(int n, std::vector<int>& idx, int s, int e) {
        Node node{};
        bounds(idx, s, e, node.mn, node.mx);
        if (e - s <= 4) {
            node.start = s;
            node.count = e - s;
            nodes[n] = node;
            return;
        }
        int axis = 0;
        float ext = -1;
        for (int k = 0; k < 3; k++)
            if (node.mx[k] - node.mn[k] > ext) ext = node.mx[k] - node.mn[k], axis = k;
        auto centroid = [&](int i) {
            const Tri& t = tris[i];
            const float* a = &t.a.x;
            return a[axis] * 3 + (&t.e1.x)[axis] + (&t.e2.x)[axis];
        };
        int mid = (s + e) / 2;
        std::nth_element(idx.begin() + s, idx.begin() + mid, idx.begin() + e,
                         [&](int a, int b) { return centroid(a) < centroid(b); });
        int left = static_cast<int>(nodes.size());
        nodes.push_back({});
        nodes.push_back({});
        node.start = left;
        node.count = 0;
        nodes[n] = node;
        buildNode(left, idx, s, mid);
        buildNode(left + 1, idx, mid, e);
    }
    static bool slab(const Node& n, const float o[3], const float inv[3], float tmax) {
        float t0 = 0, t1 = tmax;
        for (int k = 0; k < 3; k++) {
            float a = (n.mn[k] - o[k]) * inv[k], b = (n.mx[k] - o[k]) * inv[k];
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a);
            t1 = std::min(t1, b);
            if (t0 > t1) return false;
        }
        return true;
    }
    bool walk(const Vec3& ov, const Vec3& dv, float tmax, bool any, Hit& hit) const {
        if (nodes.empty() || tris.empty()) return false;
        const float o[3] = {static_cast<float>(ov.x), static_cast<float>(ov.y), static_cast<float>(ov.z)};
        const float d[3] = {static_cast<float>(dv.x), static_cast<float>(dv.y), static_cast<float>(dv.z)};
        float inv[3];
        for (int k = 0; k < 3; k++) inv[k] = std::fabs(d[k]) > 1e-12f ? 1.0f / d[k] : 1e30f;
        hit.t = tmax;
        hit.tri = -1;
        int stack[96];
        int sp = 0;
        stack[sp++] = 0;
        while (sp) {
            const Node& n = nodes[stack[--sp]];
            if (!slab(n, o, inv, hit.t)) continue;
            if (n.count == 0) {
                if (sp + 2 <= 96) {
                    stack[sp++] = n.start;
                    stack[sp++] = n.start + 1;
                }
                continue;
            }
            for (int i = n.start; i < n.start + n.count; i++) {
                const Tri& t = tris[i];
                // Moller-Trumbore, two-sided.
                float px = d[1] * t.e2.z - d[2] * t.e2.y, py = d[2] * t.e2.x - d[0] * t.e2.z,
                      pz = d[0] * t.e2.y - d[1] * t.e2.x;
                float det = t.e1.x * px + t.e1.y * py + t.e1.z * pz;
                if (std::fabs(det) < 1e-9f) continue;
                float id = 1.0f / det;
                float sx = o[0] - t.a.x, sy = o[1] - t.a.y, sz = o[2] - t.a.z;
                float u = (sx * px + sy * py + sz * pz) * id;
                if (u < 0 || u > 1) continue;
                float qx = sy * t.e1.z - sz * t.e1.y, qy = sz * t.e1.x - sx * t.e1.z, qz = sx * t.e1.y - sy * t.e1.x;
                float v = (d[0] * qx + d[1] * qy + d[2] * qz) * id;
                if (v < 0 || u + v > 1) continue;
                float tt = (t.e2.x * qx + t.e2.y * qy + t.e2.z * qz) * id;
                if (tt <= 1e-3f || tt >= hit.t) continue;
                hit.t = tt;
                hit.tri = i;
                if (any) return true;
            }
        }
        return hit.tri >= 0;
    }
};

namespace {

// A face being lightmapped.
struct LitFace {
    Face face;
    FaceMap map;
    std::vector<double> pu, pv;  // winding projected onto (axisU, axisV)
    double cu = 0, cv = 0;       // centroid in that projection
};

// World point on the face plane at projected coordinates (u, v), moved inside the winding if it
// falls outside (luxels on the border would otherwise sample inside neighbouring solids).
Vec3 facePoint(const LitFace& lf, double u, double v, double inset) {
    const Face& f = lf.face;
    const int a = lf.map.axisU, b = lf.map.axisV, c = 3 - a - b;
    const size_t n = lf.pu.size();
    // Winding orientation in this projection, then inside test.
    double area = 0;
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        area += lf.pu[i] * lf.pv[j] - lf.pu[j] * lf.pv[i];
    }
    const double sgn = area < 0 ? -1 : 1;
    bool inside = true;
    for (size_t i = 0; i < n && inside; i++) {
        size_t j = (i + 1) % n;
        double cr = (lf.pu[j] - lf.pu[i]) * (v - lf.pv[i]) - (lf.pv[j] - lf.pv[i]) * (u - lf.pu[i]);
        if (cr * sgn < 0) inside = false;
    }
    if (!inside) {
        double best = 1e300, bu = u, bv = v;
        for (size_t i = 0; i < n; i++) {
            size_t j = (i + 1) % n;
            double ex = lf.pu[j] - lf.pu[i], ey = lf.pv[j] - lf.pv[i];
            double len2 = ex * ex + ey * ey;
            double t = len2 > 0 ? std::clamp(((u - lf.pu[i]) * ex + (v - lf.pv[i]) * ey) / len2, 0.0, 1.0) : 0;
            double qx = lf.pu[i] + ex * t, qy = lf.pv[i] + ey * t;
            double d2 = (qx - u) * (qx - u) + (qy - v) * (qy - v);
            if (d2 < best) best = d2, bu = qx, bv = qy;
        }
        u = bu;
        v = bv;
    }
    // Pull slightly toward the centroid so the sample isn't exactly on an edge.
    double dx = lf.cu - u, dy = lf.cv - v, dl = std::sqrt(dx * dx + dy * dy);
    if (dl > 1e-9) {
        double s = std::min(inset, dl) / dl;
        u += dx * s;
        v += dy * s;
    }
    Vec3 p;
    p[a] = u;
    p[b] = v;
    p[c] = (f.dist - f.normal[a] * u - f.normal[b] * v) / f.normal[c];
    return p;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    double next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (s >> 11) * (1.0 / 9007199254740992.0);
    }
};

void basis(const Vec3& n, Vec3& t, Vec3& b) {
    t = normalize(std::fabs(n.z) < 0.9 ? cross(n, {0, 0, 1}) : cross(n, {1, 0, 0}));
    b = cross(n, t);
}

// Cosine-weighted direction around n, stratified by (i, count).
Vec3 cosineDir(const Vec3& n, const Vec3& t, const Vec3& b, int i, int count, Rng& rng) {
    int side = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(count))));
    double u1 = ((i % side) + rng.next()) / side, u2 = (((i / side) % side) + rng.next()) / side;
    double r = std::sqrt(u1), phi = 2 * kPi * u2;
    double x = r * std::cos(phi), y = r * std::sin(phi), z = std::sqrt(std::max(0.0, 1 - u1));
    return normalize(t * x + b * y + n * z);
}

Vec3 pointLights(const Scene& sc, const std::vector<PointLight>& lights, const Vec3& p, const Vec3* n) {
    Vec3 sum;
    for (const PointLight& L : lights) {
        Vec3 d = L.origin - p;
        double dist = length(d);
        if (dist >= L.radius || dist < 1e-6) continue;
        Vec3 l = d * (1.0 / dist);
        double ndl = n ? dot(*n, l) : 1.0;
        if (ndl <= 0) continue;
        double f = 1 - (dist * dist) / (L.radius * L.radius);
        double atten = f * f;
        if (sc.occluded(p, l, static_cast<float>(dist - 1.0))) continue;
        sum += L.color * (atten * ndl);
    }
    return sum;
}

bool sunVisible(const Scene& sc, const Environment& env, const Vec3& p) {
    Hit h;
    if (!sc.trace(p, env.sunDir, 1e6f, h)) return true;  // escaped the world: open sky
    return sc.tris[h.tri].sky;
}

// Runs fn(i) for i in [0, n) on all cores; returns false if cancelled.
template <class Fn>
bool parallelFor(size_t n, Progress* progress, float from, float to, Fn fn) {
    std::atomic<size_t> next{0}, done{0};
    unsigned workers = std::max(1u, std::thread::hardware_concurrency());
    auto run = [&] {
        for (;;) {
            if (progress && progress->cancel) return;
            size_t i = next++;
            if (i >= n) return;
            fn(i);
            size_t d = ++done;
            if (progress && (d & 15) == 0) progress->fraction = from + (to - from) * static_cast<float>(d) / n;
        }
    };
    std::vector<std::thread> pool;
    for (unsigned w = 1; w < workers; w++) pool.emplace_back(run);
    run();
    for (std::thread& t : pool) t.join();
    if (progress) progress->fraction = to;
    return !(progress && progress->cancel);
}

int pow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

// Shelf-packs the face rects (w/h already set) into an atlas at most maxDim on a side, trying
// shelf widths from about sqrt(area) up so the atlas comes out roughly square.
bool pack(std::vector<LitFace>& faces, int maxDim, int& outW, int& outH) {
    std::vector<size_t> order(faces.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return faces[a].map.h != faces[b].map.h ? faces[a].map.h > faces[b].map.h : faces[a].map.w > faces[b].map.w;
    });
    double area = 0;
    int widest = 1;
    for (const LitFace& f : faces) {
        if (f.map.w > maxDim || f.map.h > maxDim) return false;
        area += static_cast<double>(f.map.w) * f.map.h;
        widest = std::max(widest, f.map.w);
    }
    for (int shelfW = std::max(pow2(widest), pow2(static_cast<int>(std::sqrt(area)))); shelfW <= maxDim; shelfW *= 2) {
        int x = 0, y = 0, shelf = 0, usedW = 1;
        bool ok = true;
        for (size_t i : order) {
            FaceMap& m = faces[i].map;
            if (x + m.w > shelfW) {
                x = 0;
                y += shelf;
                shelf = 0;
            }
            m.x = x;
            m.y = y;
            x += m.w;
            shelf = std::max(shelf, m.h);
            usedW = std::max(usedW, x);
            if (y + shelf > maxDim) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        outW = pow2(usedW);
        outH = pow2(std::max(1, y + shelf));
        if (outH <= shelfW * 2 || shelfW >= maxDim) return true;  // squarish enough
    }
    return false;
}

}  // namespace

Environment readEnvironment(const Document& doc) {
    Environment env;
    Vec3 angles{-50, 30, 0}, sunTint{1, 0.96, 0.88}, skyTint{0.75, 0.84, 1.0}, fillTint{1, 1, 1};
    double sunlight = 1.0, diffuse = 0.35, ambient = 0.06;
    if (!doc.ents.empty()) {
        const Entity& w = doc.ents[0];
        readVec(w, "sundirection", angles);
        readVec(w, "suncolor", sunTint);
        readVec(w, "sundiffusecolor", skyTint);
        readVec(w, "_color", fillTint);
        readNum(w, "sunlight", sunlight);
        readNum(w, "diffusefraction", diffuse);
        readNum(w, "ambient", ambient);
    }
    diffuse = std::clamp(diffuse, 0.0, 1.0);
    // CoD AnglesToForward (pitch down positive): the forward vector points at the sun.
    double p = angles.x * kPi / 180, y = angles.y * kPi / 180;
    env.sunDir = normalize(Vec3{std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)});
    env.hasSun = sunlight > 0 && env.sunDir.z > -0.99;
    env.sunColor = env.hasSun ? sunTint * (sunlight * (1 - diffuse)) : Vec3{};
    env.skyColor = skyTint * (std::max(0.0, sunlight) * diffuse);
    env.ambient = fillTint * std::max(0.0, ambient);
    return env;
}

Volumetric readVolumetric(const Document& doc) {
    Volumetric v;
    const Environment env = readEnvironment(doc);
    const Entity* ws = doc.ents.empty() ? nullptr : &doc.ents[0];
    // worldspawn number: volumetric_<name> (older bare spelling still read), else the default.
    auto num = [&](const char* name, double def) {
        double x = def;
        if (ws && !readNum(*ws, (std::string("volumetric_") + name).c_str(), x)) readNum(*ws, name, x);
        return x;
    };
    Vec3 sunTint{1, 0.96, 0.88};
    if (ws) readVec(*ws, "suncolor", sunTint);
    v.color = sunTint;
    if (ws) readVec(*ws, "volumetric_color", v.color);
    double on = 1;
    if (ws) readNum(*ws, "volumetric", on);
    v.hasSun = env.hasSun;
    v.sunDir = env.sunDir;
    v.fog = num("fog", 0) != 0;
    v.enabled = on != 0 && (env.hasSun || v.fog);
    v.shafts = env.hasSun && num("shafts", 1) != 0;
    v.intensity = num("intensity", v.intensity);
    v.density = num("density", v.density);
    v.decay = num("decay", v.decay);
    v.threshold = num("threshold", v.threshold);
    v.glow = num("glow", v.glow);
    v.glowRadius = num("glowradius", v.glowRadius);
    v.skyMode = static_cast<int>(num("skymode", v.skyMode));
    v.skyDepth = num("skydepth", v.skyDepth);
    v.debug = static_cast<int>(num("debug", v.debug));
    v.fogDensity = num("fog_density", v.fogDensity);
    v.fogFalloff = num("fog_falloff", v.fogFalloff);
    v.fogHeight = num("fog_height", v.fogHeight);
    v.fogSun = env.hasSun ? num("fog_sun", v.fogSun) : 0;
    v.fogG = num("fog_anisotropy", v.fogG);
    v.fogStart = num("fog_start", v.fogStart);
    v.fogMax = num("fog_max", v.fogMax);
    v.fogSky = num("fog_sky", v.fogSky);
    v.fogSkyDist = num("fog_skydist", v.fogSkyDist);
    if (ws && !readVec(*ws, "volumetric_fog_color", v.fogColor)) readVec(*ws, "fog_color", v.fogColor);
    return v;
}

std::string volumetricConfig(const Document& doc) {
    const Volumetric v = readVolumetric(doc);
    char buf[512];
    std::string out = "// iw4-radiant volumetric light (sun shafts, height fog); edit live while the game runs\n";
    std::snprintf(buf, sizeof buf, "enabled %d\nsun %.6f %.6f %.6f\ncolor %.4f %.4f %.4f\n", v.enabled ? 1 : 0, v.sunDir.x, v.sunDir.y,
                  v.sunDir.z, v.color.x, v.color.y, v.color.z);
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "intensity %g\ndensity %g\ndecay %g\nthreshold %g\nglow %g\nglowradius %g\nskymode %d\nskydepth %g\ndebug %d\nshafts %d\n",
                  v.intensity, v.density, v.decay, v.threshold, v.glow, v.glowRadius, v.skyMode, v.skyDepth, v.debug, v.shafts ? 1 : 0);
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "// height fog\nfog %d\nfogdensity %g\nfogfalloff %g\nfogheight %g\nfogsun %g\nfogg %g\nfogstart %g\nfogmax %g\n"
                  "fogsky %g\nfogskydist %g\nfogcolor %.4f %.4f %.4f\n",
                  v.fog ? 1 : 0, v.fogDensity, v.fogFalloff, v.fogHeight, v.fogSun, v.fogG, v.fogStart, v.fogMax, v.fogSky, v.fogSkyDist,
                  v.fogColor.x, v.fogColor.y, v.fogColor.z);
    out += buf;
    // full_clip brushes stop the sun's shafts (the game doesn't draw them, so the DLL draws these
    // triangles into its sky mask).
    size_t tris = 0;
    for (const Entity& e : doc.ents)
        for (const Brush& br : e.brushes)
            for (const Face& f : br.faces) {
                if (f.winding.size() < 3 || !blocksVolumetricSun(f.material)) continue;
                if (!tris) out += "// sun occluders (full_clip faces): world-space triangles\n";
                for (size_t i = 1; i + 1 < f.winding.size(); i++, tris++) {
                    const Vec3 &a = f.winding[0], &b = f.winding[i], &c = f.winding[i + 1];
                    std::snprintf(buf, sizeof buf, "occluder %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n", a.x, a.y, a.z, b.x, b.y, b.z,
                                  c.x, c.y, c.z);
                    out += buf;
                }
            }
    return out;
}

std::vector<PointLight> readLights(const Document& doc) {
    std::vector<PointLight> out;
    for (size_t i = 1; i < doc.ents.size(); i++) {
        const Entity& e = doc.ents[i];
        if (lower(e.classname()) != "light") continue;
        PointLight L;
        if (!e.origin(L.origin)) continue;
        Vec3 color{1, 1, 1};
        readVec(e, "_color", color);
        double intensity = 1, radius = 0;
        readNum(e, "intensity", intensity);
        if (!readNum(e, "radius", radius)) readNum(e, "light", radius);
        L.radius = radius > 0 ? radius : 300;
        L.color = color * intensity;
        out.push_back(L);
    }
    return out;
}

bool isSkyMaterial(const std::string& raw) {
    std::string n = lower(raw);
    if (n.rfind("wc/", 0) == 0) n = n.substr(3);
    return n.rfind("sky", 0) == 0;
}

bool blocksVolumetricSun(const std::string& raw) {
    std::string n = lower(raw);
    if (n.rfind("wc/", 0) == 0) n = n.substr(3);
    return n.find("clip") != std::string::npos && n.find("full") != std::string::npos;  // clip_full / full_clip
}

bool blocksLight(const std::string& raw) {
    if (isSkyMaterial(raw)) return true;
    if (lower(raw).find("caulk") != std::string::npos) return true;
    return !gfx::isToolMaterial(raw);
}

bool inMaterialSet(const std::set<std::string>& set, const std::string& name) {
    if (set.empty()) return false;
    std::string n = lower(name);
    if (n.rfind("wc/", 0) == 0) n = n.substr(3);
    return set.count(n) > 0;
}

uint64_t faceKey(const Face& f) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 1099511628211ull;
    };
    for (char c : f.material) mix(static_cast<unsigned char>(c));
    for (const Vec3& p : f.winding)
        for (int k = 0; k < 3; k++) mix(static_cast<uint64_t>(static_cast<int64_t>(std::llround(p[k] * 8))));
    return h;
}

uint64_t sceneHash(const Document& doc) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 1099511628211ull;
    };
    auto mixStr = [&](const std::string& s) {
        for (char ch : s) mix(static_cast<unsigned char>(ch));
        mix(0xFF);
    };
    for (size_t e = 0; e < doc.ents.size(); e++) {
        const Entity& ent = doc.ents[e];
        if (e != 0 && lower(ent.classname()) != "light") continue;
        for (const KeyValue& kv : ent.kv) mixStr(kv.key), mixStr(kv.value);
        if (e == 0)
            for (const Brush& b : ent.brushes)
                for (const Face& f : b.faces) {
                    for (const Vec3& p : f.p)
                        for (int k = 0; k < 3; k++) mix(static_cast<uint64_t>(std::llround(p[k] * 8)));
                    mixStr(f.material);
                }
    }
    return h;
}

const FaceMap* Bake::find(const Face& f) const {
    auto it = faces.find(faceKey(f));
    return it == faces.end() ? nullptr : &it->second;
}

void Bake::lmapCoord(const FaceMap& m, const Vec3& p, float& u, float& v) const {
    double tx = m.x + (p[m.axisU] - m.minU) / luxelSize;
    double ty = m.y + (p[m.axisV] - m.minV) / luxelSize;
    u = static_cast<float>(tx / width);
    v = static_cast<float>(ty / height);
}

bool bake(const Document& doc, const Settings& settings, Bake& out, std::string& error, Progress* progress) {
    out = Bake{};
    out.docRevision = doc.revision;
    out.sceneHash = sceneHash(doc);
    out.env = readEnvironment(doc);
    if (doc.ents.empty()) {
        error = "map has no worldspawn";
        return false;
    }
    const Environment& env = out.env;
    const std::vector<PointLight> lights = readLights(doc);

    // Gather faces: lit ones get a lightmap, every light-blocking one goes into the scene.
    auto scene = std::make_shared<Scene>();
    std::vector<LitFace> lit;
    for (const Brush& src : doc.ents[0].brushes) {
        Brush b = src;
        if (!b.rebuild()) continue;
        for (const Face& f : b.faces) {
            if (f.winding.size() < 3 || !blocksLight(f.material) || inMaterialSet(settings.unlitMaterials, f.material))
                continue;
            int litIndex = -1;
            const bool sky = isSkyMaterial(f.material);
            if (!sky && !gfx::isToolMaterial(f.material)) {
                litIndex = static_cast<int>(lit.size());
                LitFace lf;
                lf.face = f;
                lit.push_back(std::move(lf));
            }
            for (size_t k = 1; k + 1 < f.winding.size(); k++) {
                Tri t;
                t.a = f3(f.winding[0]);
                t.e1 = f3(f.winding[k] - f.winding[0]);
                t.e2 = f3(f.winding[k + 1] - f.winding[0]);
                t.face = litIndex;
                t.sky = sky;
                scene->tris.push_back(t);
            }
        }
    }
    if (lit.empty()) {
        error = "nothing to light (no drawn faces in worldspawn)";
        return false;
    }
    scene->build();

    // Projection + size of every face's rect; grow the luxel until the atlas fits.
    double ls = std::max(1.0, settings.luxelSize);
    for (int attempt = 0;; attempt++) {
        for (LitFace& lf : lit) {
            const Vec3& n = lf.face.normal;
            int drop = std::fabs(n.x) > std::fabs(n.y) ? (std::fabs(n.x) > std::fabs(n.z) ? 0 : 2)
                                                       : (std::fabs(n.y) > std::fabs(n.z) ? 1 : 2);
            lf.map.axisU = drop == 0 ? 1 : 0;
            lf.map.axisV = drop == 2 ? 1 : 2;
            double mnU = 1e300, mxU = -1e300, mnV = 1e300, mxV = -1e300;
            lf.pu.clear();
            lf.pv.clear();
            lf.cu = lf.cv = 0;
            for (const Vec3& p : lf.face.winding) {
                double u = p[lf.map.axisU], v = p[lf.map.axisV];
                lf.pu.push_back(u);
                lf.pv.push_back(v);
                lf.cu += u;
                lf.cv += v;
                mnU = std::min(mnU, u), mxU = std::max(mxU, u), mnV = std::min(mnV, v), mxV = std::max(mxV, v);
            }
            lf.cu /= lf.pu.size();
            lf.cv /= lf.pv.size();
            int u0 = static_cast<int>(std::floor(mnU / ls)) - 1, u1 = static_cast<int>(std::ceil(mxU / ls)) + 1;
            int v0 = static_cast<int>(std::floor(mnV / ls)) - 1, v1 = static_cast<int>(std::ceil(mxV / ls)) + 1;
            lf.map.minU = u0 * ls;
            lf.map.minV = v0 * ls;
            lf.map.w = u1 - u0;
            lf.map.h = v1 - v0;
        }
        if (pack(lit, settings.maxAtlas, out.width, out.height)) break;
        if (attempt > 12) {
            error = "map too large to fit a lightmap atlas";
            return false;
        }
        ls *= 1.5;
    }
    out.luxelSize = ls;
    const int W = out.width, H = out.height;
    out.light.assign(static_cast<size_t>(W) * H * 3, 0.0f);
    out.sunVis.assign(static_cast<size_t>(W) * H * 4, 0);
    scene->width = W;
    scene->luxelSize = ls;
    scene->direct.assign(static_cast<size_t>(W) * H * 3, 0.0f);
    for (const LitFace& lf : lit) scene->maps.push_back(lf.map);

    // Pass 1: sun visibility at 2x density, then direct light (sun + point lights) per luxel.
    const bool sun = env.hasSun;
    auto pass1 = [&](size_t fi) {
        const LitFace& lf = lit[fi];
        const FaceMap& m = lf.map;
        const Vec3& n = lf.face.normal;
        const double ndlSun = std::max(0.0, dot(n, env.sunDir));
        for (int y = 0; y < m.h * 2; y++)
            for (int x = 0; x < m.w * 2; x++) {
                uint8_t vis = 0;
                if (sun && ndlSun > 0) {
                    Vec3 p = facePoint(lf, m.minU + (x + 0.5) * ls * 0.5, m.minV + (y + 0.5) * ls * 0.5, ls * 0.05) +
                             n * kRayOffset;
                    vis = sunVisible(*scene, env, p) ? 255 : 0;
                }
                out.sunVis[static_cast<size_t>(m.y * 2 + y) * (W * 2) + (m.x * 2 + x)] = vis;
            }
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                Vec3 p = facePoint(lf, m.minU + (x + 0.5) * ls, m.minV + (y + 0.5) * ls, ls * 0.05) + n * kRayOffset;
                size_t s0 = static_cast<size_t>(m.y * 2 + y * 2) * (W * 2) + (m.x * 2 + x * 2);
                double vis = (out.sunVis[s0] + out.sunVis[s0 + 1] + out.sunVis[s0 + W * 2] + out.sunVis[s0 + W * 2 + 1]) /
                             (4.0 * 255.0);
                Vec3 d = env.sunColor * (vis * ndlSun) + pointLights(*scene, lights, p, &n);
                float* o = &scene->direct[(static_cast<size_t>(m.y + y) * W + (m.x + x)) * 3];
                o[0] = static_cast<float>(d.x), o[1] = static_cast<float>(d.y), o[2] = static_cast<float>(d.z);
            }
    };
    if (!parallelFor(lit.size(), progress, 0.0f, 0.3f, pass1)) {
        error = "cancelled";
        return false;
    }

    // Pass 2+: sky dome and bounce, gathered with cosine-weighted hemisphere rays. Each bounce
    // re-gathers from (direct + previous indirect).
    std::vector<float> source = scene->direct, indirect(out.light.size(), 0.0f);
    const int samples = std::max(1, settings.skySamples);
    const int rounds = std::max(1, settings.bounces);
    for (int round = 0; round < rounds; round++) {
        auto gather = [&](size_t fi) {
            const LitFace& lf = lit[fi];
            const FaceMap& m = lf.map;
            const Vec3& n = lf.face.normal;
            Vec3 t, b;
            basis(n, t, b);
            Rng rng(fi * 7919 + round * 104729 + 1);
            for (int y = 0; y < m.h; y++)
                for (int x = 0; x < m.w; x++) {
                    Vec3 p = facePoint(lf, m.minU + (x + 0.5) * ls, m.minV + (y + 0.5) * ls, ls * 0.05) + n * kRayOffset;
                    Vec3 sky, bounce;
                    for (int i = 0; i < samples; i++) {
                        Vec3 dir = cosineDir(n, t, b, i, samples, rng);
                        Hit h;
                        if (!scene->trace(p, dir, 1e6f, h) || scene->tris[h.tri].sky) {
                            sky += env.skyColor;
                        } else if (settings.bounces > 0) {
                            const Tri& tr = scene->tris[h.tri];
                            if (tr.face >= 0) {
                                Vec3 hp = p + dir * h.t;
                                const FaceMap& hm = scene->maps[tr.face];
                                int hx = std::clamp(static_cast<int>((hp[hm.axisU] - hm.minU) / ls), 0, hm.w - 1) + hm.x;
                                int hy = std::clamp(static_cast<int>((hp[hm.axisV] - hm.minV) / ls), 0, hm.h - 1) + hm.y;
                                const float* c = &source[(static_cast<size_t>(hy) * W + hx) * 3];
                                bounce += Vec3{c[0], c[1], c[2]} * settings.albedo;
                            }
                        }
                    }
                    Vec3 e = (sky + bounce) * (1.0 / samples);
                    float* o = &indirect[(static_cast<size_t>(m.y + y) * W + (m.x + x)) * 3];
                    o[0] = static_cast<float>(e.x), o[1] = static_cast<float>(e.y), o[2] = static_cast<float>(e.z);
                }
        };
        float from = 0.3f + 0.7f * round / rounds, to = 0.3f + 0.7f * (round + 1) / rounds;
        if (!parallelFor(lit.size(), progress, from, to, gather)) {
            error = "cancelled";
            return false;
        }
        for (size_t i = 0; i < source.size(); i++) source[i] = scene->direct[i] + indirect[i];
    }

    // Final non-sun light: ambient + point lights + sky/bounce. (Direct sun stays out: the game
    // adds it in real time through the sun primary light and the visibility mask.)
    for (size_t fi = 0; fi < lit.size(); fi++) {
        const LitFace& lf = lit[fi];
        const FaceMap& m = lf.map;
        const double ndlSun = std::max(0.0, dot(lf.face.normal, env.sunDir));
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                size_t i = (static_cast<size_t>(m.y + y) * W + (m.x + x)) * 3;
                size_t s0 = static_cast<size_t>(m.y * 2 + y * 2) * (W * 2) + (m.x * 2 + x * 2);
                double vis = (out.sunVis[s0] + out.sunVis[s0 + 1] + out.sunVis[s0 + W * 2] + out.sunVis[s0 + W * 2 + 1]) /
                             (4.0 * 255.0);
                Vec3 sunPart = env.sunColor * (vis * ndlSun);
                for (int k = 0; k < 3; k++) {
                    double v = scene->direct[i + k] - sunPart[k] + indirect[i + k] + env.ambient[k];
                    out.light[i + k] = static_cast<float>(std::max(0.0, v));
                }
            }
        out.faces[faceKey(lf.face)] = m;
    }
    out.scene = scene;
    if (progress) progress->fraction = 1.0f;
    return true;
}

std::vector<GridSample> sampleGrid(const Bake& b, const std::vector<Vec3>& points, int rays) {
    std::vector<GridSample> out(points.size());
    if (!b.scene) return out;
    const Scene& sc = *b.scene;
    const Environment& env = b.env;
    // The light entities aren't kept in the bake, so the grid gets sky + bounce + ambient (point
    // lights reach models through the bounce they put on nearby surfaces).
    rays = std::max(4, rays);
    parallelFor(points.size(), nullptr, 0, 1, [&](size_t i) {
        const Vec3& p = points[i];
        Rng rng(i * 2654435761ull + 17);
        Vec3 sum;
        for (int r = 0; r < rays; r++) {
            double z = 1 - 2 * ((r + rng.next()) / rays), phi = 2 * kPi * rng.next();
            double s = std::sqrt(std::max(0.0, 1 - z * z));
            Vec3 dir{s * std::cos(phi), s * std::sin(phi), z};
            Hit h;
            if (!sc.trace(p, dir, 1e6f, h) || sc.tris[h.tri].sky)
                sum += env.skyColor;
            else if (sc.tris[h.tri].face >= 0)
                sum += sc.directAt(sc.tris[h.tri].face, p + dir * h.t) * 0.45;
        }
        out[i].light = sum * (1.0 / rays) + env.ambient;
        out[i].sun = env.hasSun && sunVisible(sc, env, p);
    });
    return out;
}

}  // namespace light
