#include "light/lightbake.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#include "light/gpubake.h"
#include "zone/clipmap.h"
#include "zone/gfxworld.h"

namespace light {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRayOffset = 0.5;  // start rays this far off the surface (world units)
constexpr size_t kMaxPrimaryLights = 32;  // primary spotlights per map (engine primary lights 2..)

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

double lum(const Vec3& c) { return c.x * 0.3 + c.y * 0.59 + c.z * 0.11; }

// One light at p, unshadowed: colour * falloff * cone * N.L (0 when out of range / behind).
Vec3 lightReach(const PointLight& L, const Vec3& p, const Vec3* n, Vec3* toLight = nullptr, double* distOut = nullptr) {
    Vec3 d = L.origin - p;
    double dist = length(d);
    if (dist >= L.radius || dist < 1e-6) return {};
    Vec3 l = d * (1.0 / dist);
    double ndl = n ? dot(*n, l) : 1.0;
    if (ndl <= 0) return {};
    double f = 1 - (dist * dist) / (L.radius * L.radius);
    double atten = f * f;
    if (L.spot) {  // smoothstep across the cone edge (same as the shafts)
        const double c = -dot(l, L.spotDir);
        const double t = std::clamp((c - L.cosOuter) / std::max(L.cosInner - L.cosOuter, 1e-6), 0.0, 1.0);
        atten *= t * t * (3 - 2 * t);
        if (atten <= 0) return {};
    }
    if (toLight) *toLight = l;
    if (distOut) *distOut = dist;
    return L.color * (atten * ndl);
}

// Sum of the lights at p (shadowed), except lights[skip]; dirOut / wOut (optional) gather the
// luminance-weighted directions towards them.
Vec3 pointLights(const Scene& sc, const std::vector<PointLight>& lights, const Vec3& p, const Vec3* n, Vec3* dirOut = nullptr,
                 double* wOut = nullptr, int skip = -1) {
    Vec3 sum;
    for (size_t i = 0; i < lights.size(); i++) {
        const PointLight& L = lights[i];
        if (!L.bake || static_cast<int>(i) == skip) continue;
        Vec3 l;
        double dist = 0;
        const Vec3 c = lightReach(L, p, n, &l, &dist);
        if (c.x <= 0 && c.y <= 0 && c.z <= 0) continue;
        if (sc.occluded(p, l, static_cast<float>(dist - 1.0))) continue;
        sum += c;
        if (dirOut) {
            const double w = lum(c);
            *dirOut += l * w;
            *wOut += w;
        }
    }
    return sum;
}

// One light at p with its shadow ray.
Vec3 oneLight(const Scene& sc, const PointLight& L, const Vec3& p, const Vec3& n) {
    Vec3 l;
    double dist = 0;
    const Vec3 c = lightReach(L, p, &n, &l, &dist);
    if ((c.x <= 0 && c.y <= 0 && c.z <= 0) || sc.occluded(p, l, static_cast<float>(dist - 1.0))) return {};
    return c;
}

// Shadow mask of a primary spotlight: p faces the light and nothing is in between (falloff and
// cone are the engine's, in real time).
bool spotVisible(const Scene& sc, const PointLight& L, const Vec3& p, const Vec3& n) {
    const Vec3 d = L.origin - p;
    const double dist = length(d);
    if (dist < 1e-6) return true;
    if (dot(n, d) <= 0) return false;
    return !sc.occluded(p, d * (1.0 / dist), static_cast<float>(dist - 1.0));
}

bool sunVisible(const Scene& sc, const Environment& env, const Vec3& p) {
    Hit h;
    if (!sc.trace(p, env.sunDir, 1e6f, h)) return true;  // escaped the world: open sky
    return sc.tris[h.tri].sky;
}

// The ray work of the bake on the GPU (gpubake.h): shadow masks, direct light, sky + bounce
// rounds, with the same sample points as the CPU passes. False (with a reason) to fall back to
// the CPU. primaryLight[j] = index in `lights` of primary spotlight j (FaceMap::primary).
bool gpuPasses(const std::vector<LitFace>& lit, Scene& scene, const Environment& env, const std::vector<PointLight>& lights,
               const std::vector<int>& primaryLight, const Settings& settings, double ls, int W, Bake& out, std::vector<float>& indirect,
               std::vector<float>& excl, std::vector<float>& dirDirect, std::vector<float>& dirIndirect, Progress* progress,
               std::string& why) {
    gpubake::Context gpu;
    std::string device;
    if (!gpu.init(device, why)) return false;
    std::vector<gpubake::Tri> tris(scene.tris.size());
    for (size_t i = 0; i < tris.size(); i++) {
        const Tri& t = scene.tris[i];
        tris[i] = {{t.a.x, t.a.y, t.a.z}, {t.e1.x, t.e1.y, t.e1.z}, {t.e2.x, t.e2.y, t.e2.z}, t.face, t.sky ? 1 : 0};
    }
    std::vector<gpubake::Node> nodes(scene.nodes.size());
    for (size_t i = 0; i < nodes.size(); i++) {
        const Node& n = scene.nodes[i];
        nodes[i] = {{n.mn[0], n.mn[1], n.mn[2]}, n.start, {n.mx[0], n.mx[1], n.mx[2]}, n.count};
    }
    std::vector<gpubake::Map> maps(scene.maps.size());
    for (size_t i = 0; i < maps.size(); i++) {
        const FaceMap& m = scene.maps[i];
        maps[i] = {m.x, m.y, m.w, m.h, m.axisU, m.axisV, static_cast<float>(m.minU), static_cast<float>(m.minV)};
    }
    if (!gpu.setScene(tris, nodes, maps, W, static_cast<float>(ls), why)) return false;
    struct TickCtx {
        Progress* p;
        float from, to;
    } tc{progress, 0, 0};
    auto tick = [](void* u, float f) -> bool {
        auto* t = static_cast<TickCtx*>(u);
        if (!t->p) return true;
        if (t->p->cancel) return false;
        t->p->fraction = t->from + (t->to - t->from) * f;
        return true;
    };
    auto f3v = [](const Vec3& v, float out[3]) {
        out[0] = static_cast<float>(v.x), out[1] = static_cast<float>(v.y), out[2] = static_cast<float>(v.z);
    };
    // lights index -> index in the GPU light list (only baked lights go up).
    std::vector<int> glIndex(lights.size(), -1);
    std::vector<gpubake::Light> gl;
    for (size_t i = 0; i < lights.size(); i++) {
        const PointLight& L = lights[i];
        if (!L.bake) continue;
        glIndex[i] = static_cast<int>(gl.size());
        gpubake::Light g{};
        f3v(L.origin, g.pos);
        g.radius = static_cast<float>(L.radius);
        f3v(L.color, g.color);
        g.spot = L.spot ? 1.0f : 0.0f;
        f3v(L.spotDir, g.dir);
        g.cosOuter = static_cast<float>(L.cosOuter);
        g.cosInner = static_cast<float>(L.cosInner);
        gl.push_back(g);
    }

    // Shadow masks (2x density): the sun's, or the face's primary spotlight's. Plus, for spot
    // faces, the sun at each luxel (baked into their lightmap). Untraced samples stay 0.
    std::vector<gpubake::VisSample> visS;
    std::vector<size_t> visAt;               // mask index, or SIZE_MAX - luxel for the spot faces' sun
    std::vector<uint8_t> luxSun;             // per luxel (spot faces): sun visible
    std::vector<gpubake::Sample> lux;
    std::vector<size_t> luxAt;
    std::vector<double> luxNdl;
    std::vector<int> luxSpot;                // lights index of the luxel's primary spotlight, or -1
    const float sd[3] = {static_cast<float>(env.sunDir.x), static_cast<float>(env.sunDir.y), static_cast<float>(env.sunDir.z)};
    for (const LitFace& lf : lit) {
        const FaceMap& m = lf.map;
        const Vec3& n = lf.face.normal;
        const int si = m.primary >= 0 ? primaryLight[m.primary] : -1;
        const double ndlSun = std::max(0.0, dot(n, env.sunDir));
        for (int y = 0; y < m.h * 2; y++)
            for (int x = 0; x < m.w * 2; x++) {
                if (si < 0 && (!env.hasSun || ndlSun <= 0)) continue;
                const Vec3 p = facePoint(lf, m.minU + (x + 0.5) * ls * 0.5, m.minV + (y + 0.5) * ls * 0.5, ls * 0.05) + n * kRayOffset;
                gpubake::VisSample v{};
                f3v(p, v.p);
                if (si >= 0) {
                    if (dot(n, lights[si].origin - p) <= 0) continue;
                    f3v(lights[si].origin, v.t);
                    v.point = 1;
                } else {
                    std::memcpy(v.t, sd, sizeof sd);
                }
                visS.push_back(v);
                visAt.push_back(static_cast<size_t>(m.y * 2 + y) * (W * 2) + (m.x * 2 + x));
            }
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                const Vec3 p = facePoint(lf, m.minU + (x + 0.5) * ls, m.minV + (y + 0.5) * ls, ls * 0.05) + n * kRayOffset;
                gpubake::Sample s{};
                f3v(p, s.p);
                f3v(n, s.n);
                s.skip = si >= 0 ? glIndex[si] : -1;
                if (si >= 0 && env.hasSun && ndlSun > 0) {
                    gpubake::VisSample v{};
                    f3v(p, v.p);
                    std::memcpy(v.t, sd, sizeof sd);
                    visS.push_back(v);
                    visAt.push_back(SIZE_MAX - lux.size());
                }
                lux.push_back(s);
                luxAt.push_back(static_cast<size_t>(m.y + y) * W + (m.x + x));
                luxNdl.push_back(ndlSun);
                luxSpot.push_back(si);
            }
    }
    luxSun.assign(lux.size(), 0);
    std::vector<uint8_t> vis;
    tc.from = 0.0f, tc.to = 0.1f;
    if (!gpu.sun(visS, vis, tick, &tc, why)) return false;
    const size_t maskSize = out.sunVis.size();
    for (size_t i = 0; i < visAt.size(); i++) {
        if (visAt[i] < maskSize) out.sunVis[visAt[i]] = vis[i];
        else luxSun[SIZE_MAX - visAt[i]] = vis[i] ? 1 : 0;
    }

