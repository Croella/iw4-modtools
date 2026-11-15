#pragma once
// SP AI path nodes: node_pathnode entities -> the GameWorldSp PathData the game's AI navigates
// (stock format decoded from trainer.ff + iw4sp 2.0.14):
//   nodes      pathnode_t[168]: constant part (72) from the compiler, dynamic/transient zero
//              (Path_Init iw4sp+0x1a98a0 clears them, recomputes forward from fAngle, copies
//              totalLinkCount and sorts each node's links).
//   links      pathlink_s[12] per node: fDist = 2D distance, nodeNum; stored both ways (stock
//              max ~255 units).
//   basenodes  RUNTIME block (the game fills them).
//   chain      none (chainNodeCount 0, both arrays null, as trainer).
//   pathVis    bit (hi + lo * n) for nodes lo < hi set when they can see each other
//              (Path_NodesVisible iw4sp+0x1a97f0); ceil(n*(n-1)/8) bytes.
//   nodeTree   kd-tree on X/Y (axis < 0 = leaf with node indices); child[0] holds nodes below
//              dist, child[1] above (Path_NodesInCylinder iw4sp+0x1a9630); <= 4 nodes per leaf.
#include <cstdint>
#include <string>
#include <vector>

class Document;
namespace clip {
struct ClipMapData;
}

namespace pathnodes {

struct Node {
    float origin[3] = {0, 0, 0};
    float angle = 0;  // yaw, degrees
    uint32_t type = 1;  // NODE_PATHNODE
    uint16_t spawnflags = 0;
    std::string targetname, target, noteworthy, linkname;
};

struct TreeNode {
    int axis = -1;  // 0/1 split axis, -1 leaf
    float dist = 0;
    int child[2] = {-1, -1};
    std::vector<uint16_t> nodes;  // leaf
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<std::vector<std::pair<uint16_t, float>>> links;  // (node, 2D distance), sorted
    std::vector<uint8_t> vis;
    std::vector<TreeNode> tree;  // [0] = root
};

// True for the entity classes that become path nodes (and are left out of the entity lump).
bool isPathNodeClass(const std::string& classname);

// Gathers the document's path nodes, drops them to the floor, links them (box sweeps against
// the clip brushes) and builds visibility and the node tree. Problems (nodes in solid, floating,
// unlinked) go to warnings.
Graph build(const Document& doc, const clip::ClipMapData& cm, std::vector<std::string>* warnings);

}  // namespace pathnodes
