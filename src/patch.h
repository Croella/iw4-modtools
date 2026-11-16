#pragma once
#include <functional>
#include <string>
#include <vector>
#include "brush.h"

class Document;
struct Entity;

// Patches (curved / mesh surfaces) as Radiant writes them inside an entity: a raw block
//   {
//    mesh | curve
//    {
//     [contents nonColliding;] [toolFlags ...;]
//     <material>
//     <lightmap material>
//     [smoothing <name>]
//     <rows> <cols> <lightmap sample size> 8
//     (
//      v x y z [c r g b a] t u v [lu lv]
//      ... cols vertices
//     )
//     ... rows groups
//    }
//   }
// "mesh" vertices are the surface itself (a grid); "curve" vertices are biquadratic Bezier control
// points (rows and cols odd, 3x3 per section). t u v are texture coordinates times the texture's
// size (texels), as Radiant stores them.
//
// Winding (unverified in-game): triangles (r+1,c) (r,c+1) (r,c) and (r+1,c-1) (r+1,c) (r,c) are
// taken as clockwise seen from the front, so the front faces along cross(d/dcol, d/drow).
namespace patch {

struct Vert {
    Vec3 p;
    double st[2] = {0, 0};
    uint8_t rgba[4] = {255, 255, 255, 255};
};

struct Patch {
    bool curve = false;
    std::string material;
    int rows = 0, cols = 0;
    std::vector<Vert> verts;  // rows * cols, row-major
    bool nonColliding = false;
    bool hasColor = false;
    const Vert& at(int r, int c) const { return verts[static_cast<size_t>(r) * cols + c]; }
};

// Parses one raw block (Entity::rawBlocks). False (with a reason) if it isn't a patch.
bool parse(const std::string& block, Patch& out, std::string* error = nullptr);

// A tessellated patch: a w x h grid (w = along columns), positions, smooth front normals,
// normalized texture coordinates and colours.
struct Grid {
    int w = 0, h = 0;
    std::vector<Vec3> p, n;
    std::vector<double> uv;  // 2 per vertex
    std::vector<uint32_t> color;  // GfxColor (r g b a bytes)
    size_t at(int r, int c) const { return static_cast<size_t>(r) * w + c; }
};

// Texture size (texels) for a material, to normalize the stored texture coordinates.
using TexSizeFn = std::function<void(const std::string& material, double& w, double& h)>;

// maxSubdiv: curve sections are split into at most this many segments per direction (fewer where
// the curve is flat: error under ~0.5 units). Mesh patches are used as they are.
Grid tessellate(const Patch& p, const TexSizeFn& texSize, int maxSubdiv = 16);

// Render faces: one per grid quad (two triangles where a quad isn't planar). Faces carry their
// own texture coordinates, normals and colours (Face::uvs / vnormals / colors).
std::vector<Face> faces(const Grid& g, const std::string& material);

// Collision: a thin prism brush (thickness units behind the surface) per quad / triangle of a
// coarser tessellation, with the patch's material.
std::vector<Brush> collisionBrushes(const Patch& p, double thickness = 4, int maxSubdiv = 4);

// A brush face plane through 3 points, oriented so Brush::rebuild gives it normal n (outward).
Face planeFace(const Vec3& n, double dist, const std::string& material);

// Every patch in an entity's raw blocks.
std::vector<Patch> entityPatches(const Entity& e);

}  // namespace patch