    // Direct light: sun (mask, or the luxel's own sun ray on spot faces) + the lights. The part
    // the game adds in real time (sun, or the face's spotlight) goes to `excl`.
    std::vector<float> lrgb, lskip, ldir;
    tc.from = 0.1f, tc.to = 0.3f;
    if (!gpu.lights(lux, gl, lrgb, lskip, ldir, tick, &tc, why)) return false;
    for (size_t i = 0; i < lux.size(); i++) {
        const size_t px = luxAt[i];
        double v;
        if (luxSpot[i] < 0) {
            const size_t y = px / W, x = px % W;
            const size_t s0 = (y * 2) * (W * 2) + x * 2;
            v = (out.sunVis[s0] + out.sunVis[s0 + 1] + out.sunVis[s0 + W * 2] + out.sunVis[s0 + W * 2 + 1]) / (4.0 * 255.0);
        } else {
            v = luxSun[i];
        }
        const Vec3 sunPart = env.sunColor * (v * luxNdl[i]);
        for (int k = 0; k < 3; k++) {
            scene.direct[px * 3 + k] = static_cast<float>(sunPart[k] + lrgb[i * 3 + k] + lskip[i * 3 + k]);
            excl[px * 3 + k] = static_cast<float>(luxSpot[i] < 0 ? sunPart[k] : lskip[i * 3 + k]);
        }
        for (int k = 0; k < 4; k++) dirDirect[px * 4 + k] = ldir[i * 4 + k];
        if (luxSpot[i] >= 0) {  // the sun is baked on spot faces: it counts towards the direction
            const double w = lum(sunPart);
            for (int k = 0; k < 3; k++) dirDirect[px * 4 + k] += static_cast<float>(env.sunDir[k] * w);
            dirDirect[px * 4 + 3] += static_cast<float>(w);
        }
    }

