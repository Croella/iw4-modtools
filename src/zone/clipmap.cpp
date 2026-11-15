#include "clipmap.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>

namespace clip {
namespace {

constexpr double kAxialEps = 1e-6;
constexpr double kOnPlaneEps = 0.05;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// Axial side index (2*axis + positive) for an axis-aligned normal, else -1.
int axialIndex(const Vec3& n) {
    for (int k = 0; k < 3; k++)
        if (std::fabs(n[k]) > 1.0 - kAxialEps) return 2 * k + (n[k] > 0 ? 1 : 0);
    return -1;
}

Bounds toBounds(const AABB& b) {
    Bounds r{};
    for (int k = 0; k < 3; k++) {
        r.midPoint[k] = static_cast<float>((b.mins[k] + b.maxs[k]) * 0.5);
        r.halfSize[k] = static_cast<float>((b.maxs[k] - b.mins[k]) * 0.5);
    }
    return r;
}

class Builder {
public:
    explicit Builder(ClipMapData& out, const std::set<std::string>* nonSolid) : out_(out), nonSolid_(nonSolid) {}

    uint32_t plane(const Vec3& n, double d) {
        std::array<long long, 4> key = {std::llround(n.x * 1e5), std::llround(n.y * 1e5),
                                        std::llround(n.z * 1e5), std::llround(d * 1e3)};
        auto it = planeIndex_.find(key);
        if (it != planeIndex_.end()) return it->second;
        Plane p{};
        p.normal[0] = static_cast<float>(n.x);
        p.normal[1] = static_cast<float>(n.y);
        p.normal[2] = static_cast<float>(n.z);
        p.dist = static_cast<float>(d);
        int ax = axialIndex(n);
        p.type = static_cast<uint8_t>(ax >= 0 ? ax / 2 : 3);
        for (int k = 0; k < 3; k++)
            if (n[k] < 0) p.signbits |= static_cast<uint8_t>(1 << k);
        uint32_t idx = static_cast<uint32_t>(out_.planes.size());
        out_.planes.push_back(p);
        planeIndex_[key] = idx;
        return idx;
    }

    // One clipMap material per (name, surface type override): brushes overriding their surface type
    // get their own entry with the same name.
    uint16_t material(const std::string& name) {
        const std::string key = name + "#" + std::to_string(surfaceOverride_);
        auto it = materialIndex_.find(key);
        if (it != materialIndex_.end()) return it->second;
        Material m;
        m.name = name.rfind("wc/", 0) == 0 ? name.substr(3) : name;  // stock clipMap names drop "wc/"
        m.contents = contentsForMaterial(name);
        m.surfaceType = surfaceOverride_;
        uint16_t idx = static_cast<uint16_t>(out_.materials.size());
        out_.materials.push_back(m);
        materialIndex_[key] = idx;
        return idx;
    }
    int surfaceOverride_ = -1;

    bool isNonSolid(const std::string& material) const {
        if (!nonSolid_ || nonSolid_->empty()) return false;
        std::string n = lower(material);
        if (n.rfind("wc/", 0) == 0) n = n.substr(3);
        return nonSolid_->count(n) > 0;
    }

