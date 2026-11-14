#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "model/xmodelexport.h"

// Turns a parsed xmodel_export into what an XModel needs (zonebuild writes it): one surface per
// material, vertices packed as GfxPackedVertex (32 bytes: xyz, binormalSign, color, half2 uv,
// PackedUnitVec normal/tangent), u16 triangles wound like the game's world surfaces (clockwise
// seen from the front).
//
// Single-bone models are rigid (one XRigidVertList). Multi-bone models are skinned the way stock
// ones are (RE'd from viewmodel_base_viewhands): bones ordered roots first and parents before
// children, parentList = index - parent, local quats (int16 /32767) and trans per non-root bone,
// baseMat = the model-space bind pose, vertices in model space grouped by influence count (1-4)
// with vertsBlend per vertex: [b0], [b0 b1 w1], [b0 b1 w1 b2 w2], [b0 b1 w1 b2 w2 b3 w3] where
// b = bone * 64 and w = weight * 65536 (b0's weight is implied).
namespace modelbuild {

using Quat = std::array<float, 4>;  // x y z w
using V3 = std::array<float, 3>;
Quat quatFromAxes(const float axis[3][3]);  // X/Y/Z axis rows (as exported) -> rotation
Quat quatMul(const Quat& a, const Quat& b);
Quat quatConj(const Quat& q);
V3 quatRotate(const Quat& q, const V3& v);

struct PackedVertex {
    float xyz[3];
    float binormalSign;
    uint32_t color;     // GfxColor, bytes r g b a
    uint32_t texCoord;  // half u (low) | half v (high)
    uint32_t normal;    // PackedUnitVec
    uint32_t tangent;
};
static_assert(sizeof(PackedVertex) == 32, "GfxPackedVertex");

struct Surface {
    int material = 0;  // into Model::materials
    std::vector<PackedVertex> verts;
    std::vector<uint16_t> tris;  // 3 per triangle
    // Skinned surfaces: verts ordered by influence count; blendCounts = how many with 1..4.
    uint16_t blendCounts[4] = {};
    std::vector<uint16_t> blend;
    uint32_t partBits[6] = {};
};

struct Material {
    std::string name;     // as exported
    std::string texture;  // exported color texture path ("" if none)
};

struct Bone {
    std::string name;
    int parent = -1;  // index in Model::bones (after reordering), -1 for roots
    Quat bindQuat{0, 0, 0, 1};
    V3 bindTrans{};  // model-space bind pose
    Quat localQuat{0, 0, 0, 1};
    V3 localTrans{};  // relative to the parent
    // What the XModel stores as the bone's trans. The game ADDS it to the animation's
    // translation (skeleton build, iw4mp 0x2ada80: parent + rot(parent, scale*anim + modelTrans)),
    // so viewmodels store zeros like stock viewhands (weapon anims carry full offsets).
    V3 modelTrans{};
    float boundsMid[3] = {}, boundsHalf[3] = {}, radiusSquared = 0;  // bone-space bounds of its verts
};

struct Model {
    std::string name;      // asset name (file stem)
    std::string rootBone;  // first bone's name
    std::vector<Bone> bones;
    int numRootBones = 1;
    bool skinned = false;  // more than one bone
    bool viewmodel = false;  // name starts with viewhands/viewmodel: zero model trans
    std::vector<Surface> surfaces;
    std::vector<Material> materials;
    float mins[3] = {}, maxs[3] = {};
    float radius = 0;
    std::vector<std::string> warnings;
};

// Per-model compile options, mostly from the weapons that use the model.
struct BuildOptions {
    std::vector<std::string> hideBones;  // drop faces on these bones (and their children); lowercase
    bool dropUntextured = false;         // drop faces whose material has no texture (BO1 decal slots)
    bool zeroTrans = false;              // store zero bone trans (weapon viewmodels; see Bone::modelTrans)
};

bool build(const xmodelexport::Model& src, const std::string& name, Model& out, std::string& error,
           const BuildOptions& options = {});

}  // namespace modelbuild
