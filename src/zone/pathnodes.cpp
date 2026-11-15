#include "zone/pathnodes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numeric>

#include "document.h"
#include "zone/clipmap.h"

namespace pathnodes {
namespace {

constexpr uint32_t kMonsterClip = 0x20000;  // part of CONTENTS_PLAYERCLIP (clip materials set it)
constexpr double kMaxLink = 256;            // stock links reach ~255 units
constexpr double kStep = 18;                // AI step height
constexpr double kHalfWidth = 14;           // AI hull (stock actors: radius 15, height 72)
constexpr double kHeight = 72;
constexpr double kEye = 60;                 // visibility rays between node eye points
constexpr size_t kLeafNodes = 4;

struct Convex {
    double mn[3], mx[3];
    std::vector<std::array<double, 4>> planes;  // n.x n.y n.z d (inside: n.p <= d)
    uint32_t contents;
};

struct World {
    std::vector<Convex> brushes;

    // Sweeps an axis-aligned box (half extents h) from a to b through brushes whose contents
    // intersect mask. Returns the hit fraction (1 = clear); startSolid when a starts inside.
    double sweep(const double a[3], const double b[3], const double h[3], uint32_t mask, bool* startSolid = nullptr) const {
        double best = 1;
        if (startSolid) *startSolid = false;
        double lo[3], hi[3];
        for (int k = 0; k < 3; k++) lo[k] = std::min(a[k], b[k]) - h[k], hi[k] = std::max(a[k], b[k]) + h[k];
        for (const Convex& c : brushes) {
            if (!(c.contents & mask)) continue;
            bool apart = false;
            for (int k = 0; k < 3 && !apart; k++) apart = c.mx[k] < lo[k] || c.mn[k] > hi[k];
            if (apart) continue;
            double enter = -1, exit = 1;
            bool outside = false, startOut = false;
            auto clipPlane = [&](const double n[3], double d) {
                const double dd = d + std::fabs(n[0]) * h[0] + std::fabs(n[1]) * h[1] + std::fabs(n[2]) * h[2];
                const double s = n[0] * a[0] + n[1] * a[1] + n[2] * a[2] - dd;
                const double e = n[0] * b[0] + n[1] * b[1] + n[2] * b[2] - dd;
                if (s > 0) startOut = true;
                if (s > 0 && e >= 0) {
                    outside = true;
                    return;
                }
                if (s <= 0 && e <= 0) return;
                const double t = s / (s - e);
                if (s > 0) enter = std::max(enter, t);
                else exit = std::min(exit, t);
            };
            for (int k = 0; k < 3 && !outside; k++) {
                double n[3] = {0, 0, 0};
                n[k] = 1;
                clipPlane(n, c.mx[k]);
                n[k] = -1;
                if (!outside) clipPlane(n, -c.mn[k]);
            }
            for (size_t p = 0; p < c.planes.size() && !outside; p++) clipPlane(c.planes[p].data(), c.planes[p][3]);
            if (outside || enter > exit) continue;
            if (!startOut) {
                if (startSolid) *startSolid = true;
                return 0;
            }
            if (enter >= 0 && enter < best) best = enter;
        }
        return best;
    }
};

World makeWorld(const clip::ClipMapData& cm) {
    World w;
    for (const clip::Brush& b : cm.brushes) {
        if (!b.contents) continue;
        Convex c;
        c.contents = b.contents;
        for (int k = 0; k < 3; k++) {
            c.mn[k] = b.bounds.midPoint[k] - b.bounds.halfSize[k];
            c.mx[k] = b.bounds.midPoint[k] + b.bounds.halfSize[k];
        }
        for (uint32_t s = 0; s < b.numSides; s++) {
            const clip::Plane& p = cm.planes[cm.sides[b.firstSide + s].plane];
            c.planes.push_back({p.normal[0], p.normal[1], p.normal[2], p.dist});
        }
        w.brushes.push_back(std::move(c));
    }
    return w;
}

std::string str(const Entity& e, const char* key) {
    const std::string* v = e.find(key);
    return v ? *v : std::string();
}

int buildTree(Graph& g, std::vector<uint16_t> ids) {
    const int me = static_cast<int>(g.tree.size());
    g.tree.emplace_back();
    if (ids.size() <= kLeafNodes) {
        g.tree[me].nodes = std::move(ids);
        return me;
    }
    double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
    for (uint16_t i : ids)
        for (int k = 0; k < 2; k++) lo[k] = std::min(lo[k], double(g.nodes[i].origin[k])), hi[k] = std::max(hi[k], double(g.nodes[i].origin[k]));
    // Split the wider axis at the gap between distinct coordinates closest to the median (grids
    // put many nodes on one coordinate); the other axis if that one has a single value.
    int axis = hi[1] - lo[1] > hi[0] - lo[0] ? 1 : 0;
    float dist = 0;
    bool found = false;
    for (int attempt = 0; attempt < 2 && !found; attempt++, axis ^= 1) {
        std::sort(ids.begin(), ids.end(), [&](uint16_t a, uint16_t b) { return g.nodes[a].origin[axis] < g.nodes[b].origin[axis]; });
        const size_t mid = ids.size() / 2;
        for (size_t off = 0; off < ids.size() && !found; off++)
            for (int sgn : {1, -1}) {
                const long long k = static_cast<long long>(mid) + sgn * static_cast<long long>(off);
                if (k < 1 || k >= static_cast<long long>(ids.size())) continue;
                const float a = g.nodes[ids[k - 1]].origin[axis], b = g.nodes[ids[k]].origin[axis];
                if (a < b) {
                    dist = 0.5f * (a + b);
                    found = true;
                    break;
                }
            }
        if (found) break;
    }
    if (!found) {  // every node on the same spot: one (oversized) leaf
        g.tree[me].nodes = std::move(ids);
        return me;
    }
    // Every node strictly below dist goes left, the rest right.
    std::vector<uint16_t> left, right;
    for (uint16_t i : ids) (g.nodes[i].origin[axis] < dist ? left : right).push_back(i);
    g.tree[me].axis = axis;
    g.tree[me].dist = dist;
    const int c0 = buildTree(g, std::move(left));
    const int c1 = buildTree(g, std::move(right));
    g.tree[me].child[0] = c0;
    g.tree[me].child[1] = c1;
    return me;
}

}  // namespace

bool isPathNodeClass(const std::string& c) { return c == "node_pathnode"; }

Graph build(const Document& doc, const clip::ClipMapData& cm, std::vector<std::string>* warnings) {
    Graph g;
    for (const Entity& e : doc.ents) {
        if (!isPathNodeClass(e.classname())) continue;
        Node n;
        Vec3 o;
        e.origin(o);
        for (int k = 0; k < 3; k++) n.origin[k] = static_cast<float>(o[k]);
        if (const std::string* a = e.find("angles")) {
            double p = 0, y = 0, r = 0;
            if (std::sscanf(a->c_str(), "%lf %lf %lf", &p, &y, &r) >= 2) n.angle = static_cast<float>(y);
        } else if (const std::string* a1 = e.find("angle")) {
            n.angle = static_cast<float>(std::atof(a1->c_str()));
        }
        if (const std::string* sf = e.find("spawnflags")) n.spawnflags = static_cast<uint16_t>(std::atoi(sf->c_str()));
        n.targetname = str(e, "targetname");
        n.target = str(e, "target");
        n.noteworthy = str(e, "script_noteworthy");
        n.linkname = str(e, "script_linkname");
        g.nodes.push_back(std::move(n));
        if (g.nodes.size() >= 8192) {  // the game's node cap (iw4sp+0x1a5730: 0x2000)
            if (warnings) warnings->push_back("path nodes: more than 8192, the rest are ignored");
            break;
        }
    }
    const size_t n = g.nodes.size();
    g.links.resize(n);
    if (!n) return g;
    const World w = makeWorld(cm);
    const uint32_t moveMask = clip::CONTENTS_SOLID | kMonsterClip;
    char buf[256];

    // Drop every node onto the floor below it (a short way up first, so nodes placed slightly
    // in the floor still find it).
    const double foot[3] = {kHalfWidth * 0.5, kHalfWidth * 0.5, 0};
    for (size_t i = 0; i < n; i++) {
        Node& nd = g.nodes[i];
        const double a[3] = {nd.origin[0], nd.origin[1], nd.origin[2] + kStep};
        const double b[3] = {nd.origin[0], nd.origin[1], nd.origin[2] - 512};
        bool solid = false;
        const double f = w.sweep(a, b, foot, moveMask, &solid);
        if (solid) {
            std::snprintf(buf, sizeof buf, "path node %zu at (%.0f %.0f %.0f) is in solid", i, nd.origin[0], nd.origin[1], nd.origin[2]);
            if (warnings) warnings->push_back(buf);
            continue;
        }
        if (f >= 1) {
            std::snprintf(buf, sizeof buf, "path node %zu at (%.0f %.0f %.0f) has no floor below it", i, nd.origin[0], nd.origin[1], nd.origin[2]);
            if (warnings) warnings->push_back(buf);
            continue;
        }
        nd.origin[2] = static_cast<float>(a[2] + (b[2] - a[2]) * f);
    }

    // Links: a walkable hull sweep between the two nodes (the box rides kStep above the floor so
    // steps and gentle slopes pass) plus floor under the whole way.
    const double hull[3] = {kHalfWidth, kHalfWidth, (kHeight - kStep) / 2};
    const double footProbe[3] = {4, 4, 0};
    auto walkable = [&](const Node& A, const Node& B) {
        const double zc = kStep + hull[2] + 1;
        const double a[3] = {A.origin[0], A.origin[1], A.origin[2] + zc};
        const double b[3] = {B.origin[0], B.origin[1], B.origin[2] + zc};
        if (w.sweep(a, b, hull, moveMask) < 1) return false;
        const double len = std::hypot(B.origin[0] - A.origin[0], B.origin[1] - A.origin[1]);
        const int steps = std::max(1, static_cast<int>(len / 16));
        for (int s = 1; s < steps; s++) {
            const double t = double(s) / steps;
            const double p[3] = {A.origin[0] + (B.origin[0] - A.origin[0]) * t, A.origin[1] + (B.origin[1] - A.origin[1]) * t,
                                 A.origin[2] + (B.origin[2] - A.origin[2]) * t};
            const double up[3] = {p[0], p[1], p[2] + kStep};
            const double dn[3] = {p[0], p[1], p[2] - kStep * 2};
            if (w.sweep(up, dn, footProbe, moveMask) >= 1) return false;  // a hole or a drop
        }
        return true;
    };
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            const Node &A = g.nodes[i], &B = g.nodes[j];
            const double d2 = std::hypot(B.origin[0] - A.origin[0], B.origin[1] - A.origin[1]);
            if (d2 > kMaxLink || std::fabs(B.origin[2] - A.origin[2]) > kMaxLink * 0.5) continue;
            if (!walkable(A, B)) continue;
            g.links[i].push_back({static_cast<uint16_t>(j), static_cast<float>(d2)});
            g.links[j].push_back({static_cast<uint16_t>(i), static_cast<float>(d2)});
        }
    size_t lonely = 0;
    for (size_t i = 0; i < n; i++) {
        std::sort(g.links[i].begin(), g.links[i].end());
        if (g.links[i].empty() && n > 1) {
            if (lonely++ < 8) {
                std::snprintf(buf, sizeof buf, "path node %zu at (%.0f %.0f %.0f) links to no other node (too far, or blocked)", i,
                              g.nodes[i].origin[0], g.nodes[i].origin[1], g.nodes[i].origin[2]);
                if (warnings) warnings->push_back(buf);
            }
        }
    }