    // Returns false if the brush has no collision (or is degenerate) and was skipped.
    bool addBrush(const ::Brush& src) {
        ::Brush b = src;
        if (!b.rebuild()) return false;
        surfaceOverride_ = surfaceTypeIndex(b.surfaceType());

        uint32_t contents = 0;
        for (const Face& f : b.faces)
            if (!f.winding.empty() && !isNonSolid(f.material)) contents |= contentsForMaterial(f.material);
        if (!contents) return false;
        for (const std::string& a : b.attrs)
            if (contains(lower(a), "detail")) contents |= CONTENTS_DETAIL;

        // Assign side numbers: the first face on each axial plane takes 0..5, the rest 6+.
        std::vector<int> sideOf(b.faces.size(), -1);
        int axialFace[6] = {-1, -1, -1, -1, -1, -1};
        std::vector<int> nonAxialFaces;
        for (size_t i = 0; i < b.faces.size(); i++) {
            const Face& f = b.faces[i];
            if (f.winding.size() < 3) continue;
            int ax = axialIndex(f.normal);
            if (ax >= 0) {
                if (axialFace[ax] < 0) {
                    axialFace[ax] = static_cast<int>(i);
                    sideOf[i] = ax;
                }
            } else {
                sideOf[i] = 6 + static_cast<int>(nonAxialFaces.size());
                nonAxialFaces.push_back(static_cast<int>(i));
            }
        }

        Brush br;
        br.bounds = toBounds(b.bounds);
        br.contents = contents;
        br.firstSide = static_cast<uint32_t>(out_.sides.size());
        br.numSides = static_cast<uint16_t>(nonAxialFaces.size());
        br.firstEdge = static_cast<uint32_t>(out_.brushEdges.size());

        // Material for each implicit axial side: its face, else the face leaning most that way.
        for (int ax = 0; ax < 6; ax++) {
            int face = axialFace[ax];
            if (face < 0) {
                double best = -2;
                for (size_t i = 0; i < b.faces.size(); i++) {
                    if (b.faces[i].winding.empty()) continue;
                    double s = b.faces[i].normal[ax / 2] * ((ax & 1) ? 1.0 : -1.0);
                    if (s > best) { best = s; face = static_cast<int>(i); }
                }
            }
            br.axialMaterial[ax & 1][ax / 2] = material(b.faces[face].material);
        }

        // Adjacency: for every side in side-number order, the side across each winding edge.
        std::vector<uint8_t> edges;
        bool edgesOk = true;
        auto emitFace = [&](int faceIdx, uint8_t& firstOut, uint8_t& countOut) {
            firstOut = static_cast<uint8_t>(std::min<size_t>(edges.size(), 255));
            countOut = 0;
            if (faceIdx < 0) return;
            const std::vector<Vec3>& w = b.faces[faceIdx].winding;
            for (size_t e = 0; e < w.size(); e++) {
                const Vec3& p0 = w[e];
                const Vec3& p1 = w[(e + 1) % w.size()];
                int neighbour = -1;
                for (size_t j = 0; j < b.faces.size() && neighbour < 0; j++) {
                    if (static_cast<int>(j) == faceIdx || sideOf[j] < 0) continue;
                    const Face& g = b.faces[j];
                    if (std::fabs(dot(g.normal, p0) - g.dist) < kOnPlaneEps &&
                        std::fabs(dot(g.normal, p1) - g.dist) < kOnPlaneEps)
                        neighbour = sideOf[j];
                }
                if (neighbour < 0) continue;
                edges.push_back(static_cast<uint8_t>(neighbour));
                countOut++;
            }
            if (edges.size() > 255) edgesOk = false;
        };
        for (int ax = 0; ax < 6; ax++)
            emitFace(axialFace[ax], br.firstAdjacentSideOffsets[ax & 1][ax / 2], br.edgeCount[ax & 1][ax / 2]);
        std::vector<Side> sides;
        for (int faceIdx : nonAxialFaces) {
            const Face& f = b.faces[faceIdx];
            Side s{};
            s.plane = plane(f.normal, f.dist);
            s.material = material(f.material);
            emitFace(faceIdx, s.firstAdjacentSideOffset, s.edgeCount);
            sides.push_back(s);
        }
        if (!edgesOk || nonAxialFaces.size() + 6 > 255) {
            // Offsets are bytes; past 255 the format can't address them. Drop adjacency only.
            edges.clear();
            std::memset(br.firstAdjacentSideOffsets, 0, sizeof(br.firstAdjacentSideOffsets));
            std::memset(br.edgeCount, 0, sizeof(br.edgeCount));
            for (Side& s : sides) s.firstAdjacentSideOffset = s.edgeCount = 0;
        }
        br.edgeBytes = static_cast<uint32_t>(edges.size());
        out_.brushEdges.insert(out_.brushEdges.end(), edges.begin(), edges.end());
        out_.sides.insert(out_.sides.end(), sides.begin(), sides.end());
        out_.brushes.push_back(br);
        worldBounds_.add(b.bounds);
        return true;
    }