    // Sky + bounce rounds, each gathering from direct + the previous round's indirect.
    std::vector<float> source = scene.direct;
    const float sky[3] = {static_cast<float>(env.skyColor.x), static_cast<float>(env.skyColor.y), static_cast<float>(env.skyColor.z)};
    const int rounds = std::max(1, settings.bounces);
    for (int round = 0; round < rounds; round++) {
        std::vector<float> g, gd;
        tc.from = 0.3f + 0.7f * round / rounds, tc.to = 0.3f + 0.7f * (round + 1) / rounds;
        if (!gpu.gather(lux, source, sky, std::max(1, settings.skySamples), static_cast<float>(settings.albedo), settings.bounces > 0,
                        static_cast<uint32_t>(round * 104729 + 1), g, gd, tick, &tc, why))
            return false;
        for (size_t i = 0; i < lux.size(); i++)
            for (int k = 0; k < 3; k++) indirect[luxAt[i] * 3 + k] = g[i * 3 + k];
        for (size_t i = 0; i < lux.size(); i++)
            for (int k = 0; k < 4; k++) dirIndirect[luxAt[i] * 4 + k] = gd[i * 4 + k];
        for (size_t i = 0; i < source.size(); i++) source[i] = scene.direct[i] + indirect[i];
    }
    out.device = device;
    return true;
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

// Packs into one page if it fits (pack), else shelf-packs page after page of maxDim x maxDim (the
// last one cut down to a power of two) up to maxPages; faces' y include their page's first row.
bool packPages(std::vector<LitFace>& faces, int maxDim, int maxPages, int& outW, int& outH, std::vector<int>& pageY,
               std::vector<int>& pageH) {
    pageY.clear();
    pageH.clear();
    if (pack(faces, maxDim, outW, outH)) {
        for (LitFace& f : faces) f.map.page = 0;
        pageY = {0};
        pageH = {outH};
        return true;
    }
    if (maxPages <= 1) return false;
    std::vector<size_t> order(faces.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return faces[a].map.h != faces[b].map.h ? faces[a].map.h > faces[b].map.h : faces[a].map.w > faces[b].map.w;
    });
    for (const LitFace& f : faces)
        if (f.map.w > maxDim || f.map.h > maxDim) return false;
    int page = 0, x = 0, y = 0, shelf = 0;
    std::vector<int> used(1, 0);
    for (size_t i : order) {
        FaceMap& m = faces[i].map;
        if (x + m.w > maxDim) {
            x = 0;
            y += shelf;
            shelf = 0;
        }
        if (y + m.h > maxDim) {
            if (++page >= maxPages) return false;
            x = y = shelf = 0;
            used.push_back(0);
        }
        m.x = x;
        m.y = y;
        m.page = page;
        x += m.w;
        shelf = std::max(shelf, m.h);
        used[page] = std::max(used[page], y + shelf);
    }
    outW = maxDim;
    outH = 0;
    for (size_t p = 0; p < used.size(); p++) {
        pageY.push_back(outH);
        pageH.push_back(p + 1 < used.size() ? maxDim : pow2(std::max(1, used[p])));
        outH += pageH.back();
    }
    for (LitFace& f : faces) f.map.y += pageY[f.map.page];
    return true;
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

// zone_source/luts/<name>.png (or <name> as a path relative to zone_source / the map, or absolute).
std::string gradeLutPath(const Document& doc, const std::string& name) {
    namespace fs = std::filesystem;
    if (name.empty()) return {};
    const fs::path mapDir = doc.path.empty() ? fs::path() : fs::path(doc.path).parent_path();
    std::vector<fs::path> tries;
    const fs::path n(name);
    if (n.is_absolute()) tries.push_back(n);
    for (const fs::path& base : {mapDir / "zone_source" / "luts", mapDir / "zone_source", mapDir}) {
        tries.push_back(base / n);
        if (!n.has_extension()) tries.push_back(base / (name + ".png")), tries.push_back(base / (name + ".tga"));
    }
    std::error_code ec;
    for (const fs::path& p : tries)
        if (fs::is_regular_file(p, ec)) return p.string();
    return {};
}

// The baked grade table, rebuilt only when the settings or the LUT image change.
std::shared_ptr<const GradeLut> cachedGradeLut(const Document& doc, const GradeParams& g, const std::string& lutName, std::string& warning) {
    namespace fs = std::filesystem;
    struct Entry {
        std::shared_ptr<const GradeLut> lut;
        std::string warning;
    };
    static std::mutex mx;
    static std::map<std::string, Entry> cache;
    const std::string path = gradeLutPath(doc, lutName);
    std::error_code ec;
    const long long stamp = path.empty() ? 0 : static_cast<long long>(fs::last_write_time(path, ec).time_since_epoch().count());
    char nums[400];
    std::snprintf(nums, sizeof nums, "%.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g|%lld", g.exposure, g.contrast,
                  g.saturation, g.temperature, g.tint, g.lift[0], g.lift[1], g.lift[2], g.gamma[0], g.gamma[1], g.gamma[2], g.gain[0],
                  g.gain[1], g.gain[2], stamp);
    const std::string key = std::string(nums) + "|" + lutName + "|" + path;
    std::lock_guard<std::mutex> l(mx);
    if (auto it = cache.find(key); it != cache.end()) {
        warning = it->second.warning;
        return it->second.lut;
    }
    GradeLut artist;
    std::string err;
    warning.clear();
    if (!lutName.empty()) {
        if (path.empty()) warning = "grade LUT '" + lutName + "' not found (looked in zone_source/luts)";
        else if (!loadStripLut(path, artist, err)) warning = err, artist = {};
    }
    if (cache.size() > 64) cache.clear();
    Entry& e = cache[key];
    e.lut = std::make_shared<const GradeLut>(bakeGradeLut(g, artist.size ? &artist : nullptr));
    e.warning = warning;
    return e.lut;
}

// Grade keys from one entity (worldspawn: prefix "volumetric_" first, then bare) into `g`; only
// keys that are present change it.
void readGradeKeys(const Entity& e, bool worldspawn, GradeParams& g, double& vignette, std::string& lutName) {
    auto num = [&](const char* name, double& x) {
        if (worldspawn && readNum(e, (std::string("volumetric_") + name).c_str(), x)) return;
        readNum(e, name, x);
    };
    auto vec3 = [&](const char* name, double* out) {
        Vec3 x{out[0], out[1], out[2]};
        if (!(worldspawn && readVec(e, (std::string("volumetric_") + name).c_str(), x))) readVec(e, name, x);
        out[0] = x.x, out[1] = x.y, out[2] = x.z;
    };
    num("grade_exposure", g.exposure);
    num("grade_contrast", g.contrast);
    num("grade_saturation", g.saturation);
    num("grade_temperature", g.temperature);
    num("grade_tint", g.tint);
    vec3("grade_lift", g.lift);
    vec3("grade_gamma", g.gamma);
    vec3("grade_gain", g.gain);
    num("grade_vignette", vignette);
    g.contrast = std::max(0.0, g.contrast);
    g.saturation = std::max(0.0, g.saturation);
    g.temperature = std::clamp(g.temperature, -1.0, 1.0);
    g.tint = std::clamp(g.tint, -1.0, 1.0);
    vignette = std::clamp(vignette, 0.0, 1.0);
    const std::string* lut = worldspawn ? e.find("volumetric_grade_lut") : nullptr;
    if (!lut) lut = e.find("grade_lut");
    if (lut) lutName = *lut;
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
    // Ambient occlusion.
    v.ssao = num("ssao", 0) != 0;
    v.ssaoRadius = std::max(1.0, num("ssao_radius", v.ssaoRadius));
    v.ssaoIntensity = std::max(0.0, num("ssao_intensity", v.ssaoIntensity));
    v.ssaoBias = std::clamp(num("ssao_bias", v.ssaoBias), 0.0, 0.95);
    v.ssaoNear = std::max(0.0, num("ssao_near", v.ssaoNear));
    v.ssaoDistance = std::max(v.ssaoNear + 1, num("ssao_distance", v.ssaoDistance));
    v.contact = num("contact", 0) != 0 && env.hasSun;
    v.contactIntensity = std::clamp(num("contact_intensity", v.contactIntensity), 0.0, 1.0);
    v.contactLength = std::clamp(num("contact_length", v.contactLength), 1.0, 512.0);
    v.contactThickness = std::max(1.0, num("contact_thickness", v.contactThickness));
    v.contactNear = std::max(0.0, num("contact_near", v.contactNear));
    v.contactDistance = std::max(v.contactNear + 64, num("contact_distance", v.contactDistance));
    // Colour grading: worldspawn is the base; trigger_grade volumes inherit it (or neutral when the
    // base is off) and override what they set.
    v.grade = num("grade", 0) != 0;
    if (ws && v.grade) readGradeKeys(*ws, true, v.gradeParams, v.vignette, v.gradeLutName);
    for (size_t i = 1; i < doc.ents.size(); i++) {
        const Entity& e = doc.ents[i];
        if (lower(e.classname()) != "trigger_grade" || e.brushes.empty()) continue;
        GradeVolume gv;
        const std::string* tn = e.find("targetname");
        gv.name = tn && !tn->empty() ? *tn : "trigger_grade #" + std::to_string(i);
        gv.params = v.gradeParams;
        gv.vignette = v.vignette;
        std::string lutName = v.gradeLutName;
        readGradeKeys(e, false, gv.params, gv.vignette, lutName);
        readNum(e, "blend", gv.blend);
        readNum(e, "blend_time", gv.blendTime);
        double pr = 0;
        readNum(e, "priority", pr);
        gv.priority = static_cast<int>(pr);
        gv.blend = std::max(0.0, gv.blend);
        gv.blendTime = std::max(0.0, gv.blendTime);
        for (const Brush& b : e.brushes) {
            gradevol::Brush pb;
            for (const Face& f : b.faces)
                pb.push_back({{static_cast<float>(f.normal.x), static_cast<float>(f.normal.y), static_cast<float>(f.normal.z)},
                              static_cast<float>(f.dist)});
            if (!pb.empty()) gv.brushes.push_back(std::move(pb));
        }
        std::string warn;
        gv.lut = cachedGradeLut(doc, gv.params, lutName, warn);
        if (!warn.empty() && v.gradeWarning.empty()) v.gradeWarning = gv.name + ": " + warn;
        v.gradeVolumes.push_back(std::move(gv));
    }
    std::stable_sort(v.gradeVolumes.begin(), v.gradeVolumes.end(), [](const GradeVolume& a, const GradeVolume& b) { return a.priority < b.priority; });
    if (v.gradeVolumes.size() > 16) v.gradeVolumes.resize(16);
    if (v.grade || !v.gradeVolumes.empty()) {
        std::string warn;
        v.gradeLut = cachedGradeLut(doc, v.gradeParams, v.gradeLutName, warn);
        if (!warn.empty()) v.gradeWarning = warn;
        v.grade = true;
    }
    // Depth of field.
    v.dof = num("dof", 0) != 0;
    v.dofAuto = num("dof_auto", 0) != 0;
    v.dofNearStart = std::max(0.0, num("dof_near_start", v.dofNearStart));
    v.dofNearEnd = std::max(0.0, num("dof_near_end", v.dofNearEnd));
    v.dofFarStart = std::max(0.0, num("dof_far_start", v.dofFarStart));
    v.dofFarEnd = std::max(v.dofFarStart, num("dof_far_end", v.dofFarEnd));
    v.dofNearBlur = std::clamp(num("dof_near_blur", v.dofNearBlur), 0.0, 32.0);
    v.dofFarBlur = std::clamp(num("dof_far_blur", v.dofFarBlur), 0.0, 32.0);
    v.dofWeapon = std::max(0.0, num("dof_weapon", v.dofWeapon));
    v.dofAutoSpeed = std::max(0.01, num("dof_auto_speed", v.dofAutoSpeed));
    // Sun flare + glare.
    v.flare = env.hasSun && num("flare", 0) != 0;
    v.flareIntensity = std::max(0.0, num("flare_intensity", v.flareIntensity));
    v.flareHalo = std::max(0.0, num("flare_halo", v.flareHalo));
    v.flareGhosts = std::max(0.0, num("flare_ghosts", v.flareGhosts));
    v.flareStreak = std::max(0.0, num("flare_streak", v.flareStreak));
    v.flareColor = v.color;
    if (ws) readVec(*ws, "volumetric_flare_color", v.flareColor);
    v.glare = num("glare", 0) != 0;
    v.glareThreshold = std::clamp(num("glare_threshold", v.glareThreshold), 0.0, 0.98);
    v.glareIntensity = std::max(0.0, num("glare_intensity", v.glareIntensity));
    v.glareRadius = std::clamp(num("glare_radius", v.glareRadius), 1.0, 64.0);
    v.glareSun = env.hasSun ? std::max(0.0, num("glare_sun", v.glareSun)) : 0;
    // Screen-space weather.
    v.weather = std::clamp(static_cast<int>(num("weather", 0)), 0, 2);
    if (v.weather == 2) v.weatherColor = {0.95, 0.96, 1.0}, v.weatherOpacity = 0.85;
    v.weatherIntensity = std::max(0.0, num("weather_intensity", v.weatherIntensity));
    v.weatherSpeed = std::max(0.0, num("weather_speed", v.weatherSpeed));
    v.weatherDistance = std::max(32.0, num("weather_distance", v.weatherDistance));
    v.weatherOpacity = std::clamp(num("weather_opacity", v.weatherOpacity), 0.0, 1.0);
    if (ws) readVec(*ws, "volumetric_weather_color", v.weatherColor);
    if (ws) {
        Vec3 wind{0, 0, 0};
        readVec(*ws, "volumetric_weather_wind", wind);
        v.weatherWind[0] = wind.x, v.weatherWind[1] = wind.y;
    }
    for (size_t i = 1; i < doc.ents.size() && v.weather; i++) {
        const Entity& e = doc.ents[i];
        if (lower(e.classname()) != "trigger_weather" || e.brushes.empty()) continue;
        WeatherVolume wv;
        const std::string* tn = e.find("targetname");
        wv.name = tn && !tn->empty() ? *tn : "trigger_weather #" + std::to_string(i);
        readNum(e, "weather_intensity", wv.intensity);
        readNum(e, "blend", wv.blend);
        readNum(e, "blend_time", wv.blendTime);
        wv.intensity = std::max(0.0, wv.intensity);
        wv.blend = std::max(0.0, wv.blend);
        wv.blendTime = std::max(0.0, wv.blendTime);
        for (const Brush& b : e.brushes) {
            gradevol::Brush pb;
            for (const Face& f : b.faces)
                pb.push_back({{static_cast<float>(f.normal.x), static_cast<float>(f.normal.y), static_cast<float>(f.normal.z)},
                              static_cast<float>(f.dist)});
            if (!pb.empty()) wv.brushes.push_back(std::move(pb));
        }
        if (!wv.brushes.empty() && v.weatherVolumes.size() < 16) v.weatherVolumes.push_back(std::move(wv));
    }
    // Local light shafts.
    v.lights = num("lights", 0) != 0;
    v.lightsIntensity = std::max(0.0, num("lights_intensity", v.lightsIntensity));
    v.lightsShadows = std::clamp(num("lights_shadows", v.lightsShadows), 0.0, 1.0);
    v.lightsMax = std::clamp(static_cast<int>(num("lights_max", v.lightsMax)), 1, 16);
    v.lightsDistance = std::max(64.0, num("lights_distance", v.lightsDistance));
    if (v.lights)
        for (const PointLight& L : readLights(doc)) {
            if (!L.shafts || L.shaftScale <= 0) continue;
            VolLight vl;
            vl.origin = L.origin;
            vl.color = L.color * L.shaftScale;
            vl.radius = L.radius;
            vl.spot = L.spot;
            vl.spotDir = L.spotDir;
            vl.cosOuter = L.cosOuter;
            vl.cosInner = L.cosInner;
            vl.flicker = L.flicker;
            v.volLights.push_back(vl);
        }
    if (v.volLights.empty()) v.lights = false;
    v.enabled = on != 0 && (env.hasSun || v.fog || v.ssao || v.grade || v.dof || v.glare || v.weather || v.lights);
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
    std::snprintf(buf, sizeof buf, "// ambient occlusion\nssao %d\nssaoradius %g\nssaointensity %g\nssaobias %g\nssaonear %g\nssaofar %g\n",
                  v.ssao ? 1 : 0, v.ssaoRadius, v.ssaoIntensity, v.ssaoBias, v.ssaoNear, v.ssaoDistance);
    out += buf;
    std::snprintf(buf, sizeof buf, "// contact shadows\ncontact %d\ncontactparm %g %g %g %g\ncontactnear %g\n", v.contact ? 1 : 0,
                  v.contactIntensity, v.contactLength, v.contactThickness, v.contactDistance, v.contactNear);
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "// depth of field\ndof %d\ndofnear %g %g\ndoffar %g %g\ndofblur %g %g\ndofweapon %g\ndofauto %d\ndofautospeed %g\n",
                  v.dof ? 1 : 0, v.dofNearStart, v.dofNearEnd, v.dofFarStart, v.dofFarEnd, v.dofNearBlur, v.dofFarBlur, v.dofWeapon,
                  v.dofAuto ? 1 : 0, v.dofAutoSpeed);
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "// sun flare + glare\nflare %d\nflareparm %g %g %g %g\nflarecolor %.4f %.4f %.4f\nglare %d\nglareparm %g %g %g %g\n",
                  v.flare ? 1 : 0, v.flareIntensity, v.flareHalo, v.flareGhosts, v.flareStreak, v.flareColor.x, v.flareColor.y, v.flareColor.z,
                  v.glare ? 1 : 0, v.glareThreshold, v.glareIntensity, v.glareRadius, v.glareSun);
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "// screen-space weather (0 off, 1 rain, 2 snow)\nweather %d\nweatherparm %g %g %g\nweatherwind %g %g\n"
                  "weathercolor %.4f %.4f %.4f %g\n",
                  v.weather, v.weatherIntensity, v.weatherSpeed, v.weatherDistance, v.weatherWind[0], v.weatherWind[1], v.weatherColor.x,
                  v.weatherColor.y, v.weatherColor.z, v.weatherOpacity);
    out += buf;
    std::snprintf(buf, sizeof buf, "// local light shafts\nlights %d\nlightsparm %g %g %d %g\n", v.lights ? 1 : 0, v.lightsIntensity,
                  v.lightsShadows, v.lightsMax, v.lightsDistance);
    out += buf;
    if (v.lights)  // x y z  r g b  radius  spot dir xyz  cos outer  cos inner  flicker (point lights: dir 0 0 0, cos -2)
        for (const VolLight& L : v.volLights) {
            std::snprintf(buf, sizeof buf, "llight %.2f %.2f %.2f %.4f %.4f %.4f %g %.5f %.5f %.5f %.5f %.5f %g\n", L.origin.x, L.origin.y,
                          L.origin.z, L.color.x, L.color.y, L.color.z, L.radius, L.spot ? L.spotDir.x : 0, L.spot ? L.spotDir.y : 0,
                          L.spot ? L.spotDir.z : 0, L.spot ? L.cosOuter : -2, L.spot ? L.cosInner : -2, L.flicker);
            out += buf;
        }
    for (size_t vi = 0; vi < v.weatherVolumes.size(); vi++) {
        const WeatherVolume& wv = v.weatherVolumes[vi];
        std::snprintf(buf, sizeof buf, "// weather volume: %s\nwxvolume %zu %g %g %g\n", wv.name.c_str(), vi, wv.blend, wv.blendTime, wv.intensity);
        out += buf;
        for (const gradevol::Brush& b : wv.brushes) {
            out += "wxbrush " + std::to_string(vi) + "\n";
            for (const gradevol::Plane& pl : b) {
                std::snprintf(buf, sizeof buf, "wxplane %zu %.6f %.6f %.6f %.3f\n", vi, pl.n[0], pl.n[1], pl.n[2], pl.d);
                out += buf;
            }
        }
    }
    std::snprintf(buf, sizeof buf, "// colour grading (every grade setting is baked into the table below)\ngrade %d\nvignette %g\n",
                  v.grade && v.gradeLut ? 1 : 0, v.vignette);
    out += buf;
    if (v.grade && v.gradeLut) {
        const GradeLut& t = *v.gradeLut;
        static const char hex[] = "0123456789abcdef";
        out += "lut " + std::to_string(t.size) + "\n";
        for (int b = 0; b < t.size; b++)
            for (int g = 0; g < t.size; g++) {
                std::string row = "lutrow " + std::to_string(b) + " " + std::to_string(g) + " ";
                const uint8_t* p = &t.rgb[((static_cast<size_t>(b) * t.size + g) * t.size) * 3];
                for (int i = 0; i < t.size * 3; i++) row += hex[p[i] >> 4], row += hex[p[i] & 15];
                out += row + "\n";
            }
        // trigger_grade volumes: convex brushes (outward planes) + their own table, blended in by the camera position.
        int vi = 0;
        for (const GradeVolume& gv : v.gradeVolumes) {
            if (!gv.lut || gv.lut->size != t.size) continue;
            std::snprintf(buf, sizeof buf, "// grade volume: %s\ngradevolume %d %g %g %g\n", gv.name.c_str(), vi, gv.blend, gv.blendTime,
                          gv.vignette);
            out += buf;
            for (const gradevol::Brush& b : gv.brushes) {
                out += "volbrush " + std::to_string(vi) + "\n";
                for (const gradevol::Plane& pl : b) {
                    std::snprintf(buf, sizeof buf, "volplane %d %.6f %.6f %.6f %.3f\n", vi, pl.n[0], pl.n[1], pl.n[2], pl.d);
                    out += buf;
                }
            }
            for (int b = 0; b < t.size; b++)
                for (int g = 0; g < t.size; g++) {
                    std::string row = "vollutrow " + std::to_string(vi) + " " + std::to_string(b) + " " + std::to_string(g) + " ";
                    const uint8_t* p = &gv.lut->rgb[((static_cast<size_t>(b) * t.size + g) * t.size) * 3];
                    for (int i = 0; i < t.size * 3; i++) row += hex[p[i] >> 4], row += hex[p[i] & 15];
                    out += row + "\n";
                }
            vi++;
        }
    }
    // Player clip brushes (full_clip, clip_player, ...) stop the sun's shafts (the game doesn't draw them, so the DLL draws these
    // triangles into its sky mask).
    size_t tris = 0;
    for (const Entity& e : doc.ents)
        for (const Brush& br : e.brushes)
            for (const Face& f : br.faces) {
                if (f.winding.size() < 3 || !blocksVolumetricSun(f.material)) continue;
                if (!tris) out += "// sun occluders (player clip faces): world-space triangles\n";
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
        // Spotlight: a cone and/or a target.
        double outer = 0, inner = -1;
        readNum(e, "fov_outer", outer);
        readNum(e, "fov_inner", inner);
        const std::string* target = e.find("target");
        if (outer > 0 || (target && !target->empty())) {
            if (outer <= 0) outer = 45;
            outer = std::clamp(outer, 1.0, 89.0);
            inner = inner < 0 ? outer * 0.8 : std::clamp(inner, 0.0, outer);
            Vec3 dir{0, 0, -1};
            bool aimed = false;
            if (target && !target->empty())
                for (const Entity& t : doc.ents) {
                    const std::string* tn = t.find("targetname");
                    Vec3 to;
                    if (tn && *tn == *target && t.origin(to) && length(to - L.origin) > 1e-3) {
                        dir = normalize(to - L.origin);
                        aimed = true;
                        break;
                    }
                }
            if (!aimed) {  // angles "pitch yaw roll" / angle (yaw); neither: straight down
                Vec3 ang{90, 0, 0};
                double yaw = 0;
                if (!readVec(e, "angles", ang) && readNum(e, "angle", yaw)) ang = {0, yaw, 0};
                const double p = ang.x * kPi / 180, y = ang.y * kPi / 180;
                dir = {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)};
            }
            L.spot = true;
            L.spotDir = dir;
            L.cosOuter = std::cos(outer * kPi / 180);
            L.cosInner = std::cos(inner * kPi / 180);
        }
        double n = 1;
        if (readNum(e, "volumetric", n)) L.shafts = n != 0;
        readNum(e, "volumetric_intensity", L.shaftScale);
        readNum(e, "volumetric_flicker", L.flicker);
        L.flicker = std::clamp(L.flicker, 0.0, 1.0);
        L.shaftScale = std::max(0.0, L.shaftScale);
        double only = 0;
        if (readNum(e, "volumetric_only", only) && only != 0) L.bake = false;
        double prim = 0;
        if (readNum(e, "primary", prim) && prim != 0 && L.spot && L.bake) L.primary = true;
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
    // Anything with player clip contents (0x10000): full_clip, clip_player, clip_ai/monster, ...
    return (clip::contentsForMaterial(raw) & 0x10000u) != 0;
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

