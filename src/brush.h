#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "math3d.h"

// Texture projection as stored in a map face: size in world units, shift, rotation.
struct TexDef {
    double w = 256, h = 256, sx = 0, sy = 0, rot = 0;
};

struct Face {
    Vec3 p[3];             // plane points, clockwise when viewed from outside
    std::string material;
    TexDef tex;
    std::string tail;      // everything after the texdef (lightmap fields), kept verbatim
    Vec3 normal;
    double dist = 0;
    std::vector<Vec3> winding;
    // Explicit per-winding-vertex data (patch faces, see patch.h); empty for brush faces, which
    // use the texture projection (faceUV) and the flat normal.
    std::vector<float> uvs;         // 2 per vertex
    std::vector<Vec3> vnormals;     // smooth normals
    std::vector<uint32_t> colors;   // GfxColor bytes r g b a
};

// A convex solid defined by the intersection of its face planes.
struct Brush {
    std::vector<std::string> attrs;  // non-face lines such as "contents detail;", kept verbatim
    std::vector<Face> faces;
    AABB bounds;

    bool rebuild();  // recomputes planes and windings; false if the solid is degenerate
    void translate(const Vec3& d);
    // Moves every face pointing along +/- axis by amount. Returns false, leaving the
    // brush unchanged, if that would collapse it.
    bool moveFaces(int axis, int sign, double amount);
    void setMaterial(const std::string& name);

    // Two-sided: the brush's faces are also drawn from behind (compiled as a mirrored copy), so a
    // volume such as a dust brush shows from inside. The game culls back faces otherwise. Stored
    // as the brush attribute line kTwoSidedAttr, which .map files keep verbatim.
    bool twoSided() const;
    void setTwoSided(bool on);

    // Surface type override (footsteps, bullet impacts, bullettrace "surfacetype", AI groundtype):
    // one of surfaceTypeNames(), or "" to use the material's own (the stock material's type, else
    // "default"). Stored as the brush attribute line "iw4r_surface <name>;".
    std::string surfaceType() const;
    void setSurfaceType(const std::string& name);
};

inline constexpr const char* kTwoSidedAttr = "iw4r_twosided;";
// IW4's surface types in game order (index = the type in clipMap surfaceFlags >> 20).
const std::vector<std::string>& surfaceTypeNames();
// Index of a surface type name, or -1 for "" / unknown.
int surfaceTypeIndex(const std::string& name);
// The face seen from behind: winding reversed, normal and plane flipped.
Face mirroredFace(const Face& f);

Brush makeBox(const Vec3& mins, const Vec3& maxs, const std::string& material);
bool rayBrush(const Brush& b, const Vec3& origin, const Vec3& dir, double& tOut, int& faceOut);
// Texture coordinates for a point on a face.
void faceUV(const Face& f, const Vec3& p, float& u, float& v);