    // Visibility between eye points, through solid brushes only (clip does not block sight).
    g.vis.assign((n * (n - 1) + 7) / 8, 0);
    const double ray[3] = {0, 0, 0};
    size_t visible = 0;
    for (size_t lo = 0; lo < n; lo++)
        for (size_t hi = lo + 1; hi < n; hi++) {
            const Node &A = g.nodes[lo], &B = g.nodes[hi];
            const double a[3] = {A.origin[0], A.origin[1], A.origin[2] + kEye};
            const double b[3] = {B.origin[0], B.origin[1], B.origin[2] + kEye};
            if (w.sweep(a, b, ray, clip::CONTENTS_SOLID) < 1) continue;
            const size_t bit = hi + lo * n;
            g.vis[bit >> 3] |= static_cast<uint8_t>(1u << (bit & 7));
            visible++;
        }

    std::vector<uint16_t> all(n);
    std::iota(all.begin(), all.end(), uint16_t(0));
    buildTree(g, std::move(all));

    size_t linkCount = 0;
    for (const auto& l : g.links) linkCount += l.size();
    std::snprintf(buf, sizeof buf, "path nodes: %zu nodes, %zu links, %zu visible pairs, %zu tree nodes", n, linkCount / 2, visible,
                  g.tree.size());
    if (warnings) warnings->push_back(buf);
    return g;
}

}  // namespace pathnodes