void Bake::lmapCoordPage(const FaceMap& m, const Vec3& p, float& u, float& v) const {
    const int page = std::clamp(m.page, 0, static_cast<int>(pageY.size()) - 1);
    const int y0 = pageY.empty() ? 0 : pageY[page], h = pageH.empty() ? height : pageH[page];
    double tx = m.x + (p[m.axisU] - m.minU) / luxelSize;
    double ty = m.y - y0 + (p[m.axisV] - m.minV) / luxelSize;
    u = static_cast<float>(tx / width);
    v = static_cast<float>(ty / h);
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
        if (packPages(lit, settings.maxAtlas, std::max(1, settings.maxPages), out.width, out.height, out.pageY, out.pageH)) break;
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
    // Primary spotlights ("primary" "1"): each face picks the one that reaches it most (it's then
    // lit by that light in real time, with the sun baked in), else stays on the sun.
    std::vector<int> primaryLight;  // index in `lights` of primary j
    for (size_t i = 0; i < lights.size(); i++)
        if (lights[i].primary && lights[i].bake && primaryLight.size() < kMaxPrimaryLights) {
            primaryLight.push_back(static_cast<int>(i));
            out.primaries.push_back(lights[i]);
        }
    for (LitFace& lf : lit) {
        lf.map.primary = -1;
        if (primaryLight.empty()) continue;
        const Face& f = lf.face;
        Vec3 c;
        for (const Vec3& p : f.winding) c += p;
        c = c * (1.0 / f.winding.size());
        // Score over the whole face (a grid of up to 24 x 24 points on it), so a big floor that a
        // narrow cone lands in the middle of still goes to the spot.
        std::vector<Vec3> pts{c + f.normal * kRayOffset};
        for (const Vec3& p : f.winding) pts.push_back(p + (c - p) * 0.1 + f.normal * kRayOffset);
        const FaceMap& m = lf.map;
        const int sx = std::max(1, m.w / 24), sy = std::max(1, m.h / 24);
        for (int y = 0; y < m.h; y += sy)
            for (int x = 0; x < m.w; x += sx)
                pts.push_back(facePoint(lf, m.minU + (x + 0.5) * ls, m.minV + (y + 0.5) * ls, ls * 0.05) + f.normal * kRayOffset);
        double best = 0.01;  // the spot must put real light somewhere on the face
        for (size_t j = 0; j < primaryLight.size(); j++) {
            double score = 0;
            for (const Vec3& p : pts) score = std::max(score, lum(lightReach(lights[primaryLight[j]], p, &f.normal)));
            if (score > best) best = score, lf.map.primary = static_cast<int>(j);
        }
    }
    for (const LitFace& lf : lit) scene->maps.push_back(lf.map);

    // GPU first (same rays, much faster); the CPU passes below are the fallback.
    std::vector<float> indirect(out.light.size(), 0.0f), excl(out.light.size(), 0.0f);
    std::vector<float> dirDirect(static_cast<size_t>(W) * H * 4, 0.0f), dirIndirect(static_cast<size_t>(W) * H * 4, 0.0f);
    bool gpuDone = false;
    if (settings.gpu) {
        std::string why;
        gpuDone = gpuPasses(lit, *scene, env, lights, primaryLight, settings, ls, W, out, indirect, excl, dirDirect, dirIndirect, progress,
                            why);
        if (!gpuDone) {
            if (progress && progress->cancel) {
                error = "cancelled";
                return false;
            }
            out.gpuNote = why;
            std::fill(out.sunVis.begin(), out.sunVis.end(), uint8_t(0));
            std::fill(scene->direct.begin(), scene->direct.end(), 0.0f);
            std::fill(indirect.begin(), indirect.end(), 0.0f);
            std::fill(excl.begin(), excl.end(), 0.0f);
            std::fill(dirDirect.begin(), dirDirect.end(), 0.0f);
            std::fill(dirIndirect.begin(), dirIndirect.end(), 0.0f);
        }
    }
    if (!gpuDone) {
    out.device = "CPU (" + std::to_string(std::max(1u, std::thread::hardware_concurrency())) + " threads)";
    // Pass 1: shadow masks at 2x density (the sun's, or the face's primary spotlight's), then
    // direct light (sun + lights) per luxel; the part the game adds in real time goes to excl.
    const bool sun = env.hasSun;
    auto pass1 = [&](size_t fi) {
        const LitFace& lf = lit[fi];
        const FaceMap& m = lf.map;
        const Vec3& n = lf.face.normal;
        const double ndlSun = std::max(0.0, dot(n, env.sunDir));
        const int si = m.primary >= 0 ? primaryLight[m.primary] : -1;
        for (int y = 0; y < m.h * 2; y++)
            for (int x = 0; x < m.w * 2; x++) {
                uint8_t vis = 0;
                if (si >= 0 || (sun && ndlSun > 0)) {
                    Vec3 p = facePoint(lf, m.minU + (x + 0.5) * ls * 0.5, m.minV + (y + 0.5) * ls * 0.5, ls * 0.05) +
                             n * kRayOffset;
                    vis = (si >= 0 ? spotVisible(*scene, lights[si], p, n) : sunVisible(*scene, env, p)) ? 255 : 0;
                }
                out.sunVis[static_cast<size_t>(m.y * 2 + y) * (W * 2) + (m.x * 2 + x)] = vis;
            }
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                Vec3 p = facePoint(lf, m.minU + (x + 0.5) * ls, m.minV + (y + 0.5) * ls, ls * 0.05) + n * kRayOffset;
                double vis;
                if (si < 0) {
                    size_t s0 = static_cast<size_t>(m.y * 2 + y * 2) * (W * 2) + (m.x * 2 + x * 2);
                    vis = (out.sunVis[s0] + out.sunVis[s0 + 1] + out.sunVis[s0 + W * 2] + out.sunVis[s0 + W * 2 + 1]) / (4.0 * 255.0);
                } else {
                    vis = sun && ndlSun > 0 && sunVisible(*scene, env, p) ? 1 : 0;
                }
                Vec3 ldir;
                double lw = 0;
                const Vec3 sunPart = env.sunColor * (vis * ndlSun);
                const Vec3 others = pointLights(*scene, lights, p, &n, &ldir, &lw, si);
                const Vec3 spot = si >= 0 ? oneLight(*scene, lights[si], p, n) : Vec3{};
                if (si >= 0) {  // the sun is baked on spot faces: it counts towards the direction
                    ldir += env.sunDir * lum(sunPart);
                    lw += lum(sunPart);
                }
                const Vec3 d = sunPart + others + spot, ex = si >= 0 ? spot : sunPart;
                const size_t px = static_cast<size_t>(m.y + y) * W + (m.x + x);
                float* o = &scene->direct[px * 3];
                o[0] = static_cast<float>(d.x), o[1] = static_cast<float>(d.y), o[2] = static_cast<float>(d.z);
                float* oe = &excl[px * 3];
                oe[0] = static_cast<float>(ex.x), oe[1] = static_cast<float>(ex.y), oe[2] = static_cast<float>(ex.z);
                float* od = &dirDirect[px * 4];
                od[0] = static_cast<float>(ldir.x), od[1] = static_cast<float>(ldir.y), od[2] = static_cast<float>(ldir.z);
                od[3] = static_cast<float>(lw);
            }
    };
    if (!parallelFor(lit.size(), progress, 0.0f, 0.3f, pass1)) {
        error = "cancelled";
        return false;
    }

    // Pass 2+: sky dome and bounce, gathered with cosine-weighted hemisphere rays. Each bounce
    // re-gathers from (direct + previous indirect).
    std::vector<float> source = scene->direct;
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
                    Vec3 sky, bounce, gdir;
                    double gw = 0;
                    const double skyW = lum(env.skyColor);
                    for (int i = 0; i < samples; i++) {
                        Vec3 dir = cosineDir(n, t, b, i, samples, rng);
                        Hit h;
                        if (!scene->trace(p, dir, 1e6f, h) || scene->tris[h.tri].sky) {
                            sky += env.skyColor;
                            gdir += dir * skyW;
                            gw += skyW;
                        } else if (settings.bounces > 0) {
                            const Tri& tr = scene->tris[h.tri];
                            if (tr.face >= 0) {
                                Vec3 hp = p + dir * h.t;
                                const FaceMap& hm = scene->maps[tr.face];
                                int hx = std::clamp(static_cast<int>((hp[hm.axisU] - hm.minU) / ls), 0, hm.w - 1) + hm.x;
                                int hy = std::clamp(static_cast<int>((hp[hm.axisV] - hm.minV) / ls), 0, hm.h - 1) + hm.y;
                                const float* c = &source[(static_cast<size_t>(hy) * W + hx) * 3];
                                const Vec3 bc = Vec3{c[0], c[1], c[2]} * settings.albedo;
                                bounce += bc;
                                gdir += dir * lum(bc);
                                gw += lum(bc);
                            }
                        }
                    }
                    Vec3 e = (sky + bounce) * (1.0 / samples);
                    const size_t px = static_cast<size_t>(m.y + y) * W + (m.x + x);
                    float* o = &indirect[px * 3];
                    o[0] = static_cast<float>(e.x), o[1] = static_cast<float>(e.y), o[2] = static_cast<float>(e.z);
                    float* od = &dirIndirect[px * 4];
                    od[0] = static_cast<float>(gdir.x / samples), od[1] = static_cast<float>(gdir.y / samples);
                    od[2] = static_cast<float>(gdir.z / samples), od[3] = static_cast<float>(gw / samples);
                }
        };
        float from = 0.3f + 0.7f * round / rounds, to = 0.3f + 0.7f * (round + 1) / rounds;
        if (!parallelFor(lit.size(), progress, from, to, gather)) {
            error = "cancelled";
            return false;
        }
        for (size_t i = 0; i < source.size(); i++) source[i] = scene->direct[i] + indirect[i];
    }
    }  // CPU passes

    // Final baked light: everything but what the game adds in real time (the sun on sun faces,
    // the face's primary spotlight on spot faces: `excl`), plus ambient.
    for (size_t fi = 0; fi < lit.size(); fi++) {
        const LitFace& lf = lit[fi];
        const FaceMap& m = lf.map;
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                size_t i = (static_cast<size_t>(m.y + y) * W + (m.x + x)) * 3;
                for (int k = 0; k < 3; k++) {
                    double v = scene->direct[i + k] - excl[i + k] + indirect[i + k] + env.ambient[k];
                    out.light[i + k] = static_cast<float>(std::max(0.0, v));
                }
            }
        out.faces[faceKey(lf.face)] = m;
    }
    for (const PointLight& L : lights)
        if (L.bake) out.lights.push_back(L);
    out.realtime = excl;
    out.lightDir.resize(dirDirect.size());
    for (size_t i = 0; i < dirDirect.size(); i++) out.lightDir[i] = dirDirect[i] + dirIndirect[i];
    out.scene = scene;
    if (progress) progress->fraction = 1.0f;
    return true;
}