    const AABB& worldBounds() const { return worldBounds_; }

private:
    ClipMapData& out_;
    const std::set<std::string>* nonSolid_ = nullptr;
    std::map<std::array<long long, 4>, uint32_t> planeIndex_;
    std::map<std::string, uint16_t> materialIndex_;
    AABB worldBounds_;
};

uint32_t fnv1a(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

}  // namespace

uint32_t contentsForMaterial(const std::string& rawName) {
    std::string n = lower(rawName);
    if (n.rfind("wc/", 0) == 0) n = n.substr(3);
    // Tool materials that never collide.
    for (const char* t : {"trigger", "hint", "skip", "portal", "notsolid", "volume", "origin"})
        if (contains(n, t)) return 0;
    if (n.rfind("sky", 0) == 0 || contains(n, "/sky")) return CONTENTS_SKY;
    if (contains(n, "clip")) {
        bool weapon = contains(n, "missile") || contains(n, "weapon") || contains(n, "bullet") ||
                      contains(n, "shot");
        bool player = contains(n, "player") || contains(n, "monster") || contains(n, "ai");
        if (weapon && !player) return CONTENTS_WEAPONCLIP;
        if (player && !weapon) return CONTENTS_PLAYERCLIP;
        return CONTENTS_PLAYERCLIP | CONTENTS_WEAPONCLIP;
    }
    return CONTENTS_SOLID;
}

// Builds a leaf-brush-node kd-tree over the brush indices and returns the root node's index.
// Mirrors stock maps (e.g. boneyard): interior nodes split a kd-axis and duplicate straddling
// brushes into both children, so a trace that reaches a child has every brush that overlaps that
// child's half. leafBrushCount is an int16, so leaves must stay <= 32767 brushes; small leaves
// also keep broad-phase cheap. childOffset is relative (childIndex = thisIndex + childOffset).
uint32_t buildLeafBrushTree(ClipMapData& out, std::vector<uint32_t> idx, int depth) {
    constexpr size_t kLeafTarget = 1024;  // well under the 32767 int16 cap; stock leaves are tiny
    constexpr int kMaxDepth = 48;
    auto bmin = [&](uint32_t i, int k) { return out.brushes[i].bounds.midPoint[k] - out.brushes[i].bounds.halfSize[k]; };
    auto bmax = [&](uint32_t i, int k) { return out.brushes[i].bounds.midPoint[k] + out.brushes[i].bounds.halfSize[k]; };
    uint32_t contents = 0;
    for (uint32_t i : idx) contents |= out.brushes[i].contents;

    bool makeLeaf = idx.size() <= kLeafTarget || depth >= kMaxDepth;
    float dist = 0, range = 0;
    int axis = 0;
    std::vector<uint32_t> left, right;
    if (!makeLeaf) {
        // Split on the widest axis of the combined bounds, at the median of brush centres.
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t i : idx)
            for (int k = 0; k < 3; k++) {
                lo[k] = std::min(lo[k], static_cast<float>(bmin(i, k)));
                hi[k] = std::max(hi[k], static_cast<float>(bmax(i, k)));
            }
        axis = (hi[1] - lo[1] > hi[0] - lo[0]) ? 1 : 0;
        if (hi[2] - lo[2] > hi[axis] - lo[axis]) axis = 2;
        std::vector<float> centres;
        centres.reserve(idx.size());
        for (uint32_t i : idx) centres.push_back(static_cast<float>(out.brushes[i].bounds.midPoint[axis]));
        std::nth_element(centres.begin(), centres.begin() + centres.size() / 2, centres.end());
        dist = centres[centres.size() / 2];
        for (uint32_t i : idx) {
            bool inL = bmin(i, axis) <= dist, inR = bmax(i, axis) >= dist;
            if (!inL && !inR) inL = inR = true;  // degenerate; keep it reachable
            if (inL) left.push_back(i);
            if (inR) right.push_back(i);
            float over = std::max(static_cast<float>(dist - bmin(i, axis)), static_cast<float>(bmax(i, axis) - dist));
            range = std::max(range, std::min(over, static_cast<float>(std::max(hi[axis] - lo[axis], 1.0f))));
        }
        // No progress (every brush straddles, or one side empty): only acceptable as a leaf if it
        // fits the int16 cap. Otherwise force a count split so the tree still terminates.
        if (left.size() == idx.size() && right.size() == idx.size()) {
            if (idx.size() <= 32767) {
                makeLeaf = true;
            } else {
                std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
                    return out.brushes[a].bounds.midPoint[axis] < out.brushes[b].bounds.midPoint[axis];
                });
                left.assign(idx.begin(), idx.begin() + idx.size() / 2);
                right.assign(idx.begin() + idx.size() / 2, idx.end());
            }
        } else if (idx.size() <= 32767 && (left.empty() || right.empty())) {
            makeLeaf = true;
        }
    }

    uint32_t self = static_cast<uint32_t>(out.leafBrushNodes.size());
    if (makeLeaf) {
        LeafBrushNode n;
        n.leafBrushCount = static_cast<int16_t>(idx.size());
        n.contents = contents;
        n.firstLeafBrush = static_cast<uint32_t>(out.leafBrushes.size());
        for (uint32_t i : idx) out.leafBrushes.push_back(static_cast<uint16_t>(i));
        out.leafBrushNodes.push_back(n);
        return self;
    }
    out.leafBrushNodes.push_back(LeafBrushNode{});  // reserve this interior node; children follow
    uint32_t c0 = buildLeafBrushTree(out, std::move(left), depth + 1);
    uint32_t c1 = buildLeafBrushTree(out, std::move(right), depth + 1);
    LeafBrushNode& n = out.leafBrushNodes[self];
    n.leafBrushCount = -1;  // interior
    n.axis = static_cast<uint8_t>(axis);
    n.contents = contents;
    n.dist = dist;
    n.range = range;
    n.childOffset[0] = static_cast<uint16_t>(c0 - self);
    n.childOffset[1] = static_cast<uint16_t>(c1 - self);
    return self;
}

