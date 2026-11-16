#pragma once
#include <string>
#include <vector>
#include "brush.h"
#include "patch.h"

class Document;
struct Entity;

// What the map compiler (and the lighting bake) treat as world geometry besides worldspawn's
// brushes, decided in one place so the clipMap, GfxWorld, bake and entity lump agree:
//   - patches in worldspawn (patch.h): drawn + lit like brush faces, collide as thin prisms
//   - brush models: brush entities other than triggers / editor volumes (script_brushmodel,
//     func_*...). Each becomes cmodel / GfxBrushModel "*N" (N from 1, entity order), its
//     geometry stored relative to the entity origin (stock: mp_rust, boneyard), lit at its
//     placed position with the sun baked in (stock brush-model surfaces use primary light 0)
//   - static models: misc_model entities -> GfxWorld static models (+ clipMap static models for
//     bullet collision, player clip brushes from their hull)
namespace worldgeom {

bool isBrushModelClass(const std::string& classname);
// Entity indices compiled as brush models, in order (model "*1" is the first).
std::vector<size_t> brushModelEntities(const Document& doc);
// A brush model's origin: its "origin" key, else the rounded centre of its brushes.
Vec3 brushModelOrigin(const Entity& e);

// Worldspawn's patches, and their tessellated render faces.
std::vector<patch::Patch> worldPatches(const Document& doc);
std::vector<Face> worldPatchFaces(const Document& doc, const patch::TexSizeFn& texSize = nullptr);

// Placement of a model entity: origin, CoD AnglesToAxis rows (forward, left, up) from "angles"
// (pitch yaw roll) / "angle", and "modelscale".
struct Placement {
    Vec3 origin;
    Vec3 axis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double scale = 1;
    Vec3 apply(const Vec3& local) const { return origin + (axis[0] * local.x + axis[1] * local.y + axis[2] * local.z) * scale; }
    Vec3 applyDir(const Vec3& local) const { return axis[0] * local.x + axis[1] * local.y + axis[2] * local.z; }
};
Placement entityPlacement(const Entity& e);

// A convex hull approximation (26-DOP: the extremes along the axes, edge and corner diagonals)
// of points, as a brush with `material` on every side. False if the points are flat.
bool dopBrush(const std::vector<Vec3>& pts, const std::string& material, Brush& out);

}  // namespace worldgeom
