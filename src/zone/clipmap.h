#pragma once
#include <set>
#include <cstdint>
#include <string>
#include <vector>
#include "document.h"

// Collision compiler: editor brushes -> the arrays clipMap_t holds, with indices where the game
// struct has pointers. Serialization (indices -> back-references) is in zonebuild.cpp.
//
// Conventions taken from stock boneyard's clipMap_t (walked in Load_clipMap_t order):
//   - a brush's 6 axial faces are implicit: brushBounds (midPoint/halfSize) + axialMaterialNum
//     [minmax][axis]; cbrush_t.sides lists only the non-axial sides (a box has numsides 0)
//   - brush side numbering for adjacency: axial side (minmax, axis) = 2*axis + minmax, i.e.
//     -X 0, +X 1, -Y 2, +Y 3, -Z 4, +Z 5; non-axial sides are 6, 7, ...
//   - brush edges: for each side in that order, the neighbouring side across each winding edge,
//     clockwise seen from outside; packed per brush, indexed by firstAdjacentSideOffset(s)
//   - leafs[0] and leafbrushNodes[0] are all zero (leafBrushNode 0 = "no brushes")
//   - cLeafBrushNode leafBrushCount > 0: a leaf listing brushes (indices into leafbrushes)
//   - cNode_t children: >= 0 node index, < 0 leaf (-1 - leafIndex)
//   - cmodels[0] is the world; its leaf is empty (world traces walk the node tree)
//   - contents bits (CoD4 family): SOLID 0x1, SKY 0x800, weapon clip 0x2080, player+monster
//     clip 0x30000, DETAIL 0x8000000
namespace clip {

enum Contents : uint32_t {
    CONTENTS_SOLID = 0x1,
    CONTENTS_SKY = 0x800,
    CONTENTS_WEAPONCLIP = 0x2080,  // clipshot | missileclip
    CONTENTS_PLAYERCLIP = 0x30000,  // playerclip | monsterclip
    CONTENTS_DETAIL = 0x8000000,
};

struct Plane {
    float normal[3];
    float dist;
    uint8_t type;      // 0/1/2 = axial X/Y/Z, 3 = non-axial
    uint8_t signbits;  // bit i set when normal[i] < 0
};

struct Material {
    std::string name;
    uint32_t surfaceFlags = 0;
    uint32_t contents = 0;
};

struct Side {
    uint32_t plane;
    uint16_t material;
    uint8_t firstAdjacentSideOffset;
    uint8_t edgeCount;
};

struct Bounds {
    float midPoint[3];
    float halfSize[3];
};

struct Brush {
    uint16_t numSides = 0;
    uint32_t firstSide = 0;  // into ClipMapData::sides
    uint32_t firstEdge = 0;  // into ClipMapData::brushEdges
    uint32_t edgeBytes = 0;  // adjacency bytes this brush owns
    uint16_t axialMaterial[2][3] = {};
    uint8_t firstAdjacentSideOffsets[2][3] = {};
    uint8_t edgeCount[2][3] = {};
    Bounds bounds{};
    uint32_t contents = 0;
};

struct Node {
    uint32_t plane;
    int16_t children[2];
};

struct Leaf {
    uint16_t firstCollAabbIndex = 0;
    uint16_t collAabbCount = 0;
    uint32_t brushContents = 0;
    uint32_t terrainContents = 0;
    Bounds bounds{};
    uint32_t leafBrushNode = 0;
};

struct LeafBrushNode {
    uint8_t axis = 0;
    int16_t leafBrushCount = 0;    // > 0: a leaf listing brushes; <= 0: an interior (split) node
    uint32_t contents = 0;
    uint32_t firstLeafBrush = 0;   // leaf (count > 0): into ClipMapData::leafBrushes
    // Interior (count <= 0): a kd-split on `axis` at `dist` with a `range`-wide straddle zone; the
    // two children sit at this node's index + childOffset[0]/[1] (relative). Child 0 is the min
    // side (brushes with min[axis] <= dist), child 1 the max side (max[axis] >= dist); brushes that
    // straddle are duplicated into both, so collision is correct regardless of the range test.
    float dist = 0, range = 0;
    uint16_t childOffset[2] = {0, 0};
};

struct CModel {
    Bounds bounds{};
    float radius = 0;
    Leaf leaf;
};

struct ClipMapData {
    std::vector<Plane> planes;
    std::vector<Material> materials;
    std::vector<Side> sides;
    std::vector<uint8_t> brushEdges;
    std::vector<Node> nodes;
    std::vector<Leaf> leafs;
    std::vector<uint16_t> leafBrushes;
    std::vector<LeafBrushNode> leafBrushNodes;
    std::vector<CModel> cmodels;
    std::vector<Brush> brushes;
    uint32_t checksum = 0;
};

// Contents for a material name (clip, sky, non-colliding tool materials). 0 = no collision.
uint32_t contentsForMaterial(const std::string& name);

// Compiles worldspawn's brushes. Brush entities (submodels) are not compiled yet.
// nonSolid: lower-case editor material names with no collision (additive effect materials such
// as atmos_particles_dust, which stock maps use as nonsolid sheets/volumes).
bool buildClipMapData(const Document& doc, ClipMapData& out, std::string& error,
                      const std::set<std::string>* nonSolid = nullptr);

}  // namespace clip