bool buildClipMapData(const Document& doc, ClipMapData& out, std::string& error,
                      const std::set<std::string>* nonSolid) {
    out = ClipMapData{};
    if (doc.ents.empty()) {
        error = "map has no worldspawn";
        return false;
    }
    out.leafs.push_back(Leaf{});                     // leaf 0: reserved, empty
    out.leafBrushNodes.push_back(LeafBrushNode{});   // node 0: "no brushes"

    Builder b(out, nonSolid);
    for (const ::Brush& br : doc.ents[0].brushes) b.addBrush(br);

    // leafBrushes holds u16 brush indices, so the map can have at most 65535 collision brushes.
    if (out.brushes.size() > 65535) {
        error = "too many collision brushes (" + std::to_string(out.brushes.size()) +
                " > 65535); the leaf-brush index is 16-bit";
        return false;
    }

    uint32_t allContents = 0;
    std::vector<uint32_t> allIdx;
    allIdx.reserve(out.brushes.size());
    for (size_t i = 0; i < out.brushes.size(); i++) {
        allContents |= out.brushes[i].contents;
        allIdx.push_back(static_cast<uint32_t>(i));
    }
    AABB world = b.worldBounds();
    if (!world.valid()) world = AABB{{-64, -64, -64}, {64, 64, 64}};
    Bounds wb = toBounds(world);

    // Leaf 1 holds the whole world. One node splits along a plane below everything, so every point
    // lands in leaf 1 (front), the back side being the empty leaf 0. The leaf's brushes are a
    // leaf-brush-node kd-tree (not a flat list), so maps with more than 32767 brushes still build.
    Leaf worldLeaf;
    worldLeaf.brushContents = allContents;
    worldLeaf.bounds = wb;
    if (!out.brushes.empty())
        worldLeaf.leafBrushNode = buildLeafBrushTree(out, std::move(allIdx), 0);
    out.leafs.push_back(worldLeaf);

    uint32_t splitPlane = b.plane(Vec3{0, 0, 1}, std::floor(world.mins.z) - 128.0);
    out.nodes.push_back(Node{splitPlane, {static_cast<int16_t>(-1 - 1), static_cast<int16_t>(-1 - 0)}});

    CModel worldModel;
    worldModel.bounds = wb;
    worldModel.radius = std::sqrt(wb.halfSize[0] * wb.halfSize[0] + wb.halfSize[1] * wb.halfSize[1] +
                                  wb.halfSize[2] * wb.halfSize[2]);
    out.cmodels.push_back(worldModel);  // world model: empty leaf, traces walk the tree

    uint32_t h = 2166136261u;
    for (const Plane& p : out.planes) h = fnv1a(h, &p, sizeof(p));
    for (const Brush& br : out.brushes) h = fnv1a(h, &br.bounds, sizeof(br.bounds));
    out.checksum = h;
    return true;
}

}  // namespace clip
