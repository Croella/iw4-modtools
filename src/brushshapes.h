#pragma once
#include <string>
#include <vector>
#include <array>

#include "brush.h"

// Shaped brushes (Radiant's "Brush > Cylinder / Cone / ..."), each fitted to a box. Convex shapes
// are one brush; stairs, arches, pipes and rooms are several. Z is up; ramps and stairs rise
// toward +X, arches span X and are extruded along Y.
namespace shapes {

enum class Shape { Cylinder, Cone, Pyramid, Wedge, Sphere, Stairs, Arch, Pipe, Room };
inline constexpr Shape kAll[] = {Shape::Cylinder, Shape::Cone, Shape::Pyramid, Shape::Wedge, Shape::Sphere,
                                 Shape::Stairs,   Shape::Arch, Shape::Pipe,    Shape::Room};
const char* name(Shape s);
const char* tooltip(Shape s);

struct Params {
    int sides = 8;           // cylinder / cone / sphere / pipe / arch segments
    int steps = 8;           // stairs
    double thickness = 16;   // arch / pipe / room walls
};

// The brushes for a shape filling [mins, maxs], every face using `material`. Empty if the box is
// too small for the shape (e.g. walls thicker than the box).
std::vector<Brush> make(Shape s, const Vec3& mins, const Vec3& maxs, const Params& p, const std::string& material);

// A convex brush from its face polygons (any winding; oriented from the polygons' centre).
bool convexBrush(const std::vector<std::vector<Vec3>>& polygons, const std::string& material, Brush& out);

// ---- pen tool ----
using Point2 = std::array<double, 2>;
// A closed outline split into convex polygons (counter-clockwise): one piece if it's convex,
// else ear-clipped and the triangles greedily merged back while they stay convex
// (Hertel-Mehlhorn: at most 4x the minimum number of pieces). Duplicate and collinear points are
// dropped. Empty, with an error, for self-intersecting or degenerate outlines.
std::vector<std::vector<Point2>> convexPieces(std::vector<Point2> outline, std::string& error);
// The outline (in a view plane: world axes u, v) extruded along world axis w from w0 to w1.
std::vector<Brush> fromOutline(const std::vector<Point2>& outline, int u, int v, int w, double w0, double w1,
                               const std::string& material, std::string& error);

}  // namespace shapes