std::vector<uint8_t> bakeReflectionProbe(const Bake& b, const Vec3& origin, int size) {
    size = std::max(1, size);
    int levels = 1;
    while ((size >> (levels - 1)) > 1) levels++;
    size_t perFace = 0;
    for (int l = 0; l < levels; l++) perFace += static_cast<size_t>(size >> l) * (size >> l);
    std::vector<uint8_t> out(perFace * 6 * 4, 0);
    if (!b.scene) return out;
    const Scene& sc = *b.scene;
    const Environment& env = b.env;
    const double albedo = 0.4;
    // Radiance along a ray from the probe.
    auto radiance = [&](const Vec3& dir) -> Vec3 {
        Hit h;
        if (!sc.trace(origin, dir, 1e6f, h) || sc.tris[h.tri].sky) {
            Vec3 s = env.skyColor;
            if (env.hasSun) {
                const double c = std::max(0.0, dot(dir, env.sunDir));
                s += env.sunColor * (std::pow(c, 400.0) * 6 + std::pow(c, 12.0) * 0.25);
            }
            return s;
        }
        const int f = sc.tris[h.tri].face;
        if (f < 0 || f >= static_cast<int>(sc.maps.size())) return env.ambient * albedo;
        const FaceMap& m = sc.maps[f];
        const Vec3 hp = origin + dir * h.t;
        const int x = std::clamp(static_cast<int>((hp[m.axisU] - m.minU) / b.luxelSize), 0, m.w - 1) + m.x;
        const int y = std::clamp(static_cast<int>((hp[m.axisV] - m.minV) / b.luxelSize), 0, m.h - 1) + m.y;
        const size_t i = (static_cast<size_t>(y) * b.width + x) * 3;
        Vec3 e{b.light[i], b.light[i + 1], b.light[i + 2]};
        if (b.realtime.size() == b.light.size()) e += Vec3{b.realtime[i], b.realtime[i + 1], b.realtime[i + 2]};
        return e * albedo;
    };
    // Mip 0 in linear radiance, per face; then the box-filtered chain; then sqrt into BGRA.
    std::vector<std::vector<Vec3>> faces(6, std::vector<Vec3>(static_cast<size_t>(size) * size));
    parallelFor(6 * static_cast<size_t>(size), nullptr, 0, 1, [&](size_t job) {
        const int f = static_cast<int>(job / size), ty = static_cast<int>(job % size);
        for (int tx = 0; tx < size; tx++) {
            Vec3 acc;
            for (int sy = 0; sy < 2; sy++)
                for (int sx = 0; sx < 2; sx++) {
                    const double sc0 = 2.0 * (tx + 0.25 + 0.5 * sx) / size - 1, tc = 2.0 * (ty + 0.25 + 0.5 * sy) / size - 1;
                    double x = 0, y = 0, z = 0;
                    switch (f) {  // D3D cube faces: +X, -X, +Y, -Y, +Z, -Z (as the custom sky)
                        case 0: x = 1, y = -tc, z = -sc0; break;
                        case 1: x = -1, y = -tc, z = sc0; break;
                        case 2: x = sc0, y = 1, z = tc; break;
                        case 3: x = sc0, y = -1, z = -tc; break;
                        case 4: x = sc0, y = -tc, z = 1; break;
                        default: x = -sc0, y = -tc, z = -1; break;
                    }
                    acc += radiance(normalize(Vec3{x, y, z}));
                }
            faces[f][static_cast<size_t>(ty) * size + tx] = acc * 0.25;
        }
    });
    size_t at = 0;
    for (int f = 0; f < 6; f++) {
        std::vector<Vec3> mip = faces[f];
        int s = size;
        for (int l = 0; l < levels; l++) {
            for (size_t t = 0; t < mip.size(); t++, at++)
                for (int k = 0; k < 3; k++)
                    out[at * 4 + (2 - k)] = static_cast<uint8_t>(std::lround(std::clamp(std::sqrt(std::max(0.0, mip[t][k])), 0.0, 1.0) * 255));
            for (size_t t = 0; t < mip.size(); t++) out[(at - mip.size() + t) * 4 + 3] = 255;
            if (s == 1) break;
            const int h = s / 2;
            std::vector<Vec3> next(static_cast<size_t>(h) * h);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < h; x++)
                    next[static_cast<size_t>(y) * h + x] = (mip[(2 * y) * s + 2 * x] + mip[(2 * y) * s + 2 * x + 1] + mip[(2 * y + 1) * s + 2 * x] +
                                                            mip[(2 * y + 1) * s + 2 * x + 1]) *
                                                           0.25;
            mip = std::move(next);
            s = h;
        }
    }
    return out;
}

const std::array<Vec3, 56>& gridDirections() {
    static const std::array<Vec3, 56> dirs = [] {
        std::array<Vec3, 56> d{};
        size_t n = 0;
        for (int z = 0; z < 4; z++)
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    const bool inner = x >= 1 && x <= 2 && y >= 1 && y <= 2 && z >= 1 && z <= 2;
                    if (!inner) d[n++] = normalize(Vec3{x - 1.5, y - 1.5, z - 1.5});
                }
        return d;
    }();
    return dirs;
}

std::vector<GridSample> sampleGrid(const Bake& b, const std::vector<Vec3>& points, int rays) {
    std::vector<GridSample> out(points.size());
    if (!b.scene) return out;
    const Scene& sc = *b.scene;
    const Environment& env = b.env;
    const std::array<Vec3, 56>& gd = gridDirections();
    rays = std::max(8, rays);
    parallelFor(points.size(), nullptr, 0, 1, [&](size_t i) {
        const Vec3& p = points[i];
        Rng rng(i * 2654435761ull + 17);
        // Radiance arriving from each ray direction (uniform over the sphere), then the
        // cosine-weighted sum per grid direction: E(n) = 4/N * sum L * max(0, n.w), so a uniform
        // surround of L gives L, as on a lightmapped surface.
        std::array<Vec3, 56> e{};
        for (int r = 0; r < rays; r++) {
            double z = 1 - 2 * ((r + rng.next()) / rays), phi = 2 * kPi * rng.next();
            double s = std::sqrt(std::max(0.0, 1 - z * z));
            Vec3 dir{s * std::cos(phi), s * std::sin(phi), z};
            Hit h;
            Vec3 L;
            if (!sc.trace(p, dir, 1e6f, h) || sc.tris[h.tri].sky)
                L = env.skyColor;
            else if (sc.tris[h.tri].face >= 0)
                L = sc.directAt(sc.tris[h.tri].face, p + dir * h.t) * 0.45;
            else
                continue;
            for (size_t k = 0; k < 56; k++) {
                const double c = dot(gd[k], dir);
                if (c > 0) e[k] += L * c;
            }
        }
        for (Vec3& v : e) v = v * (4.0 / rays) + env.ambient;
        // Which primary light lights models here in real time: the spotlight reaching the point
        // most (it's then left out of the grid and the sun put in), else the sun if it gets here.
        const bool sunHere = env.hasSun && sunVisible(sc, env, p);
        int spot = -1;
        double best = 1e-4;
        for (size_t j = 0; j < b.primaries.size(); j++) {
            const PointLight& L = b.primaries[j];
            Vec3 l;
            double dist = 0;
            const double s = lum(lightReach(L, p, nullptr, &l, &dist));
            if (s > best && !sc.occluded(p, l, static_cast<float>(dist - 1.0))) best = s, spot = static_cast<int>(j);
        }
        out[i].primary = spot >= 0 ? 2 + spot : sunHere ? 1 : 0;
        if (spot >= 0 && sunHere)
            for (size_t k = 0; k < 56; k++) {
                const double c = dot(gd[k], env.sunDir);
                if (c > 0) e[k] += env.sunColor * c;
            }
        // The lights, each with one shadow ray, on every direction facing it.
        for (const PointLight& L : b.lights) {
            if (spot >= 0 && L.primary && length(L.origin - b.primaries[spot].origin) < 1e-6) continue;
            const Vec3 d = L.origin - p;
            const double dist = length(d);
            if (dist >= L.radius || dist < 1e-6) continue;
            const Vec3 l = d * (1.0 / dist);
            const double f = 1 - (dist * dist) / (L.radius * L.radius);
            double atten = f * f;
            if (L.spot) {
                const double c = -dot(l, L.spotDir);
                const double t = std::clamp((c - L.cosOuter) / std::max(L.cosInner - L.cosOuter, 1e-6), 0.0, 1.0);
                atten *= t * t * (3 - 2 * t);
            }
            if (atten <= 0 || sc.occluded(p, l, static_cast<float>(dist - 1.0))) continue;
            for (size_t k = 0; k < 56; k++) {
                const double c = dot(gd[k], l);
                if (c > 0) e[k] += L.color * (atten * c);
            }
        }
        Vec3 avg;
        for (const Vec3& v : e) avg += v;
        out[i].dir = e;
        out[i].light = avg * (1.0 / 56);
    });
    return out;
}

}  // namespace light
